#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// obs — Observable: контракт между движком оптимизации и любым потребителем
// событий (псевдографический статусбар, headless-демон, веб-клиент и т.д.).
// Движок никогда не знает о конкретной реализации UI — он эмитит события через
// глобальный obs::sink().
namespace obs {

// Состояние отдельного варианта (сегмента полосы файла / задачи очереди).
enum class TaskState { Running, Ok, Failed };

// Метаданные задачи для тултипов (формат/вариант/параметры).
struct TaskInfo {
    std::string fmt_id;
    std::string variant_id;
    std::vector<std::string> params;
    std::string note;
};

// Интерфейс приёмника событий. Все методы вызываются движком; реализации
// должны быть потокобезопасны (движок вызывает их из воркеров).
struct Sink {
    virtual ~Sink() = default;

    // Строка создаётся на файл; label — имя (обычно путь относительно корня).
    virtual void begin_file(size_t, const std::string&) {}

    // Файл взят в prep — помечается «активным» (за ним следует вьюпорт).
    virtual void prep(size_t) {}

    // После prep: число вариантов файла = число сегментов полосы.
    virtual void set_tasks(size_t, size_t) {}
    // Расширенный вариант с метаданными задач (для демона/веба).
    virtual void set_tasks(size_t, const std::vector<TaskInfo>&) {}

    // Форматы, исключённые по caps для этого файла (жёлтые точки в вебе).
    virtual void set_excluded(size_t, const std::vector<std::string>&) {}

    // Смена состояния варианта task_idx.
    virtual void task(size_t, size_t, TaskState) {}

    // Файл обработан успешно; pct — процент выигрыша в сжатии.
    virtual void end_file(size_t, double) {}

    // Файл завершился ошибкой.
    virtual void mark_error(size_t) {}

    // Файл завершился ошибкой с причиной (единая модель ошибки: реализация
    // получает и id, и текст причины и может сформировать одно событие).
    virtual void error_file(size_t, const std::string&) {}

    // Файл остановлен/не подходит (пользователь остановил, или ранний отсев).
    virtual void mark_stopped(size_t) {}

    // Печать диагностической строки (подавляется в интерактивном режиме).
    virtual void log(const std::string&) {}

    // Печать строки ошибки.
    virtual void error(const std::string&) {}

    // Файлы добавлены в очередь на лету (для демона); label/label каждого.
    virtual void files_added(const std::vector<size_t>&, const std::vector<std::string>&) {}

    // Метаданные задачи строки (демон): режим ("optimize"/"restore") и целевая
    // папка (пусто = замена на месте). Вызывается при добавлении строки.
    virtual void job_meta(size_t, const std::string&, const std::string&) {}

    // Итоговый путь результата (демон): вызывается после успешного завершения
    // файла, когда фактический путь отличается от исходного (restore в цель).
    virtual void out_file(size_t, const std::string&) {}
};

// Глобальный приёмник событий. Устанавливается один раз до старта воркеров.
Sink* sink();
void set_sink(Sink* s);

}  // namespace obs
