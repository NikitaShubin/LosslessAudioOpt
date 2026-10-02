#include "stats.h"

#include <algorithm>
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
    };
    json::json cands = json::json::array();
    for (const auto& c : rec.candidates) cands.push_back(candidate_to_json(c));
    json::json j = {
        {"ts", rec.ts},
        {"run_id", rec.run_id},
        {"file", rec.file},
        {"status", rec.status},
        {"source", src},
        {"candidates", cands},
    };
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

std::vector<json::json> load() {
    std::vector<json::json> out;
    std::string text = util::read_text(path());
    if (text.empty()) return out;
    try {
        json::json data = json::json::parse(text);
        if (data.is_array()) {
            for (const auto& item : data) out.push_back(item);
        }
    } catch (const nlohmann::detail::parse_error&) {
        // Испорченный файл не блокируем; перезапишем при следующем append.
    }
    return out;
}

bool append_all(const std::vector<json::json>& items) {
    if (items.empty()) return true;
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<json::json> all = load();
    all.insert(all.end(), items.begin(), items.end());
    try {
        json::json arr(all);
        std::string text = arr.dump(2);
        return util::write_text(path(), text);
    } catch (...) {
        return false;
    }
}

// Общие числа по записям. Считаем по cost (файл + sidecar): именно эта величина
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
static bool lossy_source(const Record& r) {
    return !r.codec_name.empty() && !media::codec_is_lossless(r.codec_name);
}

static Totals totals_of(const std::vector<json::json>& items) {
    Totals t;
    for (const auto& it : items) {
        Record r;
        if (!from_json(it, &r)) continue;
        if (lossy_source(r)) continue;
        t.in += r.source_size;
        if (r.has_winner) {
            t.out += r.winner_cost;
            ++t.winners;
        }
    }
    return t;
}

void print_summary(const std::vector<json::json>& items) {
    if (items.empty()) {
        out::print("%s\n", i18n::str("No statistics yet (no optimizations run).").c_str());
        return;
    }
    out::print("%s\n", i18n::fmt("Total records: %zu", items.size()).c_str());

    std::map<std::string, int> by_status;
    std::set<std::string> runs;
    Totals tot;
    for (const auto& it : items) {
        Record r;
        if (!from_json(it, &r)) continue;
        by_status[r.status]++;
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
    out::print("\nBy status:\n");
    for (const auto& [st, cnt] : by_status) {
        printf("  %-10s %d\n", st.c_str(), cnt);
    }

    std::vector<Rank> ranks = ranking(items);
    if (!ranks.empty()) {
        out::print("\nFormat ranking (files won, most likely winners first):\n");
        out::print("  %-16s %-12s %-8s %-10s\n", i18n::str("format").c_str(),
                   i18n::str("savings").c_str(), i18n::str("files").c_str(),
                   i18n::str("sizes").c_str());
        for (const auto& r : ranks) {
            out::print("  %-16s %6.2f%%  %7d  %8.2f -> %8.2f %s\n", r.format.c_str(),
                       r.savings * 100.0, r.files, r.total_in / 1048576.0,
                       r.total_out / 1048576.0, i18n::str("MB").c_str());
        }
    }
}

bool write_report(const std::string& dest, const std::vector<json::json>& items) {
    std::string text = build_report(items);
    if (text.empty()) return false;
    return util::write_text(dest, text);
}

std::string build_report(const std::vector<json::json>& items) {
    if (items.empty()) return std::string();

    Totals tot = totals_of(items);
    std::string r;
    r += "LLAO — format statistics\n";
    r += "Source: " + path() + "\n";
    r += "Records: " + std::to_string(items.size()) + "\n";
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
    std::vector<Rank> ranks = ranking(items);
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
    }
    return r;
}

std::vector<Rank> ranking(const std::vector<json::json>& items) {
    struct Agg {
        double sum = 0.0;
        int n = 0;
        uint64_t in = 0, out = 0;
    };
    std::map<std::string, Agg> agg;
    for (const auto& it : items) {
        Record r;
        if (!from_json(it, &r)) continue;
        // Победитель записан явно; записи без него (файл не отдан) в рейтинг не
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

}  // namespace stats