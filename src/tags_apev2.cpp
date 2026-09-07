#include "tags_internal.h"

#include <cctype>
#include <cstring>

namespace tags {

// ---------------------------------------------------------------------------
// APEv2
// ---------------------------------------------------------------------------

static void ape_item(std::vector<uint8_t>& body, const std::string& key,
                     const std::vector<uint8_t>& value, uint32_t flags) {
    wr32le(body, (uint32_t)value.size());
    wr32le(body, flags);
    body.insert(body.end(), key.begin(), key.end());
    body.push_back(0);
    body.insert(body.end(), value.begin(), value.end());
}

static void ape_text_item(std::vector<uint8_t>& body, const std::string& key, const std::string& val) {
    std::vector<uint8_t> v(val.begin(), val.end());
    ape_item(body, key, v, 0);
}

void build_apev2(const Group& g, const std::map<std::string, std::string>& key_map,
                 std::vector<uint8_t>& out) {
    std::vector<uint8_t> body;
    for (const auto& [key, values] : g.fields) {
        std::string k;
        if (is_replaygain(key)) {
            k = key;
            if (!k.empty()) k[0] = (char)::toupper((unsigned char)k[0]);
        } else {
            auto it = key_map.find(key);
            k = it != key_map.end() ? it->second : key;
        }
        for (const auto& val : values) ape_text_item(body, k, val);
    }
    if (!g.cue_sheet.empty()) ape_text_item(body, "Cuesheet", g.cue_sheet);
    for (size_t i = 0; i < g.pictures.size(); i++) {
        const Picture& p = g.pictures[i];
        std::string key = "Cover Art (Front)";
        std::vector<uint8_t> val;
        std::string desc = p.description.empty() ? "front" : p.description;
        val.insert(val.end(), desc.begin(), desc.end());
        val.push_back(0);
        val.insert(val.end(), p.data.begin(), p.data.end());
        ape_item(body, key, val, 0x2);
    }

    uint32_t item_count = 0;
    for (const auto& [key, values] : g.fields) item_count += (uint32_t)values.size();
    if (!g.cue_sheet.empty()) item_count++;
    item_count += (uint32_t)g.pictures.size();

    uint32_t tag_size = (uint32_t)(body.size() + 32);  // items + footer (без header)
    uint32_t flags_header = 0xA0000000;  // HAS_HEADER | IS_HEADER
    uint32_t flags_footer = 0xC0000000;  // HAS_HEADER | HAS_FOOTER
    const char* magic = "APETAGEX";
    auto header = [&](uint32_t fl) {
        out.insert(out.end(), magic, magic + 8);
        wr32le(out, 2000);
        wr32le(out, tag_size);
        wr32le(out, item_count);
        wr32le(out, fl);
        for (int i = 0; i < 8; i++) out.push_back(0);
    };
    header(flags_header);
    out.insert(out.end(), body.begin(), body.end());
    header(flags_footer);
}

bool apev2_parse(const uint8_t* d, size_t n, Group& g) {
    if (n < 32) return false;
    const uint8_t* footer = d + n - 32;
    if (memcmp(footer, "APETAGEX", 8) != 0) return false;
    uint32_t tag_size = rd32le(footer + 12);
    uint32_t item_count = rd32le(footer + 16);
    if (tag_size > n) return false;
    const uint8_t* items_end = d + n - 32;
    const uint8_t* items = items_end - tag_size + 32;
    if (items < d) return false;
    // Если есть header (32 байта) — пропускаем
    if (items < items_end && memcmp(items, "APETAGEX", 8) == 0) items += 32;
    const uint8_t* o = items;
    for (uint32_t i = 0; i < item_count; i++) {
        if (o + 8 > items_end) break;
        uint32_t vsize = rd32le(o);
        uint32_t flags = rd32le(o + 4);
        o += 8;
        const uint8_t* kend = o;
        while (kend < items_end && *kend != 0) kend++;
        if (kend >= items_end) break;
        std::string key((const char*)o, kend - o);
        o = kend + 1;
        if (o + vsize > items_end) break;
        std::string value((const char*)o, vsize);
        o += vsize;
        if (key == "Cover Art (Front)" || key == "Cover Art (Back)") {
            size_t nul = value.find('\0');
            Picture pic;
            pic.type = key == "Cover Art (Front)" ? 3 : 4;
            pic.mime = "image/jpeg";
            size_t datastart = 0;
            if (nul != std::string::npos) {
                pic.description = value.substr(0, nul);
                datastart = nul + 1;
            }
            pic.data.assign(value.begin() + datastart, value.end());
            g.pictures.push_back(std::move(pic));
            (void)flags;
        } else {
            g_put(g, key, value);
        }
    }
    return true;
}

}  // namespace tags