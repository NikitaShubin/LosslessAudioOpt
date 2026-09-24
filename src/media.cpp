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

#ifdef _WIN32
#include <windows.h>
#endif

namespace media {

namespace json = nlohmann;

namespace {
// Потоковый читатель WAV. Под Windows открывает дескриптор с FILE_SHARE_DELETE:
// иначе DeleteFile (util::remove_file) получает STATUS_SHARING_VIOLATION от
// нашего же живого handle чтения, и свежие .dec.wav «залипают» под wine.
struct WavReader {
    std::ifstream fs;
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#endif

    ~WavReader() {
#ifdef _WIN32
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#endif
    }
    bool open(const std::string& p) {
#ifdef _WIN32
        h = CreateFileW(util::u2w(p).c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h != INVALID_HANDLE_VALUE;
#else
        fs.open(std::filesystem::u8path(p), std::ios::binary);
        return fs.good();
#endif
    }
    bool seek(uint64_t pos) {
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)pos;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
#else
        fs.clear();
        fs.seekg((std::streamoff)pos);
        return fs.good();
#endif
    }
    size_t read(char* buf, size_t n) {
#ifdef _WIN32
        DWORD rd = 0;
        if (n > 0x7FFFFFFF) n = 0x7FFFFFFF;
        if (!ReadFile(h, buf, (DWORD)n, &rd, nullptr)) return 0;
        return (size_t)rd;
#else
        fs.read(buf, (std::streamsize)n);
        return (size_t)fs.gcount();
#endif
    }
};
}  // namespace

static uint32_t rd32le(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Множество lossless-кодеков строится из formats/*.json (флаг "lossless"),
// а не хардкодится: для каждого формата берутся id, engine_codec (имя, которое
// отдаёт ffprobe для ffmpeg-кодеков) и явный ffprobe_codec (для binary-кодеков,
// где ffprobe-имя отличается от id, напр. Monkey's Audio -> "ape").
// Кэшируется, т.к. вызывается часто (пробы, ранжирование).
static const std::set<std::string>& lossless_codec_set() {
    static const std::set<std::string> ls = [] {
        std::set<std::string> s;
        for (const auto& f : config::load_all()) {
            if (!f.lossless) continue;
            if (!f.id.empty()) s.insert(f.id);
            if (!f.engine_codec.empty()) s.insert(f.engine_codec);
            if (!f.ffprobe_codec.empty()) s.insert(f.ffprobe_codec);
        }
        return s;
    }();
    return ls;
}

bool codec_is_lossless(const std::string& codec_name) {
    std::string c = codec_name;
    if (c.empty()) return true;  // неизвестный кодек — не блокируем обработку
    if (c.compare(0, 4, "pcm_") == 0 || c.compare(0, 4, "dsd_") == 0) return true;
    return lossless_codec_set().count(c) != 0;
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

// Удаляет JSON-скелет ffprobe из вывода при ошибке: строки "{", "}" и пустые.
// При -print_format json ffprobe печатает {\r\n\r\n}\r\n на stdout даже при
// ошибке; stdout и stderr сливаются в один pipe, и скобки попадают в error.
static std::string strip_ffprobe_json(const std::string& s) {
    std::string out;
    for (const auto& line : util::split(s, '\n')) {
        std::string trimmed = util::trim(line);
        if (trimmed == "{" || trimmed == "}" || trimmed.empty()) continue;
        if (!out.empty()) out += '\n';
        out += line;
    }
    return out;
}

Probe probe_file(const std::string& path, const std::string& ffprobe,
                   const std::atomic<bool>* kill) {
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
                               120, "", {}, kill);
    if (!r.started) {
        p.error = i18n::str("could not launch ffprobe: ") + r.error;
        return p;
    }
    if (r.exit_code != 0) {
        std::string cleaned = strip_ffprobe_json(r.output);
        p.error = i18n::fmt("ffprobe: code %d", r.exit_code) +
                  (cleaned.empty() ? "" : ": " + util::trim(cleaned));
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
                   const std::string& ffmpeg, int bits, std::string* err,
                   const std::atomic<bool>* kill, const proc::OutputMonitor* mon) {
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
                               600, "", mon ? *mon : proc::OutputMonitor{}, kill);
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
static bool wav_data_chunk_stream(WavReader& f, uint64_t* off, uint64_t* sz) {
    char hdr[12];
    if (!f.seek(0) || f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;
    uint64_t o = 12;
    while (true) {
        char ch[8];
        if (!f.seek(o) || f.read(ch, 8) != 8) return false;
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

namespace {
struct WavChunkView {
    std::string id;
    uint64_t off = 0;  // смещение тела чанка
    uint32_t len = 0;  // длина тела (без выравнивания)
};
// Пытается прочитать заголовки всех чанков до data (и сам data). Если data найден,
// останавливается. Возвращает false при повреждённом заголовке.
bool scan_wav_chunks(WavReader& f, std::vector<WavChunkView>* out, uint64_t* data_body,
                     uint32_t* data_len) {
    char hdr[12];
    if (!f.seek(0) || f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;
    uint64_t o = 12;
    *data_body = 0;
    *data_len = 0;
    while (true) {
        char ch[8];
        if (!f.seek(o) || f.read(ch, 8) != 8) return false;
        uint32_t chsz = rd32le((uint8_t*)ch + 4);
        WavChunkView c;
        c.id.assign(ch, ch + 4);
        c.off = o + 8;
        c.len = chsz;
        out->push_back(c);
        if (c.id == "data") {
            *data_body = c.off;
            *data_len = chsz;
            return true;
        }
        if (chsz == 0) return false;
        o += 8 + chsz + (chsz & 1);
    }
}
}  // namespace

bool canonicalize_wav(const std::string& path, std::string* err) {
    WavReader r;
    if (!r.open(path)) {
        *err = i18n::str("could not open the WAV to normalize");
        return false;
    }
    std::vector<WavChunkView> ch;
    uint64_t data_body = 0;
    uint32_t data_len = 0;
    if (!scan_wav_chunks(r, &ch, &data_body, &data_len)) {
        *err = i18n::str("not a valid WAV header");
        return false;
    }

    // fmt обязателен; остальные чанки (после fmt и до data) — посторонние.
    const WavChunkView* fmt = nullptr;
    for (const auto& c : ch)
        if (c.id == "fmt " && !fmt) fmt = &c;
    if (!fmt || fmt->len < 16) {
        *err = i18n::str("WAV has no valid fmt chunk");
        return false;
    }
    if (!fmt->len || data_len == 0) {
        *err = i18n::str("WAV has no data chunk");
        return false;
    }

    bool fmt_first = ch[0].id == "fmt ";
    bool has_fact = false;
    for (const auto& c : ch)
        if (c.id == "fact") has_fact = true;

    // Уже совместимый заголовок: fmt идёт первым, нет fact. Прочие кодеки
    // (flac, tak, wavpack...) спокойно переживают чанки LIST и т.п., поэтому
    // перезапись не нужна — сохраняем файл как есть.
    if (fmt_first && !has_fact) return true;

    // Читаем PCM-параметры из fmt (первые 16 байт каноничны и для WAVEFORMATEX).
    char fbuf[16];
    if (!r.seek(fmt->off) || r.read(fbuf, 16) != 16) {
        *err = i18n::str("could not read the fmt chunk");
        return false;
    }
    uint16_t fmt_tag = (uint16_t)((uint8_t)fbuf[0] | ((uint8_t)fbuf[1] << 8));
    uint16_t chans = (uint16_t)((uint8_t)fbuf[2] | ((uint8_t)fbuf[3] << 8));
    uint32_t rate = rd32le((uint8_t*)fbuf + 4);
    uint16_t bits = (uint16_t)((uint8_t)fbuf[14] | ((uint8_t)fbuf[15] << 8));
    if (fmt_tag != 1 || chans == 0 || rate == 0 || bits == 0) {
        *err = i18n::str("unsupported WAV format for normalization");
        return false;
    }
    // Записываем канонический заголовок во временный файл, затем меняем на месте.
    std::string tmp = path + ".canon.tmp";
    util::remove_file(tmp);
    {
        std::ofstream w(std::filesystem::u8path(tmp), std::ios::binary);
        if (!w) {
            *err = i18n::str("could not create the temporary WAV");
            return false;
        }
        char fh[16];
        memcpy(fh, fbuf, 16);  // те же параметры PCM
        uint32_t riff = 4 + (8 + 16) + (8 + data_len);
        const char* id_fmt = "fmt ";
        const char* id_data = "data";
        const char* id_riff = "RIFF";
        const char* id_wave = "WAVE";
        w.write(id_riff, 4);
        w.write((const char*)&riff, 4);
        w.write(id_wave, 4);
        w.write(id_fmt, 4);
        uint32_t fmtsz = 16;
        w.write((const char*)&fmtsz, 4);
        w.write(fh, 16);
        w.write(id_data, 4);
        w.write((const char*)&data_len, 4);

        // Копируем PCM-данные блоками, без изменений.
        constexpr size_t kChunk = 1u << 20;
        std::vector<char> buf(kChunk);
        uint64_t left = data_len;
        uint64_t pos = data_body;
        while (left > 0) {
            size_t n = left < kChunk ? (size_t)left : kChunk;
            if (!r.seek(pos) || r.read(buf.data(), n) != n) {
                w.close();
                util::remove_file(tmp);
                *err = i18n::str("WAV data chunk extends beyond the file");
                return false;
            }
            w.write(buf.data(), n);
            left -= n;
            pos += n;
        }
        w.close();
        if (!w) {
            util::remove_file(tmp);
            *err = i18n::str("could not write the normalized WAV");
            return false;
        }
    }

    util::ReplaceResult rep = util::replace_file(path, tmp, path);
    if (!rep.ok) {
        util::remove_file(tmp);
        *err = i18n::str("could not replace the WAV: ") +
               (rep.error.empty() ? i18n::str("unknown") : rep.error);
        return false;
    }
    return true;
}

bool wav_data_compare(const std::string& a, const std::string& b, std::string* err) {
    WavReader fa, fb;
    if (!fa.open(a) || !fb.open(b)) {
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
    if (!fa.seek(ao) || !fb.seek(bo)) {
        *err = i18n::str("could not find the data chunk in the WAV");
        return false;
    }
    constexpr size_t kChunk = 1u << 20;  // 1 МБ
    std::vector<char> ba(kChunk), bb(kChunk);
    uint64_t left = asz;
    while (left > 0) {
        size_t n = left < kChunk ? (size_t)left : kChunk;
        size_t ga = fa.read(ba.data(), n);
        size_t gb = fb.read(bb.data(), n);
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
