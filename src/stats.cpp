#include "stats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <map>
#include <mutex>
#include <set>

#include "config.h"
#include "i18n.h"
#include "media.h"
#include "out.h"
#include "util.h"

namespace json = nlohmann;

namespace stats {

static std::mutex g_mutex;

// Идентификатор прогона: один на процесс. Нужен, чтобы отличать записи разных
// прогонов (чистка базы) и не смешивать их в сводке.
static std::string g_run_id;

std::string run_id() {
    if (g_run_id.empty()) {
        // Время запуска в UTC + pid: достаточно, чтобы различать прогоны в
        // пределах одной машины, и не требует хранить состояние на диске.
        std::time_t t = std::time(nullptr);
        std::tm tmv = {};
#if defined(_WIN32)
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        char stamp[32] = {0};
        std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tmv);
        long pid = 0;
#if defined(_WIN32)
        pid = (long)GetCurrentProcessId();
#else
        pid = (long)getpid();
#endif
        g_run_id = std::string(stamp) + "-" + std::to_string(pid);
    }
    return g_run_id;
}

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tmv = {};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

std::string path() {
    // Оверрайд нужен для тестов и для разбора чужой статистики: по умолчанию
    // файл лежит рядом с exe (у клиента и у демона это разные экземпляры).
    if (const char* env = std::getenv("LLAO_STATS_FILE")) {
        if (*env) return env;
    }
    return util::join_path(util::exe_dir(), "stats.json");
}

std::string journal_path() {
    if (const char* env = std::getenv("LLAO_STATS_JOURNAL")) {
        if (*env) return env;
    }
    return util::join_path(util::exe_dir(), "stats.jsonl");
}

std::string tsv_path() {
    if (const char* env = std::getenv("LLAO_STATS_TSV")) {
        if (*env) return env;
    }
    return util::join_path(util::exe_dir(), "stats.tsv");
}

std::string dump_path() { return path(); }

json::json candidate_to_json(const optimize::Candidate& c) {
    json::json j = {
        {"format", c.format},
        {"variant", c.variant},
        {"task", c.task},
        {"status", c.status},
        {"cost", c.cost},
        {"result_size", c.size},
        {"sidecar_size", c.sidecar},
        {"has_tags", c.has_tags},
        {"wall_ms", c.wall_ms},
        {"cpu_ms", c.cpu_ms},
        {"prep_wall_ms", c.prep_wall_ms},
        {"decode_wall_ms", c.decode_wall_ms},
        {"verify", c.verify},
    };
    if (!c.error.empty()) j["error"] = c.error;
    if (c.retry) j["retry"] = true;
    return j;
}

bool candidate_from_json(const json::json& j, optimize::Candidate* c) {
    if (!j.is_object() || !j.contains("format") || !j.contains("status")) return false;
    c->format = j.value("format", std::string());
    c->variant = j.value("variant", std::string());
    c->task = j.value("task", size_t(0));
    c->status = j.value("status", std::string());
    c->cost = j.value("cost", uint64_t(0));
    c->size = j.value("result_size", uint64_t(0));
    c->sidecar = j.value("sidecar_size", uint64_t(0));
    c->has_tags = j.value("has_tags", false);
    c->wall_ms = j.value("wall_ms", uint64_t(0));
    c->cpu_ms = j.value("cpu_ms", uint64_t(0));
    c->prep_wall_ms = j.value("prep_wall_ms", uint64_t(0));
    c->decode_wall_ms = j.value("decode_wall_ms", uint64_t(0));
    c->verify = j.value("verify", std::string());
    c->error = j.value("error", std::string());
    c->retry = j.value("retry", false);
    return !c->format.empty();
}

json::json to_json(const Record& rec) {
    json::json src = {
        {"format", rec.source_format},
        {"codec_name", rec.codec_name},
        {"size", rec.source_size},
        {"channels", rec.channels},
        {"sample_rate", rec.sample_rate},
        {"bits", rec.bits},
        {"duration", rec.duration},
        {"has_tags", rec.has_tags},
        {"wav_size", rec.wav_size},
    };
    json::json cands = json::json::array();
    for (const auto& c : rec.candidates) cands.push_back(candidate_to_json(c));
    json::json excl = json::json::array();
    for (const auto& [fmt, variant] : rec.excluded)
        excl.push_back({{"format", fmt}, {"variant", variant}});
    json::json j = {
        {"ts", rec.ts},
        {"run_id", rec.run_id},
        {"file", rec.file},
        {"status", rec.status},
        {"source", src},
        {"candidates", cands},
        {"excluded", excl},
    };
    if (!rec.out_path.empty()) j["out_path"] = rec.out_path;
    if (!rec.detail.empty()) j["detail"] = rec.detail;
    if (rec.has_winner) {
        j["winner"] = {{"format", rec.winner_format},
                       {"variant", rec.winner_variant},
                       {"cost", rec.winner_cost}};
    }
    return j;
}

bool from_json(const json::json& j, Record* out) {
    if (!j.is_object()) return false;
    if (!j.contains("candidates") || !j["candidates"].is_array()) return false;
    out->ts = j.value("ts", std::string());
    out->run_id = j.value("run_id", std::string());
    out->file = j.value("file", std::string());
    out->out_path = j.value("out_path", std::string());
    out->status = j.value("status", std::string());
    out->detail = j.value("detail", std::string());
    if (j.contains("source") && j["source"].is_object()) {
        const auto& s = j["source"];
        out->source_format = s.value("format", std::string());
        out->codec_name = s.value("codec_name", std::string());
        out->source_size = s.value("size", uint64_t(0));
        out->channels = s.value("channels", 0);
        out->sample_rate = s.value("sample_rate", 0);
        out->bits = s.value("bits", 0);
        out->duration = s.value("duration", 0.0);
        out->has_tags = s.value("has_tags", false);
        out->wav_size = s.value("wav_size", uint64_t(0));
    }
    if (j.contains("excluded") && j["excluded"].is_array()) {
        for (const auto& e : j["excluded"]) {
            if (!e.is_object()) continue;
            out->excluded.push_back({e.value("format", std::string()),
                                     e.value("variant", std::string())});
        }
    }
    for (const auto& c : j["candidates"]) {
        optimize::Candidate cand;
        if (candidate_from_json(c, &cand)) out->candidates.push_back(std::move(cand));
    }
    if (j.contains("winner") && j["winner"].is_object()) {
        out->has_winner = true;
        out->winner_format = j["winner"].value("format", std::string());
        out->winner_variant = j["winner"].value("variant", std::string());
        out->winner_cost = j["winner"].value("cost", uint64_t(0));
    }
    return true;
}

namespace {

// Разбор журнала: одна строка — одна запись. Битые строки пропускаются, а не
// роняют базу: строка могла не дописаться при обрыве питания.
std::vector<Record> load_journal_file(const std::string& p) {
    std::vector<Record> out;
    std::string text = util::read_text(p);
    if (text.empty()) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        try {
            Record r;
            if (from_json(json::json::parse(line), &r)) out.push_back(std::move(r));
        } catch (const nlohmann::detail::parse_error&) {
            // Неразобранная строка — пропускаем.
        }
    }
    return out;
}

// Старая база целиком в одном JSON-массиве.
std::vector<Record> load_legacy_file(const std::string& p) {
    std::vector<Record> out;
    std::string text = util::read_text(p);
    if (text.empty()) return out;
    try {
        json::json data = json::json::parse(text);
        if (!data.is_array()) return out;
        for (const auto& item : data) {
            Record r;
            if (from_json(item, &r)) out.push_back(std::move(r));
        }
    } catch (const nlohmann::detail::parse_error&) {
        // Испорченный файл не блокируем.
    }
    return out;
}

}  // namespace

std::vector<Record> load() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<Record> out = load_journal_file(journal_path());
    // Журнала нет — читаем прежний stats.json, чтобы база, накопленная
    // прошлыми версиями, продолжала работать без ручной возни. Как только
    // журнал появится, он становится единственным источником.
    if (out.empty() && !util::file_exists(journal_path()))
        out = load_legacy_file(dump_path());
    return out;
}

bool append_all(const std::vector<json::json>& items) {
    if (items.empty()) return true;
    std::lock_guard<std::mutex> lk(g_mutex);
    std::string buf;
    for (const auto& it : items) {
        // Компактно и в одну строку: журнал читается построчно, а не как JSON.
        buf += it.dump();
        buf += '\n';
    }
    return util::append_text(journal_path(), buf);
}

bool write_dump(const std::string& dest) {
    std::vector<Record> recs = load();
    if (recs.empty()) return false;
    json::json arr = json::json::array();
    for (const auto& r : recs) arr.push_back(to_json(r));
    try {
        return util::write_text(dest, arr.dump(2));
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Итоговая таблица stats.tsv
// ---------------------------------------------------------------------------

std::string tsv_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': r += "\\\\"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            case '\n': r += "\\n"; break;
            default: r += c;
        }
    }
    return r;
}

std::string tsv_unescape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            r += s[i];
            continue;
        }
        char c = s[++i];
        if (c == 't') r += '\t';
        else if (c == 'r') r += '\r';
        else if (c == 'n') r += '\n';
        else r += c;
    }
    return r;
}

// Порядок столбцов-пар. Сортировка по формату, а внутри формата — по варианту
// с пониманием чисел: иначе «12» встал бы перед «3», и столбцы одного кодека
// шли бы вразнобой.
static bool variant_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        bool da = std::isdigit((unsigned char)a[i]) != 0;
        bool db = std::isdigit((unsigned char)b[j]) != 0;
        if (da && db) {
            size_t si = i, sj = j;
            while (i < a.size() && std::isdigit((unsigned char)a[i])) i++;
            while (j < b.size() && std::isdigit((unsigned char)b[j])) j++;
            // Длина числа важнее: 8 < 12, даже если 8 идёт позже по символам.
            if (i - si != j - sj) return (i - si) < (j - sj);
            if (a.compare(si, i - si, b, sj, j - sj) != 0)
                return a.compare(si, i - si, b, sj, j - sj) < 0;
            continue;
        }
        if (a[i] != b[j]) return a[i] < b[j];
        i++;
        j++;
    }
    return a.size() - i < b.size() - j;
}

static std::vector<std::string> pair_columns(const std::set<std::string>& keys) {
    std::vector<std::string> out(keys.begin(), keys.end());
    std::sort(out.begin(), out.end(), [](const std::string& x, const std::string& y) {
        size_t cx = x.find(':');
        size_t cy = y.find(':');
        std::string fx = cx == std::string::npos ? x : x.substr(0, cx);
        std::string fy = cy == std::string::npos ? y : y.substr(0, cy);
        if (fx != fy) return fx < fy;
        return variant_less(cx == std::string::npos ? x : x.substr(cx + 1),
                            cy == std::string::npos ? y : y.substr(cy + 1));
    });
    return out;
}

// Столбцы, одинаковые для всех строк. Их порядок — контракт файла: значения
// разных прогонов сопоставляются по имени, но читать таблицу глазами проще,
// когда свойства источника идут блоком, а размеры — после них.
static const char* const kFixedColumns[] = {
    "out_path",     "run_id",   "ts",         "runs",
    "source_format", "codec_name", "source_size", "bits",
    "channels",     "sample_rate", "duration_ms", "has_tags",
    "wav_size",     "winner_format", "winner_variant", "winner_cost",
    "winner_sidecar",
};
static const size_t kFixedCount = sizeof(kFixedColumns) / sizeof(kFixedColumns[0]);

static std::string cell_text(const Cell& c) {
    switch (c.state) {
        case Cell::State::NA: return "NA";
        case Cell::State::Value: return std::to_string(c.value);
        case Cell::State::Empty: break;
    }
    return std::string();  // пусто: пара не запускалась
}

// Слияние журнала в строки таблицы.
//
// Ключ строки — то, что осталось на диске: после отдачи файла движок заменяет
// исходник результатом, и путь исходника больше не существует, искать по нему
// бессмысленно. Если файл не был отдан (открыт как есть), ключ — исходник.
//
// Один файл может встретиться несколько раз: сначала с ошибкой, потом после
// починки. Таблица хранит самый полный результат — по каждой паре берётся
// последнее непустое значение, поэтому повторный перебор только упавших
// вариантов дополняет её, а не затирает.
static std::vector<Row> rows_from_records(const std::vector<Record>& recs) {
    struct Acc {
        Row row;
        std::string last_status;
        int runs = 0;
    };
    std::map<std::string, Acc> acc;
    std::set<std::string> keys_seen;

    for (const Record& r : recs) {
        std::string key = !r.out_path.empty() ? r.out_path : r.file;
        if (key.empty()) continue;
        Acc& a = acc[key];
        ++a.runs;

        Row& row = a.row;
        row.out_path = key;
        row.run_id = r.run_id;
        row.ts = r.ts;
        row.runs = a.runs;
        row.source_format = r.source_format;
        row.codec_name = r.codec_name;
        row.source_size = r.source_size;
        row.bits = r.bits;
        row.channels = r.channels;
        row.sample_rate = r.sample_rate;
        row.duration_ms = (uint64_t)(r.duration * 1000.0 + 0.5);
        row.has_tags = r.has_tags;
        row.wav_size = r.wav_size;
        row.has_winner = r.has_winner;
        row.winner_format = r.winner_format;
        row.winner_variant = r.winner_variant;
        row.winner_cost = r.winner_cost;
        row.winner_sidecar = 0;
        if (r.has_winner) {
            for (const auto& c : r.candidates) {
                if (c.format == r.winner_format && c.variant == r.winner_variant) {
                    row.winner_sidecar = c.sidecar;
                    break;
                }
            }
        }

        // Отсечение по caps кодека — не сбой, а «здесь формат неприменим».
        for (const auto& [fmt, variant] : r.excluded)
            row.cells[fmt + ":" + variant] = Cell{Cell::State::NA, 0};

        // Лучший успешный кандидат пары.
        std::map<std::string, uint64_t> best;
        for (const auto& c : r.candidates) {
            if (c.status != "ok" || c.cost == 0) continue;
            std::string k = c.format + ":" + c.variant;
            auto it = best.find(k);
            if (it == best.end() || c.cost < it->second) best[k] = c.cost;
        }
        for (const auto& [k, v] : best) {
            keys_seen.insert(k);
            row.cells[k] = Cell{Cell::State::Value, v};
        }
        for (const auto& [fmt, variant] : r.excluded) keys_seen.insert(fmt + ":" + variant);

        a.last_status = r.status;
    }

    std::vector<Row> out;
    for (auto& [key, a] : acc) {
        // Незавершённое в статистику не идёт: промежуточные состояния и ошибки
        // остаются в журнале, а таблица — это итог.
        if (a.last_status != "ok") continue;
        out.push_back(std::move(a.row));
    }
    // Порядок строк по ключу: две выгрузки одного состояния дают одинаковый файл,
    // и diff не показывает ложных изменений.
    std::sort(out.begin(), out.end(),
              [](const Row& x, const Row& y) { return x.out_path < y.out_path; });
    (void)keys_seen;
    return out;
}

std::vector<std::string> tsv_columns(const std::vector<Row>& rows, size_t* fixed_count) {
    std::set<std::string> keys;
    for (const auto& row : rows)
        for (const auto& [k, c] : row.cells)
            if (c.state != Cell::State::Empty) keys.insert(k);
    std::vector<std::string> out;
    for (size_t i = 0; i < kFixedCount; i++) out.push_back(kFixedColumns[i]);
    for (const auto& k : pair_columns(keys)) out.push_back(k);
    if (fixed_count) *fixed_count = kFixedCount;
    return out;
}

std::string build_tsv(const std::vector<Row>& rows) {
    size_t fixed = 0;
    std::vector<std::string> cols = tsv_columns(rows, &fixed);
    std::string r;
    // Шапка-комментарий: файл должен быть самодостаточным. Число прогонов и
    // строк — чтобы по выгрузке было видно, откуда она взята, не открывая журнал.
    std::set<std::string> runs;
    for (const auto& row : rows)
        if (!row.run_id.empty()) runs.insert(row.run_id);
    r += "# llao stats export v1";
    r += " | generated " + now_iso();
    r += " | rows " + std::to_string(rows.size());
    r += " | runs " + std::to_string(runs.size());
    r += "\n";
    for (size_t i = 0; i < cols.size(); i++) {
        if (i) r += '\t';
        r += cols[i];
    }
    r += "\n";

    for (const Row& row : rows) {
        char buf[64];
        std::vector<std::string> f(fixed);
        f[0] = tsv_escape(row.out_path);
        f[1] = row.run_id;
        f[2] = row.ts;
        snprintf(buf, sizeof(buf), "%d", row.runs);
        f[3] = buf;
        f[4] = row.source_format;
        f[5] = row.codec_name;
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)row.source_size);
        f[6] = buf;
        snprintf(buf, sizeof(buf), "%d", row.bits);
        f[7] = buf;
        snprintf(buf, sizeof(buf), "%d", row.channels);
        f[8] = buf;
        snprintf(buf, sizeof(buf), "%d", row.sample_rate);
        f[9] = buf;
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)row.duration_ms);
        f[10] = buf;
        f[11] = row.has_tags ? "1" : "0";
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)row.wav_size);
        f[12] = buf;
        f[13] = row.has_winner ? row.winner_format : std::string();
        f[14] = row.has_winner ? row.winner_variant : std::string();
        if (row.has_winner) {
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)row.winner_cost);
            f[15] = buf;
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)row.winner_sidecar);
            f[16] = buf;
        } else {
            f[15] = std::string();
            f[16] = std::string();
        }
        for (size_t i = 0; i < fixed; i++) {
            if (i) r += '\t';
            r += f[i];
        }
        for (size_t i = fixed; i < cols.size(); i++) {
            auto it = row.cells.find(cols[i]);
            r += '\t';
            r += it == row.cells.end() ? std::string() : cell_text(it->second);
        }
        r += "\n";
    }
    return r;
}

bool export_tsv(const std::string& dest, std::string* err) {
    std::vector<Record> recs = load();
    std::vector<Row> rows = rows_from_records(recs);
    if (rows.empty()) {
        if (err) *err = "nothing to export: no finished files in the journal";
        return false;
    }
    std::string text = build_tsv(rows);
    if (!util::write_text(dest, text)) {
        if (err) *err = "could not write " + dest;
        return false;
    }
    return true;
}

std::vector<Row> load_tsv() {
    std::vector<Row> rows;
    std::string text = util::read_text(tsv_path());
    if (text.empty()) return rows;
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        lines.push_back(line);
    }
    if (lines.empty()) return rows;

    std::vector<std::string> cols = util::split(lines[0], '\t');
    std::set<std::string> fixed_names;
    for (size_t i = 0; i < kFixedCount; i++) fixed_names.insert(kFixedColumns[i]);
    // Столбцы-пары опознаются по имени, а не по позиции: так таблица читается
    // и после того, как в неё вручную добавили столбец.
    std::map<std::string, size_t> fixed_at;
    for (size_t i = 0; i < cols.size(); i++) {
        if (fixed_names.count(cols[i])) fixed_at[cols[i]] = i;
    }

    for (size_t li = 1; li < lines.size(); li++) {
        std::vector<std::string> v = util::split(lines[li], '\t');
        Row row;
        auto get = [&](const char* name) -> std::string {
            auto it = fixed_at.find(name);
            if (it == fixed_at.end() || it->second >= v.size()) return std::string();
            return v[it->second];
        };
        auto num = [&](const char* name) -> uint64_t {
            std::string s = get(name);
            if (s.empty()) return 0;
            return (uint64_t)std::strtoull(s.c_str(), nullptr, 10);
        };
        row.out_path = tsv_unescape(get("out_path"));
        row.run_id = get("run_id");
        row.ts = get("ts");
        row.runs = (int)num("runs");
        row.source_format = get("source_format");
        row.codec_name = get("codec_name");
        row.source_size = num("source_size");
        row.bits = (int)num("bits");
        row.channels = (int)num("channels");
        row.sample_rate = (int)num("sample_rate");
        row.duration_ms = num("duration_ms");
        row.has_tags = get("has_tags") == "1";
        row.wav_size = num("wav_size");
        row.winner_format = get("winner_format");
        row.winner_variant = get("winner_variant");
        row.winner_cost = num("winner_cost");
        row.winner_sidecar = num("winner_sidecar");
        row.has_winner = !row.winner_format.empty();
        for (size_t ci = 0; ci < cols.size(); ci++) {
            if (fixed_names.count(cols[ci])) continue;
            if (ci >= v.size()) continue;
            std::string s = v[ci];
            if (s.empty()) continue;
            if (s == "NA") row.cells[cols[ci]] = Cell{Cell::State::NA, 0};
            else row.cells[cols[ci]] =
                     Cell{Cell::State::Value, (uint64_t)std::strtoull(s.c_str(), nullptr, 10)};
        }
        if (row.out_path.empty()) continue;
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Сводки по таблице
// ---------------------------------------------------------------------------

// Общие числа по строкам. Считаем по cost (файл + sidecar): именно эта величина
// сравнивается при выборе победителя, поэтому сводка и рейтинг обязаны считать
// одно и то же. Раньше сводка суммировала result_size, а рейтинг считал по cost,
// и в отчёте это выглядело как ошибка арифметики.
struct Totals {
    uint64_t in = 0;
    uint64_t out = 0;
    int winners = 0;
};

// Итоги считаются по тому же, что и рейтинг: без lossy-исходников. Иначе строка
// «Total size» включала бы mp3, конвертация которого в lossless только увеличивает
// размер, и сводка показывала бы экономию, которой не было.
static bool lossy_source(const Row& r) {
    return !r.codec_name.empty() && !media::codec_is_lossless(r.codec_name);
}

static Totals totals_of(const std::vector<Row>& rows) {
    Totals t;
    for (const Row& r : rows) {
        if (lossy_source(r)) continue;
        t.in += r.source_size;
        if (r.has_winner) {
            t.out += r.winner_cost;
            ++t.winners;
        }
    }
    return t;
}

void print_summary(const std::vector<Row>& rows) {
    if (rows.empty()) {
        out::print("%s\n",
                   i18n::str("No statistics yet (no optimizations run).").c_str());
        return;
    }
    out::print("%s\n", i18n::fmt("Files: %zu", rows.size()).c_str());

    std::set<std::string> runs;
    Totals tot;
    for (const Row& r : rows) {
        if (!r.run_id.empty()) runs.insert(r.run_id);
        if (lossy_source(r)) continue;
        tot.in += r.source_size;
        if (r.has_winner) {
            tot.out += r.winner_cost;
            ++tot.winners;
        }
    }

    out::print("%s\n", i18n::fmt("Runs: %zu", runs.size()).c_str());
    out::print("%s\n", i18n::fmt("Files replaced: %d", tot.winners).c_str());
    if (tot.in > 0) {
        double ratio = tot.out > 0 ? (double)tot.out / (double)tot.in : 0.0;
        out::print("Total source size: %.2f MB, result: %.2f MB (%.2f%%)\n",
                   tot.in / 1048576.0, tot.out / 1048576.0, ratio * 100.0);
    }
    out::print("%s\n", tsv_path().c_str());

    std::vector<Rank> ranks = ranking(rows);
    if (ranks.empty()) return;
    out::print("\nFormat ranking (files won, most likely winners first):\n");
    out::print("  %-16s %-12s %-8s %-10s\n", i18n::str("format").c_str(),
               i18n::str("savings").c_str(), i18n::str("files").c_str(),
               i18n::str("sizes").c_str());
    for (const auto& r : ranks) {
        out::print("  %-16s %6.2f%%  %7d  %8.2f -> %8.2f %s\n", r.format.c_str(),
                   r.savings * 100.0, r.files, r.total_in / 1048576.0,
                   r.total_out / 1048576.0, i18n::str("MB").c_str());
    }

    // Гистограммы по всем файлам: распределение экономии показывает, насколько
    // однороден материал, а по размерам — чем он вообще является. Рейтинг выше
    // даёт среднее по формату, здесь видно, из чего оно сложилось.
    out::print("\n%s\n", i18n::str("Savings distribution (files won):").c_str());
    out::print("%s", histogram_text("", savings_histogram(rows)).c_str());
    out::print("\n%s\n", i18n::str("Source size distribution:").c_str());
    out::print("%s", histogram_text("", size_histogram(rows)).c_str());
}

bool write_report(const std::string& dest, const std::vector<Row>& rows) {
    std::string text = build_report(rows);
    if (text.empty()) return false;
    return util::write_text(dest, text);
}

std::string build_report(const std::vector<Row>& rows) {
    if (rows.empty()) return std::string();

    Totals tot = totals_of(rows);
    std::string r;
    r += "LLAO — format statistics\n";
    r += "Source: " + tsv_path() + "\n";
    r += "Files: " + std::to_string(rows.size()) + "\n";
    r += "Files replaced: " + std::to_string(tot.winners) + "\n";
    if (tot.in > 0) {
        char buf[160];
        double ratio = tot.out > 0 ? (double)tot.out / (double)tot.in : 0.0;
        snprintf(buf, sizeof(buf), "Total size: %.2f MB -> %.2f MB (%.2f%% of source)\n",
                 tot.in / 1048576.0, tot.out / 1048576.0, ratio * 100.0);
        r += buf;
    }

    // Рейтинг — по файлам, а не по кандидатам: выигрыш одного файла засчитывается
    // формату, который этот файл дал, независимо от числа его вариантов.
    std::vector<Rank> ranks = ranking(rows);
    if (!ranks.empty()) {
        r += "\nFormat ranking (savings on files won):\n";
        r += "  format            savings  files-won        size (MB)\n";
        for (const auto& rk : ranks) {
            char buf[192];
            snprintf(buf, sizeof(buf), "  %-16s %6.2f%% %9d  %8.2f -> %8.2f\n",
                     rk.format.c_str(), rk.savings * 100.0, rk.files,
                     rk.total_in / 1048576.0, rk.total_out / 1048576.0);
            r += buf;
        }

        // Гистограммы идут в отчёт без локализации — там только числа, метки
        // бинов и id форматов, чтобы выгрузку можно было отдать как есть.
        r += "\n";
        r += histogram_text("Savings distribution (files won):", savings_histogram(rows));
        r += "\n";
        r += histogram_text("Source size distribution:", size_histogram(rows));

        // По каждому формату — отдельно, чтобы видеть, у кого разброс узкий, а
        // у кого один выигранный файл вытягивает среднее.
        std::map<std::string, uint64_t> files_by_fmt;
        for (const auto& rk : ranks) files_by_fmt[rk.format] += (uint64_t)rk.files;
        for (const auto& [fmt, _n] : files_by_fmt) {
            r += "\n";
            r += histogram_text("Savings distribution: " + fmt, savings_histogram(rows, fmt));
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Гистограммы
// ---------------------------------------------------------------------------

// Проходит по строкам, отдавая выигранный cost. Фильтры те же, что и у
// рейтинга: без lossy-исходников и без файлов, которые не были отданы.
template <typename F>
static void for_each_won(const std::vector<Row>& rows, const std::string& fmt, F fn) {
    for (const auto& r : rows) {
        if (!r.has_winner || r.winner_cost == 0 || r.source_size == 0) continue;
        if (lossy_source(r)) continue;
        if (!fmt.empty() && r.winner_format != fmt) continue;
        fn(r);
    }
}

// Границы бинов экономии. Последний бин — «вырос»: провал сжатия не должен
// теряться среди нормальных значений, у него экономия отрицательная.
static const double kSavingsEdges[] = {0.10, 0.20, 0.30, 0.40, 0.50,
                                       0.60, 0.70, 0.80, 0.90};

std::vector<HistBin> savings_histogram(const std::vector<Row>& rows,
                                       const std::string& fmt) {
    std::vector<HistBin> bins;
    double prev = 0.0;
    for (double e : kSavingsEdges) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%2.0f-%2.0f%%", prev * 100, e * 100);
        bins.push_back({buf, 0});
        prev = e;
    }
    bins.push_back({"90-100%", 0});
    bins.push_back({"grew", 0});

    for_each_won(rows, fmt, [&](const Row& r) {
        double s = 1.0 - (double)r.winner_cost / (double)r.source_size;
        if (s < 0.0) {
            ++bins.back().count;
            return;
        }
        if (s >= 1.0) s = 0.999999;
        size_t idx = 0;
        while (idx < sizeof(kSavingsEdges) / sizeof(kSavingsEdges[0]) &&
               s >= kSavingsEdges[idx])
            ++idx;
        ++bins[idx].count;
    });
    return bins;
}

std::vector<HistBin> size_histogram(const std::vector<Row>& rows, const std::string& fmt) {
    // Мощная шкала: 1 МБ, потом 2-4, 4-8, ... Для музыкальной библиотеки это
    // читаемее, чем равномерные килобайты: файлы разбросаны по порядкам.
    std::vector<HistBin> bins;
    const uint64_t mb = 1048576;
    bins.push_back({"<1 MB", 0});
    // Цикл до 32: последний закрытый бин 32-63 МБ, дальше открытый «64+ МБ».
    for (uint64_t lo = 2; lo <= 32; lo *= 2) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%llu-%llu MB", (unsigned long long)lo,
                 (unsigned long long)(lo * 2 - 1));
        bins.push_back({buf, 0});
    }
    // Последний бин открытый: «64+ MB», а не «64-127» — иначе он накрывается
    // следующим и файлы от 100 МБ попадают в два бина сразу.
    bins.push_back({"64+ MB", 0});

    for_each_won(rows, fmt, [&](const Row& r) {
        uint64_t s = r.source_size / mb;
        size_t idx = 0;
        if (s >= 1) {
            idx = 1;
            for (uint64_t lo = 2; lo <= 32 && s >= lo * 2; lo *= 2) ++idx;
            if (idx >= bins.size()) idx = bins.size() - 1;
        }
        ++bins[idx].count;
    });
    return bins;
}

std::string histogram_text(const std::string& title, const std::vector<HistBin>& bins) {
    // Пустой заголовок — вызывающий уже напечатал свой (в терминале он локализован).
    std::string r = title.empty() ? std::string() : title + "\n";
    int maxc = 0;
    for (const auto& b : bins) maxc = std::max(maxc, b.count);
    if (maxc == 0) {
        r += "  (no files)\n";
        return r;
    }
    for (const auto& b : bins) {
        // Ширина столбика нормирована на максимальный бин, иначе редкий класс
        // вроде «вырос» не виден вовсе.
        int n = maxc > 0 ? (int)((int64_t)b.count * 50 / maxc) : 0;
        char buf[160];
        snprintf(buf, sizeof(buf), "  %-10s %7d  %s\n", b.label.c_str(), b.count,
                 std::string(n, '#').c_str());
        r += buf;
    }
    return r;
}

std::vector<Rank> ranking(const std::vector<Row>& rows) {
    struct Agg {
        double sum = 0.0;
        int n = 0;
        uint64_t in = 0, out = 0;
    };
    std::map<std::string, Agg> agg;
    for (const auto& r : rows) {
        // Победитель записан явно; строки без него (файл не отдан) в рейтинг не
        // идут — иначе формат получал бы «победу» за провал.
        if (!r.has_winner || r.winner_cost == 0 || r.source_size == 0) continue;
        // Lossy-источники (mp3 и т.п.) в ранжировании не участвуют: их конвертация
        // в lossless всегда увеличивает размер и искажает оценку форматов.
        if (lossy_source(r)) continue;
        Agg& a = agg[r.winner_format];
        a.sum += 1.0 - (double)r.winner_cost / (double)r.source_size;
        a.n++;
        a.in += r.source_size;
        a.out += r.winner_cost;
    }
    std::vector<Rank> res;
    for (const auto& [fmt, a] : agg) {
        res.push_back({fmt, a.n ? a.sum / a.n : 0.0, a.n, a.in, a.out});
    }
    std::sort(res.begin(), res.end(), [](const Rank& x, const Rank& y) {
        if (x.savings != y.savings) return x.savings > y.savings;
        return x.files > y.files;
    });
    return res;
}

// ---------------------------------------------------------------------------
// Сводка эффективности методов
// ---------------------------------------------------------------------------

// Границы корзин длительности. Треки сильно различаются по длине (от нескольких
// секунд до получаса), а короткие и длинные сжимаются разными методами по-
// разному, поэтому «длительность» — такой же фильтр, как разрядность.
static const uint64_t kDurationBucketEdges[] = {
    60 * 1000,        // 1 мин
    5 * 60 * 1000,    // 5 мин
    10 * 60 * 1000,   // 10 мин
    20 * 60 * 1000,   // 20 мин
};

int duration_bucket_count() {
    return (int)(sizeof(kDurationBucketEdges) / sizeof(kDurationBucketEdges[0])) + 1;
}

int duration_bucket_of(uint64_t duration_ms) {
    for (int i = 0; i < duration_bucket_count() - 1; i++)
        if (duration_ms < kDurationBucketEdges[i]) return i;
    return duration_bucket_count() - 1;
}

uint64_t duration_bucket_lower_ms(int bucket) {
    if (bucket <= 0) return 0;
    if (bucket > duration_bucket_count() - 1) bucket = duration_bucket_count() - 1;
    return kDurationBucketEdges[bucket - 1];
}

// Ключ ячейки — "формат:вариант". Имя формата доходит до первой двоеточия.
static std::string cell_format(const std::string& key) {
    size_t p = key.find(':');
    return p == std::string::npos ? key : key.substr(0, p);
}

// Имя и семейство кодека берутся из formats/*.json, а не зашиваются здесь:
// подпись оси и группировка фоном не должны ломаться от нового формата.
// Семейство — это engine.kind: «binary» (своя утилита) или «ffmpeg».
struct FormatMeta {
    std::string name;
    std::string family;
};
static std::map<std::string, FormatMeta> load_format_meta() {
    std::map<std::string, FormatMeta> out;
    std::vector<config::Format> fmts;
    try {
        fmts = config::load_all();
    } catch (const std::exception&) {
        return out;  // конфиг битый — вернём id без подписи, диаграмма не сломается
    }
    for (const auto& f : fmts) out[f.id] = {f.name, f.engine_kind};
    return out;
}

// skip — измерение, значение которого в фильтре игнорируется (для счётчиков
// граней), либо -1, когда применяются все условия.
static bool filter_matches(const Row& r, const SummaryFilter& f, int skip) {
    if (f.bits != 0 && skip != (int)SummaryFacet::Bits && r.bits != f.bits) return false;
    if (f.sample_rate != 0 && skip != (int)SummaryFacet::SampleRate &&
        r.sample_rate != f.sample_rate)
        return false;
    if (f.channels != 0 && skip != (int)SummaryFacet::Channels && r.channels != f.channels)
        return false;
    if (f.duration_bucket >= 0 && skip != (int)SummaryFacet::Duration &&
        duration_bucket_of(r.duration_ms) != f.duration_bucket)
        return false;
    return true;
}

Summary summarize(const std::vector<Row>& rows, const SummaryFilter& f) {
    Summary s;
    s.generated = now_iso();
    s.wav_denominator = f.wav_denominator;

    // Форматы берём из данных: в таблице есть ячейки каждого метода, который
    // реально отработал, плюс отсечённые по ограничениям кодека.
    std::map<std::string, MethodSummary> agg;
    for (const auto& r : rows) {
        s.files++;
        for (const auto& [key, cell] : r.cells) {
            (void)cell;
            agg[cell_format(key)];  // создаёт пустую запись, если её не было
        }
    }
    // Σ savings² по каждому методу — только для расчёта σ.
    std::map<std::string, double> sumsq;

    // Свод по заданиям. Список собирается из таблицы: у каждой ячейки есть
    // ключ «формат:вариант», и новый вариант из formats/*.json попадёт сюда сам.
    std::map<std::string, VariantSummary> vagg;
    std::map<std::string, double> vsumsq;
    auto slot_for = [&vagg](const std::string& key) -> VariantSummary& {
        auto it = vagg.find(key);
        if (it != vagg.end()) return it->second;
        VariantSummary v;
        v.key = key;
        v.format = cell_format(key);
        const size_t p = key.find(':');
        v.variant = p == std::string::npos ? std::string() : key.substr(p + 1);
        return vagg.emplace(key, v).first->second;
    };

    for (const auto& r : rows) {
        if (!filter_matches(r, f, -1)) continue;
        s.in_sample++;

        // Знаменатель экономии. У wav-исходников wav_size может быть нулевым
        // (файл не читался) — тогда честно падаем на размер исходника.
        uint64_t denom = r.source_size;
        if (f.wav_denominator && r.wav_size != 0) denom = r.wav_size;
        if (denom == 0) continue;

        // Лучший результат каждого метода на этом треке + факт отсечения.
        std::map<std::string, uint64_t> best;
        std::set<std::string> na;
        for (const auto& [key, cell] : r.cells) {
            const std::string fmt = cell_format(key);
            if (cell.state == Cell::State::NA) {
                na.insert(fmt);
                continue;
            }
            if (cell.state != Cell::State::Value || cell.value == 0) continue;
            auto it = best.find(fmt);
            if (it == best.end() || cell.value < it->second) best[fmt] = cell.value;
        }

        for (const auto& [key, cell] : r.cells) {
            if (cell.state != Cell::State::Value || cell.value == 0) continue;
            VariantSummary& v = slot_for(key);
            const double sv = 1.0 - (double)cell.value / (double)denom;
            v.considered++;
            v.total_in += denom;
            v.total_out += cell.value;
            if (v.considered == 1) v.min = v.max = sv;
            v.min = std::min(v.min, sv);
            v.max = std::max(v.max, sv);
            vsumsq[key] += sv * sv;
            int b = (int)(sv * 20.0);
            if (b < 0) b = kSummaryHistGrew;
            if (b > kSummaryHistBins - 1) b = kSummaryHistBins - 1;
            v.hist[b]++;
            if (r.has_winner && r.winner_format == v.format &&
                r.winner_variant == v.variant)
                v.wins++;
        }

        for (auto& [fmt, m] : agg) {
            auto it = best.find(fmt);
            if (it != best.end()) {
                const double v = 1.0 - (double)it->second / (double)denom;
                m.considered++;
                m.total_in += denom;
                m.total_out += it->second;
                if (m.considered == 1) m.min = m.max = v;
                m.min = std::min(m.min, v);
                m.max = std::max(m.max, v);
                sumsq[fmt] += v * v;
                // Гистограмма распределения: по 5 % на бин, файлы, которые
                // метод увеличил, в отдельный счётчик — иначе они потерялись бы
                // среди нормальных значений.
                int bin = (int)(v * 20.0);
                if (bin < 0) bin = kSummaryHistGrew;
                if (bin > kSummaryHistBins - 1) bin = kSummaryHistBins - 1;
                m.hist[bin]++;
            } else if (na.count(fmt)) {
                m.not_applicable++;
            }
        }
        if (r.has_winner && !r.winner_format.empty() && best.count(r.winner_format))
            agg[r.winner_format].wins++;
    }

    for (auto& [fmt, m] : agg) {
        m.format = fmt;
        m.mean = m.considered ? 1.0 - (double)m.total_out / (double)m.total_in : 0.0;
        // σ для подсказки на диаграмме. Сумма квадратов копится отдельно: в
        // публичной структуре она не нужна, а из огрублённых бинов точной σ не
        // получить.
        if (m.considered > 1) {
            const double var = sumsq[fmt] / (double)m.considered - m.mean * m.mean;
            m.stddev = var > 0.0 ? std::sqrt(var) : 0.0;
        }
        s.methods.push_back(m);
    }

    const std::map<std::string, FormatMeta> meta = load_format_meta();
    for (auto& [key, v] : vagg) {
        v.mean = v.considered ? 1.0 - (double)v.total_out / (double)v.total_in : 0.0;
        if (v.considered > 1) {
            const double var = vsumsq[key] / (double)v.considered - v.mean * v.mean;
            v.stddev = var > 0.0 ? std::sqrt(var) : 0.0;
        }
        auto mi = meta.find(v.format);
        if (mi != meta.end()) {
            v.name = mi->second.name;
            v.family = mi->second.family;
        } else {
            v.name = v.format;
        }
        s.variants.push_back(v);
    }
    // Методы без результата в выборке в диаграмме бесполезны: они нарисовали бы
    // свечу в нуле. Но их нужно видеть, иначе фильтр не объяснит, почему
    // формат пропал — поэтому оставляем, сортировка уведёт их вниз.
    std::sort(s.methods.begin(), s.methods.end(),
              [](const MethodSummary& x, const MethodSummary& y) {
                  if (x.considered != y.considered) return x.considered > y.considered;
                  if (x.mean != y.mean) return x.mean > y.mean;
                  return x.format < y.format;
              });
    return s;
}

std::vector<Row> load_rows() { return rows_from_records(load()); }

std::string summary_json(const std::vector<Row>& rows, const SummaryFilter& f) {
    nlohmann::json j;
    const Summary s = summarize(rows, f);
    j["generated"] = s.generated;
    j["files"] = s.files;
    j["in_sample"] = s.in_sample;
    j["denominator"] = f.wav_denominator ? "wav_size" : "source_size";
    nlohmann::json fl = nlohmann::json::object();
    fl["bits"] = f.bits;
    fl["sample_rate"] = f.sample_rate;
    fl["channels"] = f.channels;
    fl["duration_bucket"] = f.duration_bucket;
    j["filter"] = fl;

    nlohmann::json dur = nlohmann::json::array();
    for (int i = 0; i < duration_bucket_count(); i++) {
        nlohmann::json b = nlohmann::json::object();
        b["id"] = i;
        b["from_ms"] = duration_bucket_lower_ms(i);
        dur.push_back(b);
    }
    j["duration_buckets"] = dur;

    const std::map<std::string, FormatMeta> meta = load_format_meta();
    nlohmann::json ms = nlohmann::json::array();
    for (const auto& m : s.methods) {
        auto mi = meta.find(m.format);
        nlohmann::json o = nlohmann::json::object();
        o["format"] = m.format;
        o["name"] = mi != meta.end() && !mi->second.name.empty() ? mi->second.name : m.format;
        o["family"] = mi != meta.end() ? mi->second.family : std::string();
        o["considered"] = m.considered;
        o["not_applicable"] = m.not_applicable;
        o["wins"] = m.wins;
        o["mean"] = m.mean;
        o["stddev"] = m.stddev;
        o["min"] = m.min;
        o["max"] = m.max;
        nlohmann::json hb = nlohmann::json::array();
        for (int i = 0; i < kSummaryHistBins; i++) hb.push_back(m.hist[i]);
        nlohmann::json h = nlohmann::json::object();
        h["bins"] = hb;                    // по 5 %, от 0 до 100
        h["grew"] = m.hist[kSummaryHistGrew];  // метод увеличил файл
        o["hist"] = h;
        ms.push_back(o);
    }
    j["methods"] = ms;

    // Полный список значений по грани без учёта фильтра. Веб рисует кнопки по
    // нему, а не по facet_counts: иначе при переключении фильтра часть кнопок
    // исчезала, а у оставшихся менялся счётчик в подписи — и вся полоса
    // переставлялась. Здесь набор кнопок всегда один и тот же.
    nlohmann::json allv = nlohmann::json::array();
    const SummaryFacet all_order[] = {SummaryFacet::Bits, SummaryFacet::SampleRate,
                                      SummaryFacet::Channels, SummaryFacet::Duration};
    for (SummaryFacet fc : all_order) {
        nlohmann::json o = nlohmann::json::object();
        o["facet"] = fc == SummaryFacet::Bits       ? "bits"
                     : fc == SummaryFacet::SampleRate ? "sample_rate"
                     : fc == SummaryFacet::Channels   ? "channels"
                                                      : "duration";
        std::set<int> seen;
        for (const auto& r : rows) {
            switch (fc) {
                case SummaryFacet::Bits: seen.insert(r.bits); break;
                case SummaryFacet::SampleRate: seen.insert(r.sample_rate); break;
                case SummaryFacet::Channels: seen.insert(r.channels); break;
                case SummaryFacet::Duration: seen.insert(duration_bucket_of(r.duration_ms)); break;
            }
        }
        nlohmann::json vs = nlohmann::json::array();
        for (int v : seen) {
            nlohmann::json e = nlohmann::json::object();
            e["value"] = v;
            vs.push_back(e);
        }
        o["values"] = vs;
        allv.push_back(o);
    }
    j["facet_values"] = allv;

    nlohmann::json fs = nlohmann::json::array();
    for (const auto& fc : facet_counts(rows, f)) {
        nlohmann::json o = nlohmann::json::object();
        o["facet"] = fc.facet == SummaryFacet::Bits       ? "bits"
                     : fc.facet == SummaryFacet::SampleRate ? "sample_rate"
                     : fc.facet == SummaryFacet::Channels   ? "channels"
                                                            : "duration";
        nlohmann::json vs = nlohmann::json::array();
        for (const auto& v : fc.values) {
            nlohmann::json e = nlohmann::json::object();
            e["value"] = v.value;
            e["files"] = v.files;
            vs.push_back(e);
        }
        o["values"] = vs;
        fs.push_back(o);
    }
    j["facets"] = fs;

    nlohmann::json vs = nlohmann::json::array();
    for (const auto& v : s.variants) {
        nlohmann::json o = nlohmann::json::object();
        o["key"] = v.key;
        o["format"] = v.format;
        o["variant"] = v.variant;
        o["name"] = v.name;
        o["family"] = v.family;
        o["considered"] = v.considered;
        o["wins"] = v.wins;
        o["mean"] = v.mean;
        o["stddev"] = v.stddev;
        o["min"] = v.min;
        o["max"] = v.max;
        nlohmann::json hb = nlohmann::json::array();
        for (int i = 0; i < kSummaryHistBins; i++) hb.push_back(v.hist[i]);
        nlohmann::json h = nlohmann::json::object();
        h["bins"] = hb;
        h["grew"] = v.hist[kSummaryHistGrew];
        o["hist"] = h;
        vs.push_back(o);
    }
    j["variants"] = vs;
    return j.dump();
}

std::vector<FacetCounts> facet_counts(const std::vector<Row>& rows, const SummaryFilter& f) {
    const SummaryFacet order[] = {SummaryFacet::Bits, SummaryFacet::SampleRate,
                                  SummaryFacet::Channels, SummaryFacet::Duration};
    std::vector<FacetCounts> out;
    for (SummaryFacet fc : order) {
        std::map<int, int> counts;
        for (const auto& r : rows) {
            if (!filter_matches(r, f, (int)fc)) continue;
            int v = 0;
            switch (fc) {
                case SummaryFacet::Bits: v = r.bits; break;
                case SummaryFacet::SampleRate: v = r.sample_rate; break;
                case SummaryFacet::Channels: v = r.channels; break;
                case SummaryFacet::Duration: v = duration_bucket_of(r.duration_ms); break;
            }
            counts[v]++;
        }
        FacetCounts c;
        c.facet = fc;
        for (const auto& [v, n] : counts) c.values.push_back({v, n});
        out.push_back(c);
    }
    return out;
}

SummaryFilter summary_filter_from_query(const std::string& query) {
    SummaryFilter f;
    auto num = [&query](const char* key) -> int {
        std::string k = std::string(key) + "=";
        size_t p = query.find(k);
        if (p == std::string::npos) return 0;
        p += k.size();
        size_t amp = query.find('&', p);
        std::string v = query.substr(p, amp == std::string::npos ? amp : amp - p);
        if (v.empty()) return 0;
        for (char ch : v)
            if (!std::isdigit(static_cast<unsigned char>(ch))) return 0;
        return std::atoi(v.c_str());
    };
    f.bits = num("bits");
    f.sample_rate = num("rate");
    f.channels = num("ch");
    // -1, а не 0: корзина 0 («до минуты») — полноценное значение, и её нельзя
    // спутать с «фильтр не задан».
    f.duration_bucket = -1;
    std::string k = "dur=";
    size_t p = query.find(k);
    if (p != std::string::npos) {
        p += k.size();
        size_t amp = query.find('&', p);
        std::string v = query.substr(p, amp == std::string::npos ? amp : amp - p);
        bool ok = !v.empty();
        for (char ch : v)
            if (!std::isdigit(static_cast<unsigned char>(ch))) ok = false;
        if (ok) f.duration_bucket = std::atoi(v.c_str());
    }
    // По умолчанию — несжатый оригинал; wav=0 переключает на размер на диске.
    f.wav_denominator = query.find("wav=0") == std::string::npos;
    return f;
}

}  // namespace stats
