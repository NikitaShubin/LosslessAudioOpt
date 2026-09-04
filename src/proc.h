#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace proc {

struct Result {
    bool started = false;    // процесс создан
    bool timed_out = false;  // превышен таймаут, процесс завершён принудительно
    bool stalled = false;    // stall: процесс не.progressировал (файл не растёт + CPU ≈ 0)
    bool cancelled = false;  // прерван из-за cancel() (Ctrl+C), процесс завершён принудительно
    bool aborted = false;    // прерван из-за abort_all() (ошибка без --ignore-errors)
    int exit_code = -1;
    uint64_t cpu_ms = 0;     // процессорное время процесса (kernel+user), только при started
    std::string output;      // stdout+stderr (объединено)
    std::string error;       // текст ошибки при неудачном запуске
};

// Мониторинг progress: если output-файл не растёт и CPU процесса ≈ 0
// stall_timeout секунд подряд — процесс завершается принудительно.
// hard_timeout_sec — абсолютный лимит (wall-clock), защита от infinite loop.
// Если оба = 0 — мониторинг выключен.
struct OutputMonitor {
    std::string path;             // путь к output-файлу (кандидат)
    int stall_timeout_sec = 120;  // файл не растёт + CPU ≈ 0 → kill
    int hard_timeout_sec = 0;     // абсолютный лимит (0 = не используется)
};

// Глобальная отмена: сигнализирует всем запущенным/ожидающим proc::run(),
// что работу нужно прекратить (вызывается из обработчика Ctrl+C).
// Текущие процессы завершаются принудительно (TerminateProcess).
void cancel();
bool cancelled();

// Сброс флагов отмены/прерывания. Используется демоном между сессиями
// (после снятия cancel-all/после завершения батча перед новым add).
void reset_cancel_and_abort();

// Глобальное прерывание при неигнорируемой ошибке: активные процессы
// завершаются принудительно, не дожидаясь их окончания. Отличие от cancel() —
// прерванная задача не считается отменой пользователем (не возвращает код 130).
void abort_all();
bool aborted();

// Запуск процесса без шелла. args[0] — исполняемый файл (путь или имя из PATH).
// timeout_sec == 0 — без ограничения времени. cwd — рабочий каталог (пусто = текущий).
// monitor — опциональный мониторинг прогресса (stall detection по файлу/CPU).
// kill_flag — опциональный per-file флаг мгновенной остановки (remove файла
// из очереди демона): процесс завершается принудительно, res.cancelled = true.
// Проверяется в том же цикле опроса, что и глобальные cancel/abort (200 мс).
Result run(const std::vector<std::string>& args, int timeout_sec = 0,
           const std::string& cwd = "", const OutputMonitor& monitor = {},
           const std::atomic<bool>* kill_flag = nullptr);

// Суммарное процессорное время всех дочерних процессов, запущенных текущим
// потоком через run() (накапливается в thread_local). Для атрибуции затрат на
// конкретную задачу: снимок в начале/конце задачи, разность — CPU этой задачи,
// не зависящий от планировщика/приоритета окна.
uint64_t child_cpu_ms();

// Процессорное время текущего потока (kernel+user) с момента его старта.
uint64_t thread_cpu_ms();

}  // namespace proc
