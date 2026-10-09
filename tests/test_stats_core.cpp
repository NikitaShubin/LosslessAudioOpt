// Юнит-тесты сводки эффективности (stats::summarize / summary_json).
//
// Сводка — единственное место, где из журнала получаются числа для веба, и
// ошибка в ней молчалива: JSON остаётся валидным, диаграмма просто показывает
// неверное. Поэтому проверяем значения вручную, а не только форму ответа.
//
// Сборка:  make test-stats-core
// Запуск:  ./test-stats-core

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "stats.h"

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) static void test_##name();                          \
    struct Register_##name { Register_##name() { test_##name(); } } reg_##name; \
    static void test_##name()

#define ASSERT(expr) do {                                              \
        if (!(expr)) {                                                \
            fprintf(stderr, "  FAIL: %s (строка %d)\n", #expr, __LINE__); \
            g_failed++;                                               \
            return;                                                   \
        }                                                             \
        g_passed++;                                                   \
    } while (0)

#define ASSERT_EQ(a, b) do {                                          \
        auto _a = (a); auto _b = (b);                                 \
        if (!(_a == _b)) {                                            \
            fprintf(stderr, "  FAIL: %s == %s (строка %d)\n", #a, #b,  \
                    __LINE__);                                        \
            g_failed++;                                               \
            return;                                                   \
        }                                                             \
        g_passed++;                                                   \
    } while (0)

#define ASSERT_NEAR(a, b, eps) do {                                   \
        const double _a = (a), _b = (b);                              \
        if (std::fabs(_a - _b) > (eps)) {                             \
            fprintf(stderr, "  FAIL: %s = %.6f, ожидалось %.6f (строка %d)\n", \
                    #a, _a, _b, __LINE__);                            \
            g_failed++;                                               \
            return;                                                   \
        }                                                             \
        g_passed++;                                                   \
    } while (0)

// ---------------------------------------------------------------------------

static stats::Row make_row(uint64_t wav, int bits, int rate, int ch,
                           uint64_t dur_ms) {
    stats::Row r;
    r.out_path = "t/" + std::to_string(bits) + "_" + std::to_string(rate) + "_" +
                 std::to_string(wav);
    r.source_size = wav / 4;  // исходник уже сжат; для свода это неважно
    r.wav_size = wav;
    r.bits = bits;
    r.sample_rate = rate;
    r.channels = ch;
    r.duration_ms = dur_ms;
    r.codec_name = "ape";  // lossless, иначе строка отсеется как lossy
    return r;
}

static void put(stats::Row* r, const std::string& key, uint64_t cost) {
    r->cells[key] = stats::Cell{stats::Cell::State::Value, cost};
}

static void na(stats::Row* r, const std::string& key) {
    r->cells[key] = stats::Cell{stats::Cell::State::NA, 0};
}

static const stats::MethodSummary* find_method(const stats::Summary& s,
                                               const std::string& fmt) {
    for (const auto& m : s.methods)
        if (m.format == fmt) return &m;
    return nullptr;
}

static const stats::VariantSummary* find_variant(const stats::Summary& s,
                                                 const std::string& key) {
    for (const auto& v : s.variants)
        if (v.key == key) return &v;
    return nullptr;
}

// ---------------------------------------------------------------------------

// Экономия меряется от НЕСЖАТОГО оригинала, а не от того, что лежит на диске.
TEST(savings_from_uncompressed) {
    stats::Row r = make_row(1000, 16, 44100, 2, 180000);
    put(&r, "flac:0", 400);          // 60 %
    put(&r, "optimfrog:p0", 500);    // 50 %
    const stats::Summary s = stats::summarize({r}, {});
    ASSERT(find_method(s, "flac") != nullptr);
    ASSERT_NEAR(find_method(s, "flac")->mean, 0.60, 1e-9);
    ASSERT_NEAR(find_method(s, "optimfrog")->mean, 0.50, 1e-9);
}

// Разбивка по ЗАДАНИЯМ. Строка метода — лучший результат кода на треке, а не
// среднее по пресетам: усреднение спрятало бы разницу p0/p9.
TEST(summary_per_variant) {
    stats::Row r = make_row(1000, 16, 44100, 2, 180000);
    put(&r, "optimfrog:p0", 500);
    put(&r, "optimfrog:p9", 400);
    const stats::Summary s = stats::summarize({r}, {});
    ASSERT_NEAR(find_method(s, "optimfrog")->mean, 0.60, 1e-9);
    ASSERT(find_variant(s, "optimfrog:p0") != nullptr);
    ASSERT(find_variant(s, "optimfrog:p9") != nullptr);
    ASSERT_NEAR(find_variant(s, "optimfrog:p0")->mean, 0.50, 1e-9);
    ASSERT_NEAR(find_variant(s, "optimfrog:p9")->mean, 0.60, 1e-9);
}

// Знаменатель переключается на размер файла на диске.
TEST(denominator_switches) {
    stats::Row r = make_row(1000, 16, 44100, 2, 180000);
    r.source_size = 250;  // «исходник» вчетверо меньше WAV
    put(&r, "flac:0", 400);
    stats::SummaryFilter f;
    ASSERT(f.wav_denominator);  // по умолчанию — несжатый оригинал
    ASSERT_NEAR(find_method(stats::summarize({r}, f), "flac")->mean, 0.60, 1e-9);
    f.wav_denominator = false;
    ASSERT_NEAR(find_method(stats::summarize({r}, f), "flac")->mean, -0.60, 1e-9);
}

// В грани можно выбрать несколько значений; пустой набор = «любое».
TEST(filter_takes_several_values) {
    std::vector<stats::Row> rows;
    rows.push_back(make_row(1000, 16, 44100, 2, 180000));
    rows.push_back(make_row(1000, 24, 44100, 2, 180000));
    rows.push_back(make_row(1000, 16, 96000, 2, 180000));
    for (auto& r : rows) put(&r, "flac:0", 500);

    stats::SummaryFilter f;
    ASSERT_EQ(stats::summarize(rows, f).in_sample, 3);
    f.bits = {24};
    ASSERT_EQ(stats::summarize(rows, f).in_sample, 1);
    f.bits = {16, 24};
    ASSERT_EQ(stats::summarize(rows, f).in_sample, 3);
    f.bits.clear();
    f.sample_rate = {44100};
    ASSERT_EQ(stats::summarize(rows, f).in_sample, 2);
}

// Разброс и распределение считаются по фактическим значениям.
TEST(spread_and_histogram) {
    std::vector<stats::Row> rows;
    const uint64_t wavs[] = {1000, 1000, 1000};
    const uint64_t costs[] = {900, 500, 100};  // экономия 10 %, 50 %, 90 %
    for (int i = 0; i < 3; i++) {
        stats::Row r = make_row(wavs[i], 16, 44100, 2, 180000);
        put(&r, "flac:0", costs[i]);
        rows.push_back(r);
    }
    const stats::Summary s = stats::summarize(rows, {});
    const stats::MethodSummary* m = find_method(s, "flac");
    ASSERT(m != nullptr);
    ASSERT_NEAR(m->mean, (0.10 + 0.50 + 0.90) / 3.0, 1e-9);
    ASSERT_NEAR(m->min, 0.10, 1e-9);
    ASSERT_NEAR(m->max, 0.90, 1e-9);
    ASSERT(m->stddev > 0.0);
    int nonzero = 0;
    for (int i = 0; i < stats::kSummaryHistBins; i++)
        if (m->hist[i]) ++nonzero;
    ASSERT(nonzero >= 3);
}

// «Отсечён кодеком» и «не выполнялся» — разные вещи.
TEST(na_differs_from_missing) {
    stats::Row r = make_row(1000, 24, 96000, 2, 180000);
    put(&r, "flac:0", 400);
    na(&r, "la:default");
    const stats::Summary s = stats::summarize({r}, {});
    ASSERT(find_method(s, "la") != nullptr);
    ASSERT_EQ(find_method(s, "la")->not_applicable, 1);
    ASSERT_EQ(find_method(s, "la")->considered, 0);
    // У monkeys_audio ячейки нет вовсе — в свод он не попадает.
    ASSERT(find_method(s, "monkeys_audio") == nullptr);
}

// Счётчики своей грани не фильтруются: иначе кнопку нельзя было бы включить.
TEST(facet_counts_ignore_own_dimension) {
    std::vector<stats::Row> rows;
    rows.push_back(make_row(1000, 16, 44100, 2, 1000));
    rows.push_back(make_row(1000, 24, 96000, 1, 400000));
    for (auto& r : rows) put(&r, "flac:0", 500);

    stats::SummaryFilter f;
    f.bits = {24};
    const auto facets = stats::facet_counts(rows, f);
    ASSERT_EQ(facets.size(), static_cast<size_t>(4));
    for (const auto& fc : facets) {
        int total = 0;
        for (const auto& v : fc.values) total += v.files;
        if (fc.facet == stats::SummaryFacet::Bits) ASSERT_EQ(total, 2);
        if (fc.facet == stats::SummaryFacet::SampleRate) ASSERT_EQ(total, 1);
    }
}

// Набор значений для кнопок не зависит от фильтра.
TEST(facet_values_are_complete) {
    std::vector<stats::Row> rows;
    rows.push_back(make_row(1000, 16, 44100, 2, 180000));
    rows.push_back(make_row(1000, 24, 96000, 2, 180000));
    for (auto& r : rows) put(&r, "flac:0", 500);
    stats::SummaryFilter f;
    f.bits = {24};
    const auto facets = stats::facet_counts(rows, f);
    for (const auto& fc : facets) {
        if (fc.facet == stats::SummaryFacet::Bits) ASSERT_EQ(fc.values.size(), 2u);
    }
}

// Победа засчитывается задаче, а не коду.
TEST(wins_are_per_variant) {
    stats::Row r = make_row(1000, 16, 44100, 2, 180000);
    put(&r, "optimfrog:p0", 500);
    put(&r, "optimfrog:p9", 400);
    r.has_winner = true;
    r.winner_format = "optimfrog";
    r.winner_variant = "p9";
    r.winner_cost = 400;
    const stats::Summary s = stats::summarize({r}, {});
    ASSERT(find_variant(s, "optimfrog:p9") != nullptr);
    ASSERT_EQ(find_variant(s, "optimfrog:p9")->wins, 1);
    ASSERT_EQ(find_variant(s, "optimfrog:p0")->wins, 0);
}

// Разбор запроса: список значений, wav=0, мусор.
TEST(parse_filter_from_query) {
    stats::SummaryFilter f = stats::summary_filter_from_query("bits=16,24&rate=44100");
    ASSERT_EQ(f.bits.size(), 2u);
    ASSERT_EQ(f.bits[0], 16);
    ASSERT_EQ(f.bits[1], 24);
    ASSERT_EQ(f.sample_rate.size(), 1u);
    ASSERT_EQ(f.sample_rate[0], 44100);
    ASSERT(f.wav_denominator);
    f = stats::summary_filter_from_query("wav=0");
    ASSERT(!f.wav_denominator);
    ASSERT(f.bits.empty());
    f = stats::summary_filter_from_query("bits=abc");
    ASSERT(f.bits.empty());
}

TEST(duration_buckets_ascend) {
    ASSERT_EQ(stats::duration_bucket_of(1000), 0);
    ASSERT_EQ(stats::duration_bucket_of(60000), 1);
    ASSERT_EQ(stats::duration_bucket_of(300000), 2);
    ASSERT_EQ(stats::duration_bucket_of(3600000), stats::duration_bucket_count() - 1);
}

TEST(json_has_methods_variants_and_facets) {
    stats::Row r = make_row(1000, 16, 44100, 2, 180000);
    put(&r, "flac:0", 400);
    put(&r, "optimfrog:p0", 500);
    const std::string j = stats::summary_json({r}, {});
    ASSERT(j.find("\"variants\"") != std::string::npos);
    ASSERT(j.find("\"facet_values\"") != std::string::npos);
    ASSERT(j.find("\"denominator\":\"wav_size\"") != std::string::npos);
    ASSERT(j.find("optimfrog:p0") != std::string::npos);
}

// ---------------------------------------------------------------------------

int main() {
    if (g_failed == 0) {
        fprintf(stderr, "ALL PASSED (%d assertions)\n", g_passed);
        return 0;
    }
    fprintf(stderr, "%d/%d FAILED\n", g_failed, g_passed + g_failed);
    return 1;
}