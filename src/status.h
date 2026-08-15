#pragma once
#include <cstddef>
#include <string>

namespace status {

// Инициализация: вызывается один раз перед началом обработки файлов.
// no_status=true — принудительно линейный режим. В противном случае режим
// определяется автоматически: интерактивный, если stdout — консоль с поддержкой
// VT-последовательностей, иначе линейный (обычный построчный вывод).
//
// Тестовые хуки (переменные окружения):
//   LLAO_STATUS_FORCE=1 — принудительно включить интерактивный режим
//                         (например, под wine/PTY);
//   LLAO_STATUS_SIZE=<W>x<H> — задать размер консоли, если он не определяется.
void init(size_t total_files, bool no_status);

// true, если активен интерактивный статусбар.
bool interactive();

// Печать строки поверх статусбара (обычный вывод, уходит в scrollback).
void log(const std::string& line);

// Печать строки в stderr поверх статусбара.
void error(const std::string& line);

// Начать полосу прогресса для файла idx (label — имя файла).
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
