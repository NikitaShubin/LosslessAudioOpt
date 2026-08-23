#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace config {

struct DownloadEntry {
    std::string os;
    std::string kind;               // archive | extract7z
    std::string url;
    std::string file_glob;          // archive: где в архиве бинарник (glob)
    std::string checksum;           // sha256 hex (может быть пусто)
    std::string notes;
    std::vector<std::string> files; // extract7z/archive: имена файлов для копирования в кэш
};

struct Variant {
    std::string id;
    std::vector<std::string> args;
    std::string note;
};

struct CliCheck {
    bool present = false;
    std::vector<std::string> cmd;    // по умолчанию ["--help"]
    std::vector<std::string> expect; // обязательные подстроки в выводе
};

struct Format {
    std::string id;
    std::string name;
    std::string extension;
    std::string homepage;
    bool enabled = true;

    std::string engine_kind;               // binary | ffmpeg
    std::string engine_executable;
    std::string engine_decoder_executable;
    std::string engine_codec;
    std::string engine_container;

    std::vector<DownloadEntry> downloads;

    std::vector<std::string> encode_cmd;
    std::vector<Variant> variants;
    std::vector<std::string> decode_cmd;

    std::string verify_kind;               // builtin | none
    std::vector<std::string> verify_cmd;

    std::string tag_system;                // vorbis | apev2 | id3 | id3v1 | mp4
    std::string tag_writer;
    std::map<std::string, bool> tag_caps;  // text/pictures/lyrics/cue_sheet/replay_gain/chapters

    // Data-driven tag fields (из formats/*.json → tag.*)
    std::string tag_write_method;                          // id3v1_append | flac_metadata | apev2_tail | id3v2_header | mp4_ilst
    bool tag_native_reader = false;                        // встроенный парсер тегов
    bool tag_validate_skip_ffprobe = false;                // OFR/TAK: пропуск ffprobe при валидации
    std::vector<std::string> tag_numeric_fields;           // поля для norm_num (track, disc)
    std::map<std::string, std::string> tag_key_map;        // canonical → format key (запись)
    std::map<std::string, std::string> tag_reverse_key_map; // format → canonical (чтение)
    std::vector<std::string> tag_allowed_keys;             // write_constraints: пусто = все
    bool tag_replaygain_allowed = true;
    bool tag_pictures_allowed = true;
    bool tag_cue_sheet_allowed = true;
    bool tag_write_supported = true;                       // false для RIFF (запись не реализована)

    int channels_min = 1;
    int channels_max = 0;
    std::vector<int> bit_depth;
    bool has_sample_rate = false;
    int sample_rate_min = 0;
    int sample_rate_max = 0;

    CliCheck cli_check;
    std::string notes;
};

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Каталог formats/ рядом с exe.
std::string formats_dir();
// Каталог bin/ рядом с exe.
std::string bin_dir();

// Значение "language" из llao.json рядом с exe ("auto" по умолчанию, если
// файла нет или поле отсутствует).
std::string load_settings_lang();

// Загружает и валидирует все formats/*.json (сортировка по имени файла).
std::vector<Format> load_all();
// Один формат по id; бросает Error если не найден.
const Format& load_one(const std::vector<Format>& all, const std::string& id);
// Валидирует один конфиг (после разбора JSON).
Format validate(const nlohmann::json& data);

}  // namespace config
