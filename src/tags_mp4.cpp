#include "tags_internal.h"

#include <cstdio>
#include <cstring>

#include "config.h"

namespace tags {

// ---------------------------------------------------------------------------
// MP4 ilst
// ---------------------------------------------------------------------------

std::vector<M4aBox> m4a_children(const uint8_t* d, uint64_t start, uint64_t end) {
    std::vector<M4aBox> out;
    uint64_t o = start;
    while (o + 8 <= end) {
        M4aBox b;
        b.off = o;
        uint32_t sz = rd32be(d + o);
        b.size = sz;
        if (sz == 1) {
            if (o + 16 > end) break;
            b.size = ((uint64_t)rd32be(d + o + 8) << 32) | rd32be(d + o + 12);
            if (b.size < 16) break;
        } else if (sz < 8) {
            break;
        }
        memcpy(b.type, d + o + 4, 4);
        out.push_back(b);
        o += b.size;
    }
    return out;
}

// Парсит картинки из moov>udta>meta>ilst (covr) и теги.
void mp4_ilst_parse(const uint8_t* d, size_t n, Group& g) {
    size_t moov = 0;
    for (size_t i = 0; i + 4 <= n; i++)
        if (memcmp(d + i, "moov", 4) == 0) { moov = i; break; }
    if (!moov) return;
    M4aBox moov_b;
    moov_b.off = moov - 4;
    moov_b.size = rd32be(d + moov - 4);
    // Содержимое moov начинается после заголовка бокса (size+type): moov+4.
    auto traks = m4a_children(d, moov + 4, moov_b.off + moov_b.size);
    for (const auto& m : traks) {
        if (memcmp(m.type, "udta", 4) != 0) continue;
        auto udtas = m4a_children(d, m.off + 8, m.off + m.size);
        for (const auto& u : udtas) {
            if (memcmp(u.type, "meta", 4) != 0) continue;
            auto metas = m4a_children(d, u.off + 12, u.off + u.size);  // +4 fullbox
            for (const auto& mt : metas) {
                if (memcmp(mt.type, "ilst", 4) != 0) continue;
                auto items = m4a_children(d, mt.off + 8, mt.off + mt.size);
                for (const auto& it : items) {
                    std::string key((const char*)it.type, 4);
                    auto datas = m4a_children(d, it.off + 8, it.off + it.size);
                    for (const auto& dt : datas) {
                        if (memcmp(dt.type, "data", 4) != 0) continue;
                        uint32_t flags = rd32be(d + dt.off + 8);
                        uint32_t len = (uint32_t)(dt.size - 16);
                        const uint8_t* val = d + dt.off + 16;
                        if (flags == 13 || flags == 14) {  // covr
                            Picture pic;
                            pic.type = 3;
                            pic.mime = flags == 13 ? "image/jpeg" : "image/png";
                            pic.data.assign(val, val + len);
                            g.pictures.push_back(std::move(pic));
                        } else if (key == "trkn" && len >= 8) {
                            char buf[16];
                            snprintf(buf, sizeof(buf), "%u", rd16be(val + 2));
                            g.fields["track"].push_back(buf);
                        } else if (key == "disk" && len >= 6) {
                            char buf[16];
                            snprintf(buf, sizeof(buf), "%u", rd16be(val + 2));
                            g.fields["disc"].push_back(buf);
                        } else if (len > 0) {
                            std::string s((const char*)val, len);
                            if (key == "----") {
                                // custom: mean/name/data
                                auto sub = m4a_children(d, it.off + 8, it.off + it.size);
                                for (const auto& sb : sub) {
                                    if (memcmp(sb.type, "name", 4) == 0 && sb.size > 8) {
                                        std::string nm((const char*)d + sb.off + 8, sb.size - 8);
                                        if (!nm.empty()) g_put(g, nm, s);
                                    }
                                }
                            } else {
                                // MP4 4CC -> canonical key (из formats/tag_tables.json).
                                const auto& tbl = config::load_tag_tables().mp4;
                                auto it2 = tbl.find(key);
                                g_put(g, it2 != tbl.end() ? it2->second : key, s);
                            }
                        }
                    }
                }
            }
        }
    }
}

}  // namespace tags