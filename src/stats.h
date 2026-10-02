#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "optimize.h"

namespace stats {

// Путь к stats.json (рядом с exe; переопределяется LLAO_STATS_FILE).
std::string path();

// Идентификатор текущего прогона (время запуска UTC + pid). Одинаков во всех
// записях процесса, поэтому прогоны различимы в базе и их можно выборочно чистить.
std::string run_id();

// Текущее время в ISO-8601 UTC — метка времени записи.
std::string now_iso();

// Одна запись базы = один обработанный файл со всеми его кандидатами.
//
// Раньше запись создавалась на каждого кандидата, и по базе было нельзя узнать,
// кто выиграл файл: победителя приходилось расставлять постфактум перебором
// списка, из-за чего на файл появлялось несколько записей, а отдельная запись
// «verify_fail» дублировала кандидата. Теперь файл закрывается один раз, все
// кандидаты лежат внутри, победитель записан явно.
struct Record {
    std::string ts;        // ISO-8601, UTC
    std::string run_id;    // прогон, чтобы базу можно было чистить выборочно
    std::string file;
    std::string status;    // ok | error | stopped
    std::string detail;    // причина для не-ok

    // Свойства исходника
    std::string source_format;   // контейнер: flac / wav / mp3 ...
    std::string codec_name;      // аудиокодек исходника (для отсева lossy)
    uint64_t source_size = 0;
    int channels = 0;
    int sample_rate = 0;
    int bits = 0;
    double duration = 0;
    bool has_tags = false;

    std::vector<optimize::Candidate> candidates;

    bool has_winner = false;
    std::string winner_format;
    std::string winner_variant;
    uint64_t winner_cost = 0;
};

// Собирает запись из данных прогона файла. Единственное место, где известные
// сведения о кандидатах превращаются в формат базы.
nlohmann::json to_json(const Record& rec);

// Запись кандидата внутри Record (публично — им пользуется сборка записи).
nlohmann::json candidate_to_json(const optimize::Candidate& c);

// Читает запись обратно (нужно статистике для сводок). Записи неизвестной или
// старой схемы пропускаются.
bool from_json(const nlohmann::json& j, Record* out);

// Читает все записи. При отсутствии/ошибке файла — пустой список.
std::vector<nlohmann::json> load();

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
// вероятен как победитель.
//
// Считается по файлам, а не по кандидатам: для каждого файла победа — минимальный
// cost среди успешных кандидатов, и формату засчитывается экономия именно этого
// кандидата. Усреднение по всем кандидатам было бессмысленным показателем: у
// формата тем больше «средних», чем больше у него вариантов, а исходники у них
// разные, поэтому flac на старой базе показывал отрицательную экономию.
struct Rank {
    std::string format;
    double savings = 0.0;    // средняя экономия по выигранным файлам: 1.0 = вдвое меньше
    int files = 0;           // сколько файлов выиграл формат
    uint64_t total_in = 0;   // сумма размеров исходников этих файлов
    uint64_t total_out = 0;  // сумма их cost
};

// Форматы с выборкой: по убыванию средней экономии (при равенстве — больше
// файлов впереди). Форматы без побед не попадают в результат.
std::vector<Rank> ranking(const std::vector<nlohmann::json>& items);

}  // namespace stats