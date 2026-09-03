#include "http_api.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "version.h"
#include "web_assets.h"

namespace dsvc {

namespace {

// Постоянная времени сравнения токена (против side-channel).
bool token_matches(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned int diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

bool authorized(const httplib::Request& req, const std::string& token) {
    if (token.empty()) return true;  // dev: без авторизации
    const std::string prefix = "Bearer ";
    const std::string& auth = req.get_header_value("Authorization");
    if (auth.size() <= prefix.size()) return false;
    if (auth.compare(0, prefix.size(), prefix) != 0) return false;
    return token_matches(auth.substr(prefix.size()), token);
}

void send_json(httplib::Response& res, const nlohmann::json& j) {
    res.set_content(j.dump(), "application/json");
}

void send_unauthorized(httplib::Response& res) {
    res.status = 401;
    res.set_content(R"({"error":"unauthorized"})", "application/json");
}

// Строки очереди из зеркала в json-массив (порядок id).
nlohmann::json rows_json(const StateMirror& st) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : st.snapshot()) {
        nlohmann::json tasks = nlohmann::json::array();
        for (const auto& t : r.tasks) tasks.push_back(t);
        arr.push_back({{"id", r.id},
                       {"label", r.label},
                       {"state", r.state},
                       {"pct", r.pct},
                       {"tasks", std::move(tasks)}});
    }
    return arr;
}

uint64_t clamp_since(const std::string& s) {
    if (s.empty()) return 0;
    try {
        return std::stoull(s);
    } catch (...) {
        return 0;
    }
}

}  // namespace

int mount(httplib::Server& svr, const ApiContext& ctx) {
    const std::string token = ctx.token;

    // Инициализация встроенных веб-ассетов (zip → память через miniz).
    std::string werr;
    bool have_web = web_assets::init(&werr);
    // Веб-UI — публичные эндпоинты (без авторизации): GET / и /static/* .

    svr.Get("/", [have_web](const httplib::Request&, httplib::Response& res) {
        if (have_web) {
            const std::string* data = web_assets::get("index.html");
            if (data) {
                res.set_content(*data, "text/html; charset=utf-8");
                return;
            }
        }
        // Фолбэк, если ассеты не вшиты (dev-сборка без embed).
        res.set_content(
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>LLAO daemon</title></head><body>"
            "<h1>LLAO daemon</h1>"
            "<p>Headless-демон оптимизации lossless-аудио. Версия "
            LLAO_VERSION ".</p>"
            "<p>Управление через API с заголовком "
            "<code>Authorization: Bearer &lt;token&gt;</code>.</p>"
            "<p>Web UI: ассеты не вшиты — пересоберите с tools/embed_assets.py.</p></body></html>",
            "text/html; charset=utf-8");
    });

    // Статика: /static/<path> → файл из web/* (напр. /static/app.js → app.js).
    svr.Get(R"(/static/(.*))", [have_web](const httplib::Request& req, httplib::Response& res) {
        if (!have_web) {
            res.status = 404;
            res.set_content("Web assets not embedded", "text/plain");
            return;
        }
        std::string sub = req.matches[1].str();
        // Защита от path traversal.
        if (sub.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("Bad path", "text/plain");
            return;
        }
        const std::string* data = web_assets::get(sub);
        if (!data) {
            res.status = 404;
            res.set_content("Not found", "text/plain");
            return;
        }
        res.set_content(*data, web_assets::mime_type(sub));
    });

    svr.Post("/rpc", [ctx, token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) return send_unauthorized(res);
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            res.status = 400;
            return send_json(res, {{"ok", false}, {"code", "bad_json"}, {"error", "invalid json"}});
        }
        std::string cmd = body.value("cmd", "");
        nlohmann::json args = body.contains("args") ? body["args"] : nlohmann::json::object();
        if (cmd.empty() || !body.contains("cmd")) {
            res.status = 400;
            return send_json(res,
                             {{"ok", false}, {"code", "bad_args"}, {"error", "missing cmd"}});
        }
        send_json(res, dsvc::call(*ctx.daemon, cmd, args));
    });

    svr.Get("/api/state", [ctx, token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) return send_unauthorized(res);
        nlohmann::json out;
        out["version"] = ctx.daemon->version();
        out["session"] = {
            {"options", ctx.daemon->session_options()},
        };
        out["counters"] = ctx.daemon->counters();
        out["paused"] = ctx.daemon->paused();
        out["last_seq"] = ctx.events->last_seq();
        out["rows"] = rows_json(*ctx.state);
        send_json(res, out);
    });

    svr.Get("/api/events", [ctx, token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) return send_unauthorized(res);
        uint64_t since = clamp_since(req.get_param_value("since"));
        auto poll = ctx.events->copy_since(since);
        nlohmann::json evs = nlohmann::json::array();
        for (const auto& e : poll.events) {
            nlohmann::json j = e.args;
            j["type"] = e.type;
            j["seq"] = e.seq;
            evs.push_back(std::move(j));
        }
        send_json(res, {{"events", std::move(evs)},
                        {"last_seq", poll.last_seq},
                        {"resync", poll.resync}});
    });

    svr.Get("/api/formats", [ctx, token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) return send_unauthorized(res);
        send_json(res, ctx.daemon->formats());
    });

    return 0;
}

}  // namespace dsvc
