#include "media.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

#include "config.h"
#include "i18n.h"
#include "proc.h"
#include "util.h"

namespace media {

namespace json = nlohmann;

static uint32_t rd32le(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool codec_is_lossless(const std::string& codec_name) {
    std::string c = codec_name;
    if (c.empty()) return true;  // неизвестный кодек — не блокируем обработку
    if (c.compare(0, 4, "pcm_") == 0 || c.compare(0, 4, "dsd_") == 0) return true;
    static const std::set<std::string> ls = {
        "flac", "alac", "wavpack", "tta", "tak", "ape", "ofr", "la",
        "mp4als", "truehd", "mlp", "wmalossless",
    };
    return ls.count(c) != 0;
}

bool Probe::is_lossless() const {
    return codec_is_lossless(codec_name);
}

std::string find_ffprobe() {
#ifdef _WIN32
    std::string local = util::join_path(util::join_path(config::bin_dir(), "ffmpeg"), "ffprobe.exe");
    if (util::file_exists(local)) return local;
    return util::find_in_path("ffprobe.exe");
#else
    std::string local = util::join_path(util::join_path(config::bin_dir(), "ffmpeg"), "ffprobe");
    if (util::file_exists(local)) return local;
    return util::find_in_path("ffprobe");
#endif
}

std::string find_ffmpeg() {
#ifdef _WIN32
    std::string local = util::join_path(util::join_path(config::bin_dir(), "ffmpeg"), "ffmpeg.exe");
    if (util::file_exists(local)) return local;
    return util::find_in_path("ffmpeg.exe");
#else
    std::string local = util::join_path(util::join_path(config::bin_dir(), "ffmpeg"), "ffmpeg");
    if (util::file_exists(local)) return local;
    return util::find_in_path("ffmpeg");
#endif
}

Probe probe_file(const std::string& path, const std::string& ffprobe) {
    Probe p;
    std::string bin = ffprobe.empty() ? find_ffprobe() : ffprobe;
    if (bin.empty()) {
#ifdef _WIN32
        p.error = i18n::str("ffprobe.exe not found (bin/ffmpeg/ or PATH)");
#else
        p.error = i18n::str("ffprobe not found (bin/ffmpeg/ or PATH)");
#endif
        return p;
    }
    proc::Result r = proc::run({bin, "-v", "error", "-print_format", "json",
                                "-show_format", "-show_streams", path},
                               120);
    if (!r.started) {
        p.error = i18n::str("could not launch ffprobe: ") + r.error;
        return p;
    }
    if (r.exit_code != 0) {
        p.error = i18n::fmt("ffprobe: code %d", r.exit_code) +
                  (util::trim(r.output).empty() ? "" : ": " + util::trim(r.output));
        return p;
    }
    try {
        json::json d = json::json::parse(r.output);
        const auto& fmt = d.value("format", json::json::object());
        p.format_name = fmt.value("format_name", "");
        // ffprobe может отдавать duration/size строками (в т.ч. "N/A")
        if (fmt.contains("duration")) {
            const auto& v = fmt.at("duration");
            if (v.is_number()) p.duration = v.get<double>();
            else if (v.is_string()) p.duration = atof(v.get<std::string>().c_str());
        }
        if (fmt.contains("size")) {
            const auto& v = fmt.at("size");
            if (v.is_number_unsigned()) p.size = v.get<uint64_t>();
            else if (v.is_number()) p.size = (uint64_t)v.get<double>();
            else if (v.is_string()) p.size = (uint64_t)std::strtoull(v.get<std::string>().c_str(), nullptr, 10);
        }
        json::json tags = fmt.value("tags", json::json::object());
        if (tags.is_object()) {
            for (const auto& [k, v] : tags.items()) {
                if (v.is_string()) p.tags[k].push_back(v.get<std::string>());
            }
        }
        if (d.contains("streams") && d.at("streams").is_array()) {
            for (const auto& s : d.at("streams")) {
                int stype = 0;  // 0=нет, 1=видео, 2=аудио
                std::string ct = s.value("codec_type", "");
                if (ct == "video") stype = 1;
                else if (ct == "audio") stype = 2;
                if (stype == 1) {
                    int disp = s.value("disposition", json::json::object()).value("attached_pic", 0);
                    if (disp == 0) p.has_video = true;  // видео без attached_pic — настоящий видеопоток
                    continue;
                }
                if (stype == 2 && p.codec_name.empty()) {
                    p.codec_name = s.value("codec_name", "");
                    if (s.contains("channels")) p.channels = s.at("channels").get<int>();
                    if (s.contains("sample_rate")) {
                        const auto& v = s.at("sample_rate");
                        if (v.is_number()) p.sample_rate = v.get<int>();
                        else if (v.is_string()) p.sample_rate = atoi(v.get<std::string>().c_str());
                    }
                    int bps = 0;
                    for (const auto& k : {"bits_per_raw_sample", "bits_per_sample"}) {
                        if (s.contains(k)) {
                            const auto& v = s.at(k);
                            if (v.is_number()) bps = v.get<int>();
                            else if (v.is_string()) bps = atoi(v.get<std::string>().c_str());
                            if (bps) break;
                        }
                    }
                    p.bits_per_sample = bps;
                }
            }
        }
        p.ok = true;
    } catch (const std::exception& exc) {
        p.error = i18n::str("could not parse ffprobe output: ") + exc.what();
    }
    return p;
}

bool decode_to_wav(const std::string& input, const std::string& output_wav,
                   const std::string& ffmpeg, int bits, std::string* err) {
    std::string bin = ffmpeg.empty() ? find_ffmpeg() : ffmpeg;
    if (bin.empty()) {
#ifdef _WIN32
        *err = i18n::str("ffmpeg.exe not found (bin/ffmpeg/ or PATH)");
#else
        *err = i18n::str("ffmpeg not found (bin/ffmpeg/ or PATH)");
#endif
        return false;
    }
    std::string codec;
    if (bits <= 16) codec = "pcm_s16le";
    else if (bits <= 24) codec = "pcm_s24le";
    else codec = "pcm_s32le";
    proc::Result r = proc::run({bin, "-y", "-loglevel", "error", "-i", input,
                                "-c:a", codec, output_wav},
                               600);
    if (!r.started) {
        *err = i18n::str("could not launch ffmpeg: ") + r.error;
        return false;
    }
    if (r.exit_code != 0) {
        *err = i18n::fmt("decode: ffmpeg code %d", r.exit_code) +
               (util::trim(r.output).empty() ? "" : ": " + util::trim(r.output));
        return false;
    }
    return true;
}

// Поиск data-чанка WAV: возвращает смещение данных и их размер. Файл читается
// потоково; возвращает false, если WAV-заголовок не найден или data-чанка нет.
static bool wav_data_chunk_stream(std::ifstream& f, uint64_t* off, uint64_t* sz) {
    char hdr[12];
    f.seekg(0);
    f.read(hdr, 12);
    if (f.gcount() != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;
    uint64_t o = 12;
    while (true) {
        char ch[8];
        f.clear();
        f.seekg((std::streamoff)o);
        f.read(ch, 8);
        if (f.gcount() != 8) return false;
        uint32_t chsz = rd32le((uint8_t*)ch + 4);
        if (memcmp(ch, "data", 4) == 0) {
            *off = o + 8;
            *sz = chsz;
            return true;
        }
        if (chsz == 0) return false;
        o += 8 + chsz + (chsz & 1);
    }
}

bool wav_data_compare(const std::string& a, const std::string& b, std::string* err) {
    std::ifstream fa(std::filesystem::u8path(a), std::ios::binary);
    std::ifstream fb(std::filesystem::u8path(b), std::ios::binary);
    if (!fa || !fb) {
        *err = i18n::str("could not open the WAV for comparison");
        return false;
    }
    uint64_t ao = 0, asz = 0, bo = 0, bsz = 0;
    if (!wav_data_chunk_stream(fa, &ao, &asz) || !wav_data_chunk_stream(fb, &bo, &bsz)) {
        *err = i18n::str("could not find the data chunk in the WAV");
        return false;
    }
    if (asz == 0 || bsz == 0) {
        *err = i18n::str("could not find the data chunk in the WAV");
        return false;
    }
    if (asz != bsz) {
        *err = i18n::fmt("different amounts of audio data (%s vs %s bytes)",
                          std::to_string(asz).c_str(), std::to_string(bsz).c_str());
        return false;
    }
    fa.clear();
    fb.clear();
    fa.seekg((std::streamoff)ao);
    fb.seekg((std::streamoff)bo);
    constexpr size_t kChunk = 1u << 20;  // 1 МБ
    std::vector<char> ba(kChunk), bb(kChunk);
    uint64_t left = asz;
    while (left > 0) {
        size_t n = left < kChunk ? (size_t)left : kChunk;
        fa.read(ba.data(), (std::streamsize)n);
        size_t ga = (size_t)fa.gcount();
        fb.read(bb.data(), (std::streamsize)n);
        size_t gb = (size_t)fb.gcount();
        if (ga != n || gb != n) {
            *err = i18n::str("corrupted WAV (data chunk extends beyond the file)");
            return false;
        }
        if (memcmp(ba.data(), bb.data(), n) != 0) {
            *err = i18n::str("PCM data does not match");
            return false;
        }
        left -= n;
    }
    return true;
}

}  // namespace media
