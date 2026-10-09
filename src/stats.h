#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "optimize.h"

namespace stats {

// ---------------------------------------------------------------------------
// Хранение статистики разделено на два файла с разными задачами.
//
//   stats.jsonl — журнал. Append-only, одна строка на один закрытый файл.
//     Горячий путь: дописать запись, ничего не перечитывая. Раньше база была
//     одним JSON-массивом, и append перечитывал и перезаписывал его целиком на
//     каждый файл — на реальной библиотеке это и есть основная цена прогона.
//     Содержит всё, включая ошибки и тайминги: это база для разбора.
//
//   stats.tsv — итог. Не пишется в процессе, а выводится из журнала, когда
//     работа закончена. Одна строка на файл, столбцы — свойства источника и
//     размер каждой пары format/variant. Только то, что нужно для статистики
//     и аналитики; промежуточные и ошибочные состояния остаются в журнале.
//
// Пути переопределяются переменными окружения (нужно тестам и разбору чужой
// статистики); по умолчанию все три лежат рядом с exe. У клиента и у демона
// это разные экземпляры, поэтому пути и различаются.
std::string journal_path();     // LLAO_STATS_JOURNAL, иначе stats.jsonl
std::string tsv_path();         // LLAO_STATS_TSV, иначе stats.tsv
std::string dump_path();        // LLAO_STATS_FILE, иначе stats.json

// Идентификатор текущего прогона (время запуска UTC + pid). Одинаков во всех
// записях процесса, поэтому прогоны различимы в базе и их можно выборочно чистить.
std::string run_id();

// Текущее время в ISO-8601 UTC — метка времени записи.
std::string now_iso();

// Одна запись журнала = один закрытый файл со всеми его кандидатами.
//
// Раньше запись создавалась на каждого кандидата, и по базе было нельзя узнать,
// кто выиграл файл: победителя приходилось расставлять постфактум перебором
// списка, из-за чего на файл появлялось несколько записей, а отдельная запись
// «verify_fail» дублировала кандидата. Теперь файл закрывается один раз, все
// кандидаты лежат внутри, победитель записан явно.
struct Record {
    std::string ts;        // ISO-8601, UTC
    std::string run_id;    // прогон, чтобы базу можно было чистить выборочно
    std::string file;      // исходник, как его видел движок
    std::string out_path;  // что осталось на диске; ключ итоговой таблицы
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

    // Размер эталонного WAV с тегатами. Знаменатель, от которого отсчитывается
    // сжатие любого lossless-кодека: без него размеры форматов между собой не
    // сравнить. Читается с диска в момент закрытия файла.
    uint64_t wav_size = 0;

    std::vector<optimize::Candidate> candidates;

    // Варианты, отсечённые ограничениями самого кодека (формат не умеет такую
    // разрядность или такое число каналов). В отчёте это не сбой, а «здесь этот
    // кодек неприменим» — в таблице такие ячейки помечаются NA.
    std::vector<std::pair<std::string, std::string>> excluded;

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

// Читает запись обратно. Записи неизвестной или старой схемы пропускаются.
bool from_json(const nlohmann::json& j, Record* out);

// Дописывает записи в журнал (потокобезопасно, без перечитывания файла).
bool append_all(const std::vector<nlohmann::json>& items);

// Читает журнал. Если журнала нет, читается старый stats.json — чтобы база,
// накопленная прошлыми версиями, продолжала работать без ручной возни.
std::vector<Record> load();

// Отладочный дамп журнала в JSON-массив (тот прежний формат целиком).
bool write_dump(const std::string& dest);

// ---------------------------------------------------------------------------
// Итоговая таблица
// ---------------------------------------------------------------------------

// Ячейка размера одной пары format/variant.
//
//   Empty — пара не запускалась (или запускалась и не дала результата);
//   NA    — кодек отсечён ограничениями самого формата;
//   Value — лучший cost среди успешных кандидатов пары.
struct Cell {
    enum class State { Empty, NA, Value } state = State::Empty;
    uint64_t value = 0;
};

// Строка итоговой таблицы: один файл в том виде, в каком он лежит на диске.
struct Row {
    std::string out_path;   // ключ строки; при пустом отданном пути — исходник
    std::string run_id;     // прогон, зафиксировавший итог
    std::string ts;
    int runs = 1;           // сколько прогонов затронули файл

    std::string source_format;
    std::string codec_name;
    uint64_t source_size = 0;
    int bits = 0;
    int channels = 0;
    int sample_rate = 0;
    uint64_t duration_ms = 0;
    bool has_tags = false;
    uint64_t wav_size = 0;

    bool has_winner = false;
    std::string winner_format;
    std::string winner_variant;
    uint64_t winner_cost = 0;
    uint64_t winner_sidecar = 0;

    std::map<std::string, Cell> cells;   // ключ "format:variant"
};

// Собирает строки из журнала и записывает таблицу в dest.
//
// Слияние: файл может встретиться в нескольких прогонах (сначала с ошибкой,
// потом после починки), и таблица хранит самый полный результат — по каждой
// паре берётся последнее непустое значение, так что повторный перебор только
// упавших вариантов дополняет таблицу, а не затирает её. Строка попадает в
// таблицу, только если итоговый статус файла — ok: незавершённое в статистику
// не идёт, для разбора остаётся журнал.
bool export_tsv(const std::string& dest, std::string* err = nullptr);

// Читает таблицу. Пустой список при отсутствии или ошибке файла.
std::vector<Row> load_tsv();

// Столбцы таблицы в фиксированном порядке; sizes — пары format/variant.
std::vector<std::string> tsv_columns(const std::vector<Row>& rows, size_t* fixed_count);

// Сборка таблицы без записи на диск — для тестов и проверок.
std::string build_tsv(const std::vector<Row>& rows);

// Экранирование значения ячейки/пути: обратный слэш и управляющие символы.
// В имени файла на POSIX допустим любой байт кроме '/', поэтому «табуляция
// разделяет столбцы, а в пути её не бывает» — предположение, а не факт.
std::string tsv_escape(const std::string& s);
std::string tsv_unescape(const std::string& s);

// ---------------------------------------------------------------------------
// Сводки по таблице
// ---------------------------------------------------------------------------

// Краткая сводка накопленной статистики (для `llao.exe stats`).
void print_summary(const std::vector<Row>& rows);

// Один столбик гистограммы: метка бина и сколько файлов в него попало.
struct HistBin {
    std::string label;
    int count = 0;
};

// Распределение экономии по бинам. Экономия файла = 1 - cost/source_size;
// файлы, которые не были отданы, и lossy-исходники не учитываются (иначе в
// нулевом бине копятся ошибки кодеков, а не результат).
std::vector<HistBin> savings_histogram(const std::vector<Row>& rows,
                                       const std::string& fmt = std::string());

// Распределение исходных размеров по бинам. Бины кратны 2, начиная с 1 МБ:
// для музыкальной библиотеки это читаемее, чем равномерная шкала в килобайтах.
std::vector<HistBin> size_histogram(const std::vector<Row>& rows,
                                    const std::string& fmt = std::string());

// Текст гистограммы в ASCII-столбиках, нормированных на максимальный бин.
// Без локализации, как и отчёт: тот же текст предназначен для передачи автору
// кодека.
std::string histogram_text(const std::string& title, const std::vector<HistBin>& bins);

// Текст файла-отчёта (для `llao.exe stats --report=<file>`). Пустая строка,
// если записей нет: отчёт без данных бесполезен. Формат табличный и без
// локализации — в нём только id форматов, числа и проценты, — чтобы его можно
// было отдать автору кодека как есть.
std::string build_report(const std::vector<Row>& rows);

// Записать build_report() в dest. false — если нечего писать или запись не удалась.
bool write_report(const std::string& dest, const std::vector<Row>& rows);

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
std::vector<Rank> ranking(const std::vector<Row>& rows);

// ---------------------------------------------------------------------------
// Сводка эффективности методов — для веб-диаграммы
// ---------------------------------------------------------------------------
//
// Совокупность одна: все обработанные треки. Разбивки по альбомам нет — она
// ничего не сообщает о кодеке. Вместо неё фильтры по свойствам трека:
// разрядность, частота дискретизации, число каналов и длительность заметно
// меняют эффективность сжатия, поэтому это отбор внутри совокупности, а не
// набор отдельных диаграмм.

enum class SummaryFacet { Bits, SampleRate, Channels, Duration };

// Отбор. Ноль — «любое значение», duration = -1 — «любая длительность».
struct SummaryFilter {
    int bits = 0;
    int sample_rate = 0;
    int channels = 0;
    int duration_bucket = -1;
    // Знаменатель экономии. По умолчанию wav_size — размер несжатого оригинала
    // (эталонный WAV с тегами), потому что только тогда проценты читаются как
    // «во сколько раз файл меньше несжатого». source_size — то, что лежало на
    // диске: если исходник уже сжат другим кодеком, проценты сравнивают
    // кодек с кодеком, а не с несжатым оригиналом.
    bool wav_denominator = true;
};

// Номер корзины длительности. Границы — в миллисекундах, по возрастанию;
// корзин на одну больше, чем границ.
int duration_bucket_of(uint64_t duration_ms);
int duration_bucket_count();
uint64_t duration_bucket_lower_ms(int bucket);

// Число бинов гистограммы распределения экономии: по 5 % от 0 до 100.
// Плюс отдельный счётчик kSummaryHistGrew — файлы, которые метод УВЕЛИЧИЛ.
constexpr int kSummaryHistBins = 21;
constexpr int kSummaryHistGrew = kSummaryHistBins;

// Свод по одному методу (формату).
struct MethodSummary {
    std::string format;
    int considered = 0;      // треков, где метод дал результат
    int not_applicable = 0;  // треков, где метод отсечён ограничениями кодека
    int wins = 0;            // треков, которые метод выиграл
    double mean = 0.0;       // средняя экономия; 0.1 = на 10 % меньше
    double stddev = 0.0;     // разброс: на столько метод промахивается в обе стороны
    double min = 0.0;
    double max = 0.0;
    uint64_t total_in = 0;   // сумма знаменателей
    uint64_t total_out = 0;  // сумма результатов
    int hist[kSummaryHistBins + 1] = {0};  // распределение экономии по бинам
};

// Свод по одному заданию: пара «формат:вариант».
struct VariantSummary {
    std::string format;
    std::string variant;   // идентификатор варианта из formats/*.json
    std::string key;       // "format:variant" — как в таблице
    std::string name;      // человеческое имя формата
    std::string family;    // engine.kind: binary | ffmpeg
    int considered = 0;
    int wins = 0;
    double mean = 0.0;
    double stddev = 0.0;
    double min = 0.0;
    double max = 0.0;
    uint64_t total_in = 0;
    uint64_t total_out = 0;
    int hist[kSummaryHistBins + 1] = {0};
};

struct Summary {
    std::string generated;
    int files = 0;      // строк в таблице всего
    int in_sample = 0;  // строк, прошедших фильтр
    bool wav_denominator = true;  // проставляется в summarize() из фильтра
    std::vector<MethodSummary> methods;  // по убыванию средней экономии
    // То же самое, но по ЗАДАНИЯМ (пара «формат:вариант»), а не по методам.
    // Их десятки: у OptimFROG одиннадцать пресетов, у FLAC больше десятка
    // вариантов. Сравнивать надо именно их — по методу видно только «среднее по
    // кодеку», а разброс внутри кодека как раз и объясняет, почему он
    // проигрывает сам себе.
    std::vector<VariantSummary> variants;  // в порядке форматов и вариантов
};

// Свод по всем методам. Список форматов выводится из данных таблицы, а не
// зашит: новый формат из formats/*.json попадёт сюда сам.
Summary summarize(const std::vector<Row>& rows, const SummaryFilter& f);

// Тот же свод в JSON — ответ /api/stats.
std::string summary_json(const std::vector<Row>& rows, const SummaryFilter& f);

// Счётчики значений по каждому измерению при текущем фильтре. Значения самого
// измерения в фильтре игнорируются: иначе чип показывал бы ноль и переключить
// его было бы нельзя.
struct FacetValue {
    int value = 0;
    int files = 0;
};
struct FacetCounts {
    SummaryFacet facet = SummaryFacet::Bits;
    std::vector<FacetValue> values;
};
std::vector<FacetCounts> facet_counts(const std::vector<Row>& rows, const SummaryFilter& f);

// Разбор фильтра из query-строки HTTP. Пустые и нечисловые значения означают
// «любое».
SummaryFilter summary_filter_from_query(const std::string& query);

// Прочитать журнал и собрать из него строки таблицы. Тот же путь, что у
// export_tsv(): итоговая таблица и сводка должны считаться из одних и тех же
// данных, иначе числа на веб-диаграмме разойдутся с stats.tsv.
std::vector<Row> load_rows();

}  // namespace stats
