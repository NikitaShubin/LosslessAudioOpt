#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace stats {

// Путь к stats.json (рядом с exe; переопределяется LLAO_STATS_FILE).
std::string path();

// Читает все записи (массив). При отсутствии/ошибке файла — пустой список.
std::vector<nlohmann::json> load();

// Добавляет одну запись и сохраняет файл.
bool append(const nlohmann::json& item);

// Добавляет пачку записей за один проход (потокобезопасно).
bool append_all(const std::vector<nlohmann::json>& items);

// Краткая сводка накопленной статистики (для `llao.exe stats`).
void print_summary(const std::vector<nlohmann::json>& items);

// Текст файла-отчёта (для `llao.exe stats --report=<file>`). Пустая строка,
// если записей нет: отчёт без данных бесполезен. Формат табличный и без
// локализации — в нём только id форматов, числа и проценты, — чтобы его можно
// было отдать автору кодека как есть.
std::string build_report(const std::vector<nlohmann::json>& items);

// Записать build_report() в dest. false — если нечего писать или запись не удалась.
bool write_report(const std::string& dest, const std::vector<nlohmann::json>& items);

// Ранжирование форматов по накопленной статистике: формат выше — тем более
// вероятен как победитель (средняя экономия по успешным кандидатам).
struct Rank {
    std::string format;
    double savings = 0.0;    // средняя экономия: 1.0 = в 2 раза меньше исходника
    int samples = 0;         // сколько кандидатов учтено
    uint64_t total_in = 0;
    uint64_t total_out = 0;
};

// Форматы с выборкой: по убыванию средней экономии (при равенстве — больше
// данных впереди). Форматы без успешных кандидатов не попадают в результат.
std::vector<Rank> ranking(const std::vector<nlohmann::json>& items);

}  // namespace stats
