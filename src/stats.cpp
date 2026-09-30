#include "stats.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

#include "i18n.h"
#include "media.h"
#include "out.h"
#include "util.h"

namespace json = nlohmann;

namespace stats {

static std::mutex g_mutex;

std::string path() {
    // Оверрайд нужен для тестов и для разбора чужой статистики: по умолчанию
    // файл лежит рядом с exe (у клиента и у демона это разные экземпляры).
    if (const char* env = std::getenv("LLAO_STATS_FILE")) {
        if (*env) return env;
    }
    return util::join_path(util::exe_dir(), "stats.json");
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

bool append(const json::json& item) {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<json::json> items = load();
    items.push_back(item);
    try {
        json::json arr(items);
        std::string text = arr.dump(2);
        return util::write_text(path(), text);
    } catch (...) {
        return false;
    }
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

void print_summary(const std::vector<json::json>& items) {
    if (items.empty()) {
        out::print("No statistics yet (no optimizations run).\n");
        return;
    }
    out::print("Total records: %zu\n", items.size());

    std::map<std::string, int> by_format;
    std::map<std::string, int> by_status;
    uint64_t total_in = 0, total_out = 0;
    int winners = 0;
    for (const auto& it : items) {
        std::string fmt = it.value("format", it.value("fmt_id", "?"));
        std::string status = it.value("status", "?");
        by_format[fmt]++;
        by_status[status]++;
        if (it.contains("source_size") && it["source_size"].is_number_unsigned()) total_in += it["source_size"].get<uint64_t>();
        if (it.contains("result_size") && it["result_size"].is_number_unsigned()) total_out += it["result_size"].get<uint64_t>();
        if (it.value("winner", false)) winners++;
    }

    out::print("Winners (files replaced): %d\n", winners);
    if (total_in > 0) {
        double ratio = total_out > 0 ? (double)total_out / (double)total_in : 0.0;
        out::print("Total source size: %.2f MB, result: %.2f MB (%.2f%%)\n",
               total_in / 1048576.0, total_out / 1048576.0, ratio * 100.0);
    }
    out::print("\nBy format:\n");
    for (const auto& [fmt, cnt] : by_format) {
        printf("  %-16s %d\n", fmt.c_str(), cnt);
    }
    out::print("\nBy status:\n");
    for (const auto& [st, cnt] : by_status) {
        printf("  %-10s %d\n", st.c_str(), cnt);
    }

    std::vector<Rank> ranks = ranking(items);
    if (!ranks.empty()) {
        out::print("\nFormat ranking (most likely winners first):\n");
        out::print("  %-16s %-12s %-8s %-10s\n", i18n::str("format").c_str(),
             i18n::str("savings").c_str(), i18n::str("samples").c_str(),
             i18n::str("sizes").c_str());
        for (const auto& r : ranks) {
            out::print("  %-16s %6.2f%%  %7d  %8.2f -> %8.2f %s\n", r.format.c_str(),
                   r.savings * 100.0, r.samples, r.total_in / 1048576.0,
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

    std::map<std::string, int> by_status;
    uint64_t total_in = 0, total_out = 0;
    int winners = 0;
    for (const auto& it : items) {
        by_status[it.value("status", "?")]++;
        if (it.contains("source_size") && it["source_size"].is_number_unsigned())
            total_in += it["source_size"].get<uint64_t>();
        if (it.contains("result_size") && it["result_size"].is_number_unsigned())
            total_out += it["result_size"].get<uint64_t>();
        if (it.value("winner", false)) winners++;
    }

    std::string r;
    r += "LLAO — format statistics\n";
    r += "Source: " + path() + "\n";
    r += "Records: " + std::to_string(items.size()) + "\n";
    r += "Files replaced: " + std::to_string(winners) + "\n";
    if (total_in > 0) {
        char buf[160];
        double ratio = total_out > 0 ? (double)total_out / (double)total_in : 0.0;
        snprintf(buf, sizeof(buf), "Total size: %.2f MB -> %.2f MB (%.2f%% of source)\n",
                 total_in / 1048576.0, total_out / 1048576.0, ratio * 100.0);
        r += buf;
    }

    r += "\nCandidates by status:\n";
    for (const auto& [st, cnt] : by_status) {
        char buf[64];
        snprintf(buf, sizeof(buf), "  %-10s %d\n", st.c_str(), cnt);
        r += buf;
    }

    // Рейтинг — по убыванию средней экономии: так видно, кто выигрывает на
    // конкретном материале, а не «вообще у всех одинаково».
    std::vector<Rank> ranks = ranking(items);
    if (!ranks.empty()) {
        r += "\nFormat ranking (average savings over successful candidates):\n";
        r += "  format            savings  candidates        size (MB)\n";
        for (const auto& rk : ranks) {
            char buf[192];
            snprintf(buf, sizeof(buf), "  %-16s %6.2f%% %9d  %8.2f -> %8.2f\n",
                     rk.format.c_str(), rk.savings * 100.0, rk.samples,
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
        if (it.value("status", "?") != "ok") continue;  // только успешные кандидаты
        // Lossy-источники (mp3 и т.п.) в ранжировании не участвуют: их конвертация
        // в lossless всегда увеличивает размер и искажает оценку форматов.
        if (it.contains("codec_name") && it["codec_name"].is_string() &&
            !media::codec_is_lossless(it["codec_name"].get<std::string>()))
            continue;
        uint64_t src = 0, dst = 0;
        if (it.contains("source_size") && it["source_size"].is_number_unsigned())
            src = it["source_size"].get<uint64_t>();
        if (it.contains("cost") && it["cost"].is_number_unsigned())
            dst = it["cost"].get<uint64_t>();
        else if (it.contains("result_size") && it["result_size"].is_number_unsigned())
            dst = it["result_size"].get<uint64_t>();
        if (src == 0 || dst == 0) continue;
        std::string fmt = it.value("format", it.value("fmt_id", "?"));
        Agg& a = agg[fmt];
        a.sum += 1.0 - (double)dst / (double)src;
        a.n++;
        a.in += src;
        a.out += dst;
    }
    std::vector<Rank> res;
    for (const auto& [fmt, a] : agg) {
        res.push_back({fmt, a.n ? a.sum / a.n : 0.0, a.n, a.in, a.out});
    }
    std::sort(res.begin(), res.end(), [](const Rank& x, const Rank& y) {
        if (x.savings != y.savings) return x.savings > y.savings;
        return x.samples > y.samples;
    });
    return res;
}

}  // namespace stats
