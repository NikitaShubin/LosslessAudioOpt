#include "persist.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include <nlohmann/json.hpp>

#include "util.h"

namespace persist {

std::string sidecar_path_for(const std::string& path) {
    std::string base = util::base_name(path);
    size_t dot = base.find_last_of('.');
    size_t sep = base.find_last_of('/');
    if (sep == std::string::npos) sep = base.find_last_of('\\');
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        base = base.substr(0, dot);
    return util::join_path(util::dir_name(path), base + ".tags.zip");
}

std::string to_json(const std::vector<Row>& rows) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : rows) {
        arr.push_back({{"path", r.path},
                       {"mode", r.mode},
                       {"target_dir", r.target_dir},
                       {"state", r.state},
                       {"pct", r.pct},
                       {"out_path", r.out_path},
                       {"had_sidecar", r.had_sidecar},
                       {"has_sidecar", r.has_sidecar},
                       {"last_error", r.last_error}});
    }
    util::sanitize_json(arr);
    nlohmann::json doc = {{"version", 2}, {"rows", std::move(arr)}};
    return doc.dump();
}

bool from_json(const std::string& text, std::vector<Row>* rows) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (...) {
        return false;
    }
    if (!doc.is_object() || doc.value("version", 0) != 2) return false;
    if (!doc.contains("rows") || !doc["rows"].is_array()) return false;
    rows->clear();
    for (const auto& j : doc["rows"]) {
        if (!j.is_object()) return false;
        Row r;
        r.path = j.value("path", "");
        r.mode = j.value("mode", "optimize");
        r.target_dir = j.value("target_dir", "");
        r.state = j.value("state", "stopped");
        r.pct = j.value("pct", 0.0);
        r.out_path = j.value("out_path", "");
        r.had_sidecar = j.value("had_sidecar", false);
        r.has_sidecar = j.value("has_sidecar", false);
        r.last_error = j.value("last_error", "");
        if (r.path.empty()) continue;
        rows->push_back(std::move(r));
    }
    return true;
}

bool write_file(const std::string& path, const std::vector<Row>& rows) {
    if (path.empty()) return false;
    util::mkdirs(util::dir_name(path));
    std::string tmp = path + ".llao-tmp";
    if (!util::write_text(tmp, to_json(rows))) return false;
    for (int i = 0; i < 5; i++) {
        if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    util::remove_file(tmp);
    return false;
}

bool read_file(const std::string& path, std::vector<Row>* rows) {
    rows->clear();
    std::string text = util::read_text(path);
    if (text.empty()) return false;
    return from_json(text, rows);
}

}  // namespace persist