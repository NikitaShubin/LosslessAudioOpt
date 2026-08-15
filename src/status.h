#pragma once
#include <cstddef>
#include <string>

namespace status {

// Инициализация: вызывается один раз перед началом обработки файлов.
// no_status=true — принудительно линейный режим. В противном случае режим
// определяется автоматически: интерактивный, если stdout — консоль с поддержкой
// VT-последовательностей, иначе линейный (обычный построчный вывод).
void init(size_t total_files, bool no_status);

// true, если активен интерактивный статусбар (перерисовка строк).
bool interactive();

// Счётчик в начале строки: "[  3/12]".
std::string counter(size_t idx);

// Имя файла, выровненное по ширине колонки имени (обрезка с многоточием).
std::string pad_name(const std::string& name);

// Колонка победителя/статуса (фиксированная ширина, обрезка).
std::string win_col(const std::string& s);

// Колонка процента (6 символов, справа). pct<0 — прочерк.
std::string pct_col(double pct);

// Печать строки поверх статусбара (обычный вывод, уходит в scrollback).
void log(const std::string& line);

// Печать строки в stderr поверх статусбара.
void error(const std::string& line);

// Установить полосу прогресса для файла idx (label — имя файла).
void begin_file(size_t idx, const std::string& label, size_t total_tasks);

// Обновить прогресс (done из total) с троттлингом.
void tick(size_t idx, size_t done);

// Завершить файл: полоса заменяется строкой результата.
// label — имя файла, winner — текст колонки победителя, pct — процент (pct<0 — прочерк).
void end_file(size_t idx, const std::string& label, const std::string& winner, double pct);

// Финал: сбросить цвета, восстановить режим консоли, курсор в конец.
// Строки результатов остаются на экране.
void shutdown();

}  // namespace status
