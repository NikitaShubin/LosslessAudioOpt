#include "tags_internal.h"

#include <cstring>

#include <nlohmann/json.hpp>

#include "i18n.h"
#include "util.h"
#include "miniz/miniz.h"

namespace tags {

namespace json = nlohmann;

// ---------------------------------------------------------------------------
// Sidecar (ZIP v2: группы)
// ---------------------------------------------------------------------------

uint64_t write_sidecar(const std::string& base_path, const std::vector<Group>& groups,
                       std::string* err) {
    json::json doc;
    doc["version"] = 2;
    doc["format"] = "llao-sidecar";
    json::json gs = json::json::array();
    int picidx = 0;
    for (const auto& g : groups) {
        json::json jg;
        jg["type"] = tag_type_name(g.type);
        json::json fields = json::json::object();
        for (const auto& [k, vs] : g.fields) fields[k] = vs;
        jg["fields"] = fields;
        if (!g.cue_sheet.empty()) jg["cue_sheet"] = g.cue_sheet;
        json::json pics = json::json::array();
        for (const auto& p : g.pictures) {
            json::json jp;
            jp["type"] = p.type;
            jp["mime"] = p.mime;
            jp["description"] = p.description;
            std::string ext = p.mime.find("png") != std::string::npos ? "png" : "jpg";
            jp["file"] = "pictures/" + std::to_string(picidx) + "." + ext;
            pics.push_back(jp);
            picidx++;
        }
        jg["pictures"] = pics;
        gs.push_back(jg);
    }
    doc["groups"] = gs;

    std::string zip_path = base_path + ".tags.zip";
    util::remove_file(zip_path);
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, zip_path.c_str(), 0)) {
        *err = i18n::str("could not create ZIP");
        return 0;
    }
    util::sanitize_json(doc);
    std::string js = doc.dump();
    mz_zip_writer_add_mem(&z, "tags.json", js.data(), js.size(), MZ_BEST_COMPRESSION);
    int pi = 0;
    for (const auto& g : groups) {
        for (const auto& p : g.pictures) {
            std::string ext = p.mime.find("png") != std::string::npos ? "png" : "jpg";
            std::string file = "pictures/" + std::to_string(pi) + "." + ext;
            mz_zip_writer_add_mem(&z, file.c_str(), p.data.data(), p.data.size(),
                                  MZ_BEST_COMPRESSION);
            pi++;
        }
    }
    bool ok = mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    if (!ok) {
        util::remove_file(zip_path);
        *err = i18n::str("could not finalize ZIP");
        return 0;
    }
    return util::file_size(zip_path);
}

// ---------------------------------------------------------------------------
// Чтение ZIP-sidecar (v1 — для совместимости, v2 — группы)
// ---------------------------------------------------------------------------

bool read_sidecar(const std::string& base_path, TagSet& ts, std::string* err) {
    std::string zip_path = base_path + ".tags.zip";
    auto data = util::read_file(zip_path);
    if (data.empty()) {
        // Оптимизация сохраняет sidecar как "<имя файла без расширения>.tags.zip",
        // а не "<имя файла>.tags.zip" — пробуем и такой вариант.
        std::string alt = base_path;
        size_t dot = alt.find_last_of('.');
        size_t sep = alt.find_last_of("/\\");
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
            alt = alt.substr(0, dot);
        alt += ".tags.zip";
        if (alt != zip_path) data = util::read_file(alt);
    }
    if (data.empty()) return false;
    if (data.size() < 4 || memcmp(data.data(), "PK\x03\x04", 4) != 0) {
        if (err) *err = i18n::str("sidecar is not a ZIP");
        return false;
    }
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) {
        if (err) *err = i18n::str("could not open sidecar");
        return false;
    }
    auto finish = [&](bool ok, const std::string& m) {
        mz_zip_reader_end(&z);
        if (!ok && err) *err = m;
        return ok;
    };

    int tagidx = mz_zip_reader_locate_file(&z, "tags.json", nullptr, 0);
    if (tagidx < 0) return finish(false, i18n::str("no tags.json in the sidecar"));
    size_t n = 0;
    void* raw = mz_zip_reader_extract_to_heap(&z, (mz_uint)tagidx, &n, 0);
    if (!raw) return finish(false, i18n::str("could not extract tags.json"));
    std::string js((const char*)raw, n);
    MZ_FREE(raw);

    try {
        json::json doc = json::json::parse(js);
        int version = doc.value("version", 0);
        if (version != 1 && version != 2)
            return finish(false, i18n::str("unknown sidecar version"));

        auto read_pictures = [&](const json::json& arr, Group& g) {
            for (const auto& p : arr) {
                if (!p.is_object()) continue;
                Picture pic;
                pic.type = p.value("type", 3);
                pic.mime = p.value("mime", "image/jpeg");
                pic.description = p.value("description", "");
                std::string file = p.value("file", "");
                if (file.empty()) continue;
                int fi = mz_zip_reader_locate_file(&z, file.c_str(), nullptr, 0);
                if (fi < 0) continue;
                size_t pn = 0;
                void* praw = mz_zip_reader_extract_to_heap(&z, (mz_uint)fi, &pn, 0);
                if (!praw) continue;
                pic.data.assign((const uint8_t*)praw, (const uint8_t*)praw + pn);
                MZ_FREE(praw);
                g.pictures.push_back(std::move(pic));
            }
        };

        if (version == 1) {
            // Старый плоский формат — сворачиваем в одну группу unknown.
            Group g;
            g.type = TagType::unknown;
            if (doc.contains("fields") && doc["fields"].is_object()) {
                for (auto& [k, vv] : doc["fields"].items()) {
                    if (!vv.is_array()) continue;
                    for (auto& el : vv)
                        if (el.is_string()) g_put(g, k, el.get<std::string>());
                }
            }
            if (doc.contains("cue_sheet") && doc["cue_sheet"].is_string())
                g.cue_sheet = doc["cue_sheet"].get<std::string>();
            if (doc.contains("pictures") && doc["pictures"].is_array())
                read_pictures(doc["pictures"], g);
            if (!g.empty()) ts.groups.push_back(std::move(g));
        } else {
            if (!doc.contains("groups") || !doc["groups"].is_array())
                return finish(false, i18n::str("no groups in the sidecar"));
            for (const auto& jg : doc["groups"]) {
                if (!jg.is_object()) continue;
                Group g;
                g.type = tag_type_from_string(jg.value("type", ""));
                if (jg.contains("fields") && jg["fields"].is_object()) {
                    for (auto& [k, vv] : jg["fields"].items()) {
                        if (!vv.is_array()) continue;
                        for (auto& el : vv)
                            if (el.is_string()) g_put(g, k, el.get<std::string>());
                    }
                }
                if (jg.contains("cue_sheet") && jg["cue_sheet"].is_string())
                    g.cue_sheet = jg["cue_sheet"].get<std::string>();
                if (jg.contains("pictures") && jg["pictures"].is_array())
                    read_pictures(jg["pictures"], g);
                if (!g.empty()) ts.groups.push_back(std::move(g));
            }
        }
    } catch (const std::exception& exc) {
        return finish(false, i18n::str("invalid tags.json: ") + exc.what());
    }
    rebuild_canonical(ts);
    ts.complete = true;
    return finish(true, "");
}

}  // namespace tags