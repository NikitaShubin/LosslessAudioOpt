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
        if (args.is_object() && args.contains("paths") && args["paths"].is_array())
            for (const auto& p : args["paths"]) paths.push_back(p.get<std::string>());
        nlohmann::json list = nlohmann::json::array();
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
        if (args.is_object() && args.contains("paths") && args["paths"].is_array())
            for (const auto& p : args["paths"]) paths.push_back(p.get<std::string>());
        bool recursive = args.is_object() && args.value("recursive", true);
        nlohmann::json result = {{"added", nlohmann::json::array()},
                                 {"rejected", nlohmann::json::array()}};
        d.add(paths, recursive, result);
        return ok(result);
    }

    if (cmd == "cancel-file") {
        uint64_t id = args.is_object() && args.contains("id") ? args["id"].get<uint64_t>() : 0;
        bool existed = d.cancel_file(id);
        return ok({{"deleted", existed}});
    }

    if (cmd == "remove") {
        uint64_t id = args.is_object() && args.contains("id") ? args["id"].get<uint64_t>() : 0;
        bool ok_ = d.remove(id);
        return ok({{"removed", ok_}});
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
        std::vector<size_t> order;
        if (args.is_object() && args.contains("order") && args["order"].is_array()) {
            for (const auto& v : args["order"]) order.push_back(v.get<size_t>());
        } else if (args.is_object() && args.contains("ids") && args["ids"].is_array()) {
            for (const auto& v : args["ids"]) order.push_back(v.get<size_t>());
        } else {
            return err("bad_args", "missing order array");
        }
        bool ok_ = d.reorder(order);
        if (!ok_) return err("bad_args", "invalid order");
        return ok({{"reordered", true}});
    }

    if (cmd == "shutdown") {
        bool force = args.is_object() && args.value("force", false);
        d.request_shutdown(force);
        return ok({{"shutdown", true}});
    }

    if (cmd == "formats") {
        return ok(d.formats());
    }

    return err("unknown_cmd", "unknown command: " + cmd);
}

}  // namespace dsvc
