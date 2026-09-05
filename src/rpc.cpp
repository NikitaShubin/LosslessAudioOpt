#include "rpc.h"

#include <string>
#include <vector>

#include "util.h"

namespace dsvc {

namespace {

nlohmann::json ok(nlohmann::json result) {
    return {{"ok", true}, {"result", std::move(result)}};
}

nlohmann::json err(const std::string& code, const std::string& msg) {
    return {{"ok", false}, {"code", code}, {"error", msg}};
}

// Строгое чтение аргументов: никаких исключений наружу — некорректный тип
// это bad_args, а не 500/обрыв соединения. nlohmann get<>() на неверном
// типе бросает type_error, поэтому сначала проверяем is_*().
// Принимаем целое неотрицательное (JSON-парсер даёт number_integer
// даже для "3", поэтому is_number_unsigned() здесь не подходит).
bool get_uint(const nlohmann::json& args, const char* key, uint64_t& out) {
    if (!args.is_object() || !args.contains(key)) return false;
    const auto& v = args[key];
    if (v.is_number_unsigned()) {
        out = v.get<uint64_t>();
        return true;
    }
    if (v.is_number_integer() && v.get<int64_t>() >= 0) {
        out = (uint64_t)v.get<int64_t>();
        return true;
    }
    return false;
}

bool get_bool(const nlohmann::json& args, const char* key, bool dflt) {
    if (!args.is_object() || !args.contains(key)) return dflt;
    const auto& v = args[key];
    if (!v.is_boolean()) return dflt;
    return v.get<bool>();
}

bool get_id_list(const nlohmann::json& args, const char* key,
                 std::vector<uint64_t>& out) {
    if (!args.is_object() || !args.contains(key)) return false;
    const auto& arr = args[key];
    if (!arr.is_array()) return false;
    for (const auto& v : arr) {
        uint64_t one = 0;
        bool ok = false;
        if (v.is_number_unsigned()) {
            one = v.get<uint64_t>();
            ok = true;
        } else if (v.is_number_integer() && v.get<int64_t>() >= 0) {
            one = (uint64_t)v.get<int64_t>();
            ok = true;
        }
        if (!ok) return false;
        out.push_back(one);
    }
    return true;
}

}  // namespace

nlohmann::json call(Daemon& d, const std::string& cmd, const nlohmann::json& args) {
    if (!args.is_object() && !args.is_null())
        return err("bad_args", "args must be a JSON object");

    if (cmd == "ping") {
        nlohmann::json counters = d.counters();
        return ok({
            {"version", d.version()},
            {"uptime_s", d.uptime_s()},
            {"paused", d.paused()},
            {"queue", counters},
        });
    }

    if (cmd == "stat") {
        std::vector<std::string> paths;
        nlohmann::json list = nlohmann::json::array();
        if (args.is_object() && args.contains("paths")) {
            if (!args["paths"].is_array())
                return err("bad_args", "paths must be an array");
            for (const auto& p : args["paths"]) {
                if (!p.is_string()) {
                    list.push_back({{"path", "<non-string>"}, {"type", "invalid"}});
                    continue;
                }
                paths.push_back(p.get<std::string>());
            }
        }
        for (const auto& p : paths) {
            std::string type = util::dir_exists(p) ? "dir"
                                 : util::file_exists(p) ? "file"
                                                        : "missing";
            list.push_back({{"path", p}, {"type", type}});
        }
        return ok({{"paths", std::move(list)}});
    }

    if (cmd == "add") {
        std::vector<std::string> paths;
        nlohmann::json result = {{"added", nlohmann::json::array()},
                                 {"rejected", nlohmann::json::array()}};
        if (args.is_object() && args.contains("paths")) {
            if (!args["paths"].is_array())
                return err("bad_args", "paths must be an array");
            for (const auto& p : args["paths"]) {
                if (!p.is_string()) {
                    result["rejected"].push_back(
                        {{"path", "<non-string>"}, {"reason", "not a string path"}});
                    continue;
                }
                paths.push_back(p.get<std::string>());
            }
        }
        bool recursive = get_bool(args, "recursive", true);
        d.add(paths, recursive, result);
        return ok(result);
    }

    if (cmd == "cancel-file") {
        uint64_t id = 0;
        if (!get_uint(args, "id", id)) return err("bad_args", "missing numeric id");
        bool existed = d.cancel_file(id);
        return ok({{"deleted", existed}});
    }

    if (cmd == "remove") {
        uint64_t id = 0;
        if (!get_uint(args, "id", id)) return err("bad_args", "missing numeric id");
        bool ok_ = d.remove(id);
        return ok({{"removed", ok_}});
    }

    if (cmd == "clear-done") {
        uint64_t removed = d.clear_done();
        return ok({{"removed", removed}});
    }

    if (cmd == "bulk-remove") {
        std::vector<uint64_t> ids;
        if (!get_id_list(args, "ids", ids))
            return err("bad_args", "missing ids array");
        std::vector<size_t> sid(ids.begin(), ids.end());
        return ok({{"removed", d.bulk_remove(sid)}});
    }

    if (cmd == "bulk-cancel") {
        std::vector<uint64_t> ids;
        if (!get_id_list(args, "ids", ids))
            return err("bad_args", "missing ids array");
        std::vector<size_t> sid(ids.begin(), ids.end());
        return ok({{"cancelled", d.bulk_cancel(sid)}});
    }

    if (cmd == "sort") {
        return ok({{"sorted", d.sort_by_path()}});
    }

    if (cmd == "restart") {
        std::vector<uint64_t> ids;
        uint64_t single = 0;
        if (get_id_list(args, "ids", ids)) {
            // ok
        } else if (get_uint(args, "id", single)) {
            ids.push_back(single);
        } else {
            return err("bad_args", "missing ids array");
        }
        nlohmann::json done = nlohmann::json::array();
        for (uint64_t id : ids)
            if (d.restart(id)) done.push_back(id);
        return ok({{"restarted", std::move(done)}});
    }

    if (cmd == "cancel-all" || cmd == "pause") {
        d.set_paused(true);
        return ok({{"paused", true}});
    }

    if (cmd == "resume") {
        d.set_paused(false);
        return ok({{"paused", false}});
    }

    if (cmd == "reorder") {
        std::vector<uint64_t> raw;
        if (!(get_id_list(args, "order", raw) || get_id_list(args, "ids", raw)))
            return err("bad_args", "missing order array");
        std::vector<size_t> order(raw.begin(), raw.end());
        bool ok_ = d.reorder(order);
        if (!ok_) return err("bad_args", "invalid order");
        return ok({{"reordered", true}});
    }

    if (cmd == "shutdown") {
        bool force = get_bool(args, "force", false);
        d.request_shutdown(force);
        return ok({{"shutdown", true}});
    }

    if (cmd == "formats") {
        return ok(d.formats());
    }

    if (cmd == "debug") {
        return ok(d.debug_state());
    }

    return err("unknown_cmd", "unknown command: " + cmd);
}

}  // namespace dsvc
