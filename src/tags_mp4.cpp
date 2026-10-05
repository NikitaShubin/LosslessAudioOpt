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
            b.wide = true;
            if (b.size < 16) break;
        } else if (sz == 0) {
            // size==0 по спецификации ISO/IEC 14496-12 означает «бокс до конца
            // файла» — так пишет, например, последний free-бокс.
            b.size = end - o;
        } else if (sz < 8) {
            break;
        }
        // Бокс не должен вылезать за пределы области: иначе битый заголовок
        // уводит обход далеко за границы и подсовывает выдуманные боксы.
        if (b.size > end - o) break;
        memcpy(b.type, d + o + 4, 4);
        out.push_back(b);
        o += b.size;
    }
    return out;
}

std::vector<M4aBox> m4a_top_level(const uint8_t* d, uint64_t n) {
    return m4a_children(d, 0, n);
}

bool m4a_top_find(const uint8_t* d, uint64_t n, const char* type, M4aBox* out) {
    for (const auto& b : m4a_top_level(d, n)) {
        if (memcmp(b.type, type, 4) == 0) {
            if (out) *out = b;
            return true;
        }
    }
    return false;
}

// Числовой тег MP4 приходит парой 16-битных чисел: номер и «всего». Раньше
// читался только номер (rd16be(val+2)), а «всего» выбрасывалось, поэтому из
// "3/13" получалось "3" — и сверка тегов объявляла поле не выжившим, хотя
// файл был записан правильно. Теперь пара собирается обратно в исходную
// форму; если «всего» нет (ноль), остаётся только номер, как его пишут APEv2,
// Vorbis и ID3 для одиночного значения.
static std::string mp4_pair(const uint8_t* val, uint32_t len) {
    unsigned num = rd16be(val + 2);
    unsigned total = len >= 6 ? rd16be(val + 4) : 0;
    char buf[24];
    if (total) snprintf(buf, sizeof(buf), "%u/%u", num, total);
    else snprintf(buf, sizeof(buf), "%u", num);
    return buf;
}

// Парсит картинки из moov>udta>meta>ilst (covr) и теги.
void mp4_ilst_parse(const uint8_t* d, size_t n, Group& g) {
    M4aBox moov_b;
    if (!m4a_top_find(d, n, "moov", &moov_b)) return;
    // moov ищем обходом дерева, а не поиском байтов: внутри энтропийных данных
    // mdat встречается ASCII "moov", и прежний поиск цеплял его, читал
    // мусорные 4 байта размера и молча терял все теги файла.
    const auto& tbl = config::load_tag_tables();
    // Числовые теги MP4 приходят бинарной парой 16-битных чисел (номер/всего).
    // Соответствие 4CC -> каноническое поле и размер пары берём из
    // formats/tag_tables.json (mp4_numeric), а не из кода.
    // Содержимое moov начинается после заголовка бокса (size+type): +8,
    // при 64-битном заголовке — +16.
    uint64_t moov_hdr = moov_b.wide ? 16 : 8;
    auto traks = m4a_children(d, moov_b.off + moov_hdr, moov_b.off + moov_b.size);
    for (const auto& m : traks) {
        if (memcmp(m.type, "udta", 4) != 0) continue;
        auto udtas = m4a_children(d, m.off + (m.wide ? 16 : 8), m.off + m.size);
        for (const auto& u : udtas) {
            if (memcmp(u.type, "meta", 4) != 0) continue;
            auto metas = m4a_children(d, u.off + (u.wide ? 20 : 12), u.off + u.size);  // +4 fullbox
            for (const auto& mt : metas) {
                if (memcmp(mt.type, "ilst", 4) != 0) continue;
                auto items = m4a_children(d, mt.off + (mt.wide ? 16 : 8), mt.off + mt.size);
                for (const auto& it : items) {
                    std::string key((const char*)it.type, 4);
                    auto datas = m4a_children(d, it.off + (it.wide ? 16 : 8), it.off + it.size);
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
                        } else if (const config::Mp4Numeric* num = tbl.mp4_numeric_find(key)) {
                            if (len >= num->box_size) g_put(g, num->field, mp4_pair(val, len));
                        } else if (len > 0) {
                            std::string s((const char*)val, len);
                            if (key == "----") {
                                // custom: mean/name/data
                                auto sub = m4a_children(d, it.off + (it.wide ? 16 : 8), it.off + it.size);
                                for (const auto& sb : sub) {
                                    if (memcmp(sb.type, "name", 4) == 0 && sb.size > 8) {
                                        std::string nm((const char*)d + sb.off + 8, sb.size - 8);
                                        if (!nm.empty()) g_put(g, nm, s);
                                    }
                                }
                            } else {
                                // MP4 4CC -> canonical key (из formats/tag_tables.json).
                                auto it2 = tbl.mp4.find(key);
                                g_put(g, it2 != tbl.mp4.end() ? it2->second : key, s);
                            }
                        }
                    }
                }
            }
        }
    }
}

}  // namespace tags