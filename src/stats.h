#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace stats {

// Путь к stats.json (рядом с exe).
std::string path();

// Читает все записи (массив). При отсутствии/ошибке файла — пустой список.
std::vector<nlohmann::json> load();

// Добавляет одну запись и сохраняет файл.
bool append(const nlohmann::json& item);

// Добавляет пачку записей за один проход (потокобезопасно).
bool append_all(const std::vector<nlohmann::json>& items);

// Краткая сводка накопленной статистики (для `llao.exe stats`).
void print_summary(const std::vector<nlohmann::json>& items);

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
