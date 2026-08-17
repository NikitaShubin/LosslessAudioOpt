#pragma once
#include <cstddef>
#include <string>

namespace status {

// Состояние отдельного варианта (сегмента полосы файла).
enum class TaskState { Running, Ok, Failed };

// Инициализация: вызывается один раз перед началом обработки.
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

// Строки создаются на все файлы сразу; label — имя файла, обычно путь
// относительно заданного в параметрах корня (может содержать слэши).
void begin_file(size_t idx, const std::string& label);

// Файл взят в prep — помечается «активным» (за ним следует вьюпорт).
void prep(size_t idx);

// После prep: число вариантов файла = число сегментов полосы.
void set_tasks(size_t idx, size_t total);

// Смена состояния варианта task_idx (см. TaskState).
void task(size_t idx, size_t task_idx, TaskState st);

// Файл обработан успешно: полоса остаётся с финальными цветами сегментов,
// pct — процент выигрыша в сжатии (для отображения справа).
void end_file(size_t idx, double pct);

// Файл не конвертирован (skip): полоса целиком тускло-серая.
void mark_skip(size_t idx);

// Файл завершился ошибкой: полоса целиком красная.
void mark_error(size_t idx);

// Печать строки. В интерактивном режиме подавляется (на экране только полосы);
// в линейном — обычный построчный вывод.
void log(const std::string& line);

// То же в stderr.
void error(const std::string& line);

// Финал: сбросить цвета, восстановить режим консоли, курсор в конец.
void shutdown();

}  // namespace status