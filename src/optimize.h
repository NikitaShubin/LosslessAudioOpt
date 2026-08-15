#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace optimize {

struct Options {
    std::vector<std::string> inputs;   // файлы/папки
    int jobs = 0;                      // 0 = число ядер
    std::vector<std::string> formats;  // пусто = все включённые
    bool no_download = false;
    bool dry_run = false;
    bool allow_lossy = false;          // обрабатывать lossy-входы (mp3, aac, …)
    bool debug = false;                // писать журнал runs/*.jsonl
    bool no_stats = false;             // не накапливать stats.json
    bool no_status = false;            // без интерактивного статусбара
    std::string report_path;           // путь к итоговому отчёту (пусто = не писать)
};

struct Candidate {
    std::string format;
    std::string variant;
    uint64_t size = 0;         // файл (после тегов)
    uint64_t sidecar = 0;      // размер sidecar (0 если не нужен)
    uint64_t cost = 0;         // файл + sidecar
    bool has_tags = false;
    std::string path;          // путь к tmp-файлу кандидата
    size_t order = 0;          // детерминированный порядок (для тай-брейка)
};

struct FileResult {
    std::string path;
    std::string status;        // ok | replace | skip | error
    std::string message;
    uint64_t original_size = 0;
    uint64_t best_cost = 0;
    std::string best_format;
    std::string best_variant;
    std::vector<Candidate> candidates;  // прошедшие валидацию, с размером < оригинала
};

// Полный перебор форматов для каждого входного файла. Возвращает код выхода.
int run(const Options& opts);

struct RestoreOptions {
    std::vector<std::string> inputs;   // файлы/папки
    int jobs = 0;                      // 0 = число ядер
    std::string to = "flac";           // целевой формат
    std::string variant;               // пусто = последний вариант (максимальное сжатие)
    bool no_download = false;
    bool allow_lossy = false;          // восстанавливать и lossy-входы
};

// Восстановление: декод оптимизированного файла -> пережатие в целевой формат
// (по умолчанию FLAC) -> теги обратно (embedded или из .tags.zip). Возвращает код выхода.
int restore_run(const RestoreOptions& opts);

// Список вариантов сжатия (комбинации параметров кодера) для каждого формата
// из formats/*.json; ids пуст = все форматы. Возвращает код выхода.
int list_variants(const std::vector<std::string>& ids);

}  // namespace optimize
