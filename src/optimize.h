#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace optimize {

// Режим верификации кандидатов.
enum class Verify {
    All,     // полная проверка каждого кандидата (builtin + декод + PCM)
    Winner,  // проверяется только победитель по cost, перед заменой
    None,    // никакой проверки, замена сразу
};

// Режим выполнения сессии.
enum class SessionMode {
    OneShot,  // одноразовый прогон (llao.exe optimize): ошибка файла без
              // --ignore-errors останавливает прогон
    Daemon,   // долгоживущий демон: ошибки файлов не прерывают очередь
              // (эквивалентно вечному --ignore-errors)
};

struct Options {
    std::vector<std::string> inputs;   // файлы/папки
    double jobs = 2.0;                 // число потоков: целое — как есть, вещественное — множитель ядер
    bool jobs_float = true;            // jobs задано как множитель (умножать на число ядер)
    std::vector<std::string> formats;  // пусто = все включённые
    bool no_download = false;
    bool dry_run = false;
    bool allow_lossy = false;          // обрабатывать lossy-входы (mp3, aac, …)
    bool debug = false;                // писать журнал runs/*.jsonl
    bool no_stats = false;             // не накапливать stats.json
    bool no_status = false;            // без интерактивного статусбара
    std::string report_path;           // путь к итоговому отчёту (пусто = не писать)
    Verify verify = Verify::All;       // режим верификации кандидатов
    bool ignore_errors = false;        // ошибки файлов помечать skip, прогон не прерывать
    std::string tmp_dir;               // --tmp: путь к временной папке (пусто = exe_dir/tmp)
    SessionMode mode = SessionMode::OneShot;  // режим выполнения сессии
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

// --- Движок для демона (долгоживущий, пополняемая очередь) ---

// Строка очереди для снимка состояния.
struct EngineFile {
    size_t idx = 0;
    std::string path;
    std::string rel;
    std::string state;   // queued | prep | running | ok | skip | error | removed
    size_t completed = 0;  // выполнено задач (вариантов)
    size_t total_tasks = 0;  // всего задач файла (0 до prep)
    double pct = 0.0;     // выигрыш в сжатии (после ok)
    uint64_t original = 0;
    uint64_t best = 0;
    std::string best_format;
    std::string detail;  // текст ошибки/иесключения при skip/error
};

// Движок оптимизации: держит Runner + пул воркеров, принимает файлы на лету,
// отдаёт снимок очереди. Реализация (pimpl) — в optimize.cpp.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Инициализация: загрузка конфигов, ранжирование форматов, tmp-каталог,
    // запуск пула воркеров. opts копируются внутрь (движок владеет копией).
    // initial_inputs могут быть пусты (демон добавляет файлы позже через add).
    // Возвращает 0 при успехе, иначе код ошибки (сообщение в err).
    int init(const Options& opts, const std::vector<std::string>& initial_inputs,
             std::string* err);

    // Добавить файлы/папки в очередь на лету.
    void add(const std::vector<std::string>& inputs);

    // Снять файл из очереди (pending — сразу, running — дорабатывает).
    bool remove(size_t idx);

    // Переупорядочить очередь (ids — новый порядок индексов файлов).
    bool reorder(const std::vector<size_t>& ids);

    // Пауза/продолжение всей очереди.
    void pause();
    void resume();

    // Снимок состояния всех файлов очереди.
    std::vector<EngineFile> snapshot();

    // Число завершённых файлов и оставшихся.
    size_t done_count();
    size_t total_count();

    // Запрашивает graceful shutdown воркеров и ждёт их завершения.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct RestoreOptions {
    std::vector<std::string> inputs;   // файлы/папки
    double jobs = 2.0;                 // число потоков: целое — как есть, вещественное — множитель ядер
    bool jobs_float = true;            // jobs задано как множитель (умножать на число ядер)
    std::string to = "flac";           // целевой формат
    std::string variant;               // пусто = последний вариант (максимальное сжатие)
    bool no_download = false;
    bool allow_lossy = false;          // восстанавливать и lossy-входы
    bool no_status = false;            // без интерактивного статусбара
};

// Восстановление: декод оптимизированного файла -> пережатие в целевой формат
// (по умолчанию FLAC) -> теги обратно (embedded или из .tags.zip). Возвращает код выхода.
int restore_run(const RestoreOptions& opts);

// Список вариантов сжатия (комбинации параметров кодера) для каждого формата
// из formats/*.json; ids пуст = все форматы. Возвращает код выхода.
int list_variants(const std::vector<std::string>& ids);

// Очистка tmp-директории текущего процесса (вызывается из atexit).
void clear_session_tmp_dir(const std::string& custom_tmp);

}  // namespace optimize
