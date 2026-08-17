#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace report {

// Локальное время "YYYYMMDD-HHMMSS" (для имён файлов журналов/отчётов).
std::string timestamp();

// Потокобезопасный журнал JSONL для одного запуска.
// Каждая запись — один JSON-объект в строке; в начало добавляется "ts".
class Logger {
public:
    // dir — каталог журнала (создаётся при необходимости).
    explicit Logger(const std::string& dir);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Добавляет событие (дописывает "ts", если его нет).
    void event(const nlohmann::json& ev);

    const std::string& path() const { return path_; }
    bool ok() const { return ok_; }

private:
    std::string path_;
    std::mutex m_;
    std::ofstream f_;
    bool ok_ = false;
};

// Сводка по одному файлу для итогового отчёта.
struct FileSummary {
    std::string path;
    std::string status;   // ok | skip | error
    std::string detail;   // причина skip/error или строка о победителе
    bool replaced = false;
    std::string replacement_error;  // текст ошибки замены на месте (если не удалась)
    uint64_t original = 0;
    uint64_t best = 0;
    double savings_pct = 0;
    std::string best_format;
    std::string best_variant;
    std::vector<std::string> exclusions;  // причины исключений (по форматам)
};

// Пишет человекочитаемый итоговый отчёт (таблица файлов, экономия, исключения).
// path — путь к файлу отчёта.
void write_report(const std::string& path, const std::vector<FileSummary>& files);

}  // namespace report
