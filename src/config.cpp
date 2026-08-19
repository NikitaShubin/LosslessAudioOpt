#include "config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "i18n.h"
#include "util.h"

namespace fs = std::filesystem;
namespace json = nlohmann;

namespace config {

// JSON \u00a9 декодируется nlohmann/json как UTF-8 \xc2\xa9 (2 байта),
// а MP4 4CC ключи требуют ровно 4 байт с raw \xa9.  Нормализуем: любую
// UTF-8 последовательность 2 байта для символов U+0080..U+00BF
// (0xc2 0x80..0xbf) заменяем одним байтом.
static std::string normalize_4cc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if ((unsigned char)s[i] == 0xc2 && i + 1 < s.size() &&
            (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0xbf) {
            out.push_back(s[++i]);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static const char* REQUIRED_FIELDS[] = {"id", "name", "extension", "engine", "downloads", "encode", "decode", "tag", "caps"};
static const char* TAG_CAPS[] = {"text", "pictures", "lyrics", "cue_sheet", "replay_gain", "chapters"};
static const char* DOWNLOAD_KINDS[] = {"archive", "extract7z"};

std::string formats_dir() { return util::join_path(util::exe_dir(), "formats"); }
std::string bin_dir() { return util::join_path(util::exe_dir(), "bin"); }

std::string load_settings_lang() {
    std::string path = util::join_path(util::exe_dir(), "llao.json");
    std::string text = util::read_text(path);
    if (text.empty()) return "auto";
    try {
        json::json j = json::json::parse(text);
        if (j.is_object() && j.contains("language") && j.at("language").is_string()) {
            return j.at("language").get<std::string>();
        }
    } catch (...) {
    }
    return "auto";
}

static std::string err_str(const std::string& fmt_id, const std::string& msg) {
    return "[" + fmt_id + "] " + msg;
}

static bool has(const json::json& j, const char* key) { return j.contains(key); }

static std::string get_str(const json::json& j, const char* key, const std::string& def = "") {
    if (!has(j, key) || !j.at(key).is_string()) return def;
    return j.at(key).get<std::string>();
}

static std::vector<std::string> get_str_list(const json::json& j, const char* key) {
    std::vector<std::string> out;
    if (!has(j, key) || !j.at(key).is_array()) return out;
    for (const auto& e : j.at(key)) {
        if (e.is_string()) out.push_back(e.get<std::string>());
    }
    return out;
}

static std::vector<Variant> parse_variants(const json::json& v) {
    std::vector<Variant> out;
    for (const auto& item : v) {
        Variant var;
        var.id = item.value("id", "");
        for (const auto& a : item.value("args", json::json::array())) {
            if (a.is_string()) var.args.push_back(a.get<std::string>());
        }
        var.note = item.value("note", "");
        out.push_back(std::move(var));
    }
    return out;
}

static bool is_known_os(const std::string& os) {
    return os == "any" || os == "linux" || os == "windows" || os == "macos" || os == "freebsd";
}

Format validate(const json::json& data) {
    std::string fmt_id = data.value("id", std::string("?"));
    for (const char* k : REQUIRED_FIELDS) {
        if (!data.contains(k)) throw Error(err_str(fmt_id, i18n::fmt("required field '%s' is missing", std::string(k).c_str())));
    }

    Format f;
    f.id = data.at("id").get<std::string>();
    f.name = data.at("name").get<std::string>();
    f.extension = data.at("extension").get<std::string>();
    f.homepage = get_str(data, "homepage");
    f.enabled = data.value("enabled", true);
    f.notes = get_str(data, "notes");

    // engine
    const auto& eng = data.at("engine");
    f.engine_kind = eng.value("kind", "");
    if (f.engine_kind != "binary" && f.engine_kind != "ffmpeg") {
        throw Error(err_str(fmt_id, i18n::fmt("engine.kind must be 'binary' or 'ffmpeg', got: '%s'", f.engine_kind.c_str())));
    }
    f.engine_executable = get_str(eng, "executable");
    f.engine_decoder_executable = get_str(eng, "decoder_executable");
    f.engine_codec = get_str(eng, "codec");
    f.engine_container = get_str(eng, "container");
    if (f.engine_kind == "binary" && f.engine_executable.empty()) {
        throw Error(err_str(fmt_id, i18n::str("engine.executable is required for kind=binary")));
    }
    if (f.engine_kind == "ffmpeg") {
        if (f.engine_codec.empty()) throw Error(err_str(fmt_id, i18n::str("engine.codec is required for kind=ffmpeg")));
        if (f.engine_container.empty()) throw Error(err_str(fmt_id, i18n::str("engine.container is required for kind=ffmpeg")));
    }
    if (f.engine_decoder_executable.empty()) f.engine_decoder_executable = f.engine_executable;

    // downloads
    if (!data.at("downloads").is_array() || data.at("downloads").empty()) {
        throw Error(err_str(fmt_id, i18n::str("downloads must be a non-empty list")));
    }
    size_t i = 0;
    for (const auto& dl : data.at("downloads")) {
        std::string tag = fmt_id + " downloads[" + std::to_string(i) + "]";
        if (!dl.is_object()) throw Error(err_str(fmt_id, i18n::fmt("downloads[%s]: not an object", std::to_string(i).c_str())));
        DownloadEntry e;
        e.os = get_str(dl, "os");
        e.kind = get_str(dl, "kind");
        if (e.os.empty()) throw Error(err_str(tag, i18n::str("missing field 'os'")));
        if (!is_known_os(e.os)) throw Error(err_str(tag, i18n::fmt("unknown OS '%s'", e.os.c_str())));
        if (e.kind.empty()) throw Error(err_str(tag, i18n::str("missing field 'kind'")));
        if (std::find(std::begin(DOWNLOAD_KINDS), std::end(DOWNLOAD_KINDS), e.kind) == std::end(DOWNLOAD_KINDS)) {
            throw Error(err_str(tag, i18n::fmt("unknown kind '%s'", e.kind.c_str())));
        }
        if (!dl.contains("url")) throw Error(err_str(tag, i18n::str("missing field 'url'")));
        if (dl.at("url").is_string()) e.url = dl.at("url").get<std::string>();
        if (e.url.empty()) throw Error(err_str(tag, i18n::fmt("kind=%s needs a url", e.kind.c_str())));
        e.file_glob = get_str(dl, "file_glob");
        e.notes = get_str(dl, "notes");
        if (dl.contains("checksum") && dl.at("checksum").is_object()) {
            const auto& cs = dl.at("checksum");
            std::string type = get_str(cs, "type");
            std::string val = get_str(cs, "value");
            if (!type.empty() && type != "sha256") throw Error(err_str(tag, i18n::str("only sha256 is supported for now")));
            e.checksum = val;
        }
        if (dl.contains("files") && dl.at("files").is_array()) {
            e.files = get_str_list(dl, "files");
        }
        f.downloads.push_back(std::move(e));
        i++;
    }

    // encode
    const auto& enc = data.at("encode");
    f.encode_cmd = get_str_list(enc, "cmd");
    if (f.encode_cmd.empty()) throw Error(err_str(fmt_id, i18n::str("encode.cmd is required (non-empty array of strings)")));
    if (!enc.contains("variants") || !enc.at("variants").is_array() || enc.at("variants").empty()) {
        throw Error(err_str(fmt_id, i18n::str("encode.variants is required (non-empty list)")));
    }
    f.variants = parse_variants(enc.at("variants"));

    // decode
    const auto& dec = data.at("decode");
    f.decode_cmd = get_str_list(dec, "cmd");
    if (f.decode_cmd.empty()) throw Error(err_str(fmt_id, i18n::str("decode.cmd is required (non-empty array of strings)")));

    // verify
    if (data.contains("verify") && data.at("verify").is_object()) {
        const auto& v = data.at("verify");
        f.verify_kind = v.value("kind", "none");
        f.verify_cmd = get_str_list(v, "cmd");
    } else {
        f.verify_kind = "none";
    }

    // tag
    const auto& tag = data.at("tag");
    f.tag_system = get_str(tag, "system");
    f.tag_writer = get_str(tag, "writer");
    if (tag.contains("capabilities") && tag.at("capabilities").is_object()) {
        for (const char* k : TAG_CAPS) {
            if (!tag.at("capabilities").contains(k)) {
                throw Error(err_str(fmt_id, i18n::fmt("tag.capabilities.%s is required (bool)", k)));
            }
            f.tag_caps[k] = tag.at("capabilities").at(k).get<bool>();
        }
    } else {
        throw Error(err_str(fmt_id, i18n::str("tag.capabilities is required")));
    }

    // Data-driven tag fields (все опциональны)
    f.tag_write_method = tag.value("write_method", "");
    f.tag_native_reader = tag.value("native_reader", false);
    f.tag_validate_skip_ffprobe = tag.value("validate_skip_ffprobe", false);
    if (tag.contains("numeric_fields") && tag.at("numeric_fields").is_array())
        for (const auto& v : tag.at("numeric_fields"))
            f.tag_numeric_fields.push_back(v.get<std::string>());
    if (tag.contains("key_map") && tag.at("key_map").is_object())
        for (auto& [k, v] : tag.at("key_map").items())
            f.tag_key_map[k] = normalize_4cc(v.get<std::string>());
    if (tag.contains("reverse_key_map") && tag.at("reverse_key_map").is_object())
        for (auto& [k, v] : tag.at("reverse_key_map").items())
            f.tag_reverse_key_map[normalize_4cc(k)] = normalize_4cc(v.get<std::string>());
    if (tag.contains("write_constraints") && tag.at("write_constraints").is_object()) {
        const auto& wc = tag.at("write_constraints");
        if (wc.contains("allowed_keys") && wc.at("allowed_keys").is_array())
            for (const auto& v : wc.at("allowed_keys"))
                f.tag_allowed_keys.push_back(v.get<std::string>());
        f.tag_replaygain_allowed = wc.value("replaygain_allowed", true);
        f.tag_pictures_allowed = wc.value("pictures_allowed", true);
        f.tag_cue_sheet_allowed = wc.value("cue_sheet_allowed", true);
        f.tag_write_supported = wc.value("write_supported", true);
    }

    // caps
    const auto& caps = data.at("caps");
    if (!caps.contains("channels") || !caps.contains("bit_depth")) {
        throw Error(err_str(fmt_id, i18n::str("caps.channels and caps.bit_depth are required")));
    }
    const auto& ch = caps.at("channels");
    if (!ch.is_object() || !ch.contains("min") || !ch.contains("max") || !ch.at("min").is_number_integer() || !ch.at("max").is_number_integer()) {
        throw Error(err_str(fmt_id, i18n::str("caps.channels must be {min: int, max: int}")));
    }
    f.channels_min = ch.at("min").get<int>();
    f.channels_max = ch.at("max").get<int>();
    if (!caps.at("bit_depth").is_array() || caps.at("bit_depth").empty()) {
        throw Error(err_str(fmt_id, i18n::str("caps.bit_depth must be a non-empty list of integers")));
    }
    for (const auto& b : caps.at("bit_depth")) {
        if (!b.is_number_integer()) throw Error(err_str(fmt_id, i18n::str("caps.bit_depth must contain only integers")));
        f.bit_depth.push_back(b.get<int>());
    }
    if (caps.contains("sample_rate") && caps.at("sample_rate").is_object()) {
        const auto& sr = caps.at("sample_rate");
        f.has_sample_rate = true;
        f.sample_rate_min = sr.value("min", 0);
        f.sample_rate_max = sr.value("max", 0);
    }

    // cli_check (опционально)
    if (data.contains("cli_check") && data.at("cli_check").is_object()) {
        const auto& cc = data.at("cli_check");
        f.cli_check.present = true;
        f.cli_check.cmd = get_str_list(cc, "cmd");
        f.cli_check.expect = get_str_list(cc, "expect");
        if (f.cli_check.cmd.empty()) f.cli_check.cmd = {"--help"};
    }

    return f;
}

std::vector<Format> load_all() {
    std::vector<Format> out;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::u8path(formats_dir()), ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path().u8string());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream fh(path, std::ios::binary);
        if (!fh) throw Error(i18n::str("could not open ") + path);
        json::json data;
        try {
            fh >> data;
        } catch (const nlohmann::detail::parse_error& exc) {
            throw Error("[" + util::base_name(path) + "] " + i18n::str("invalid JSON: ") + exc.what());
        }
        Format f = validate(data);
        out.push_back(std::move(f));
    }
    return out;
}

const Format& load_one(const std::vector<Format>& all, const std::string& id) {
    for (const auto& f : all) {
        if (f.id == id) return f;
    }
    throw Error(i18n::fmt("format '%s' not found in %s", id.c_str(), formats_dir().c_str()));
}

}  // namespace config
