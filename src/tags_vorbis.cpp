#include "tags_internal.h"

#include <cctype>
#include <cstring>

#include "util.h"

namespace tags {

// ---------------------------------------------------------------------------
// FLAC / Vorbis comment
// ---------------------------------------------------------------------------

void build_vorbis_comment(const Group& g, const std::map<std::string, std::string>& key_map,
                          std::vector<uint8_t>& out) {
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& [key, values] : g.fields) {
        for (const auto& val : values) {
            std::string k;
            if (is_replaygain(key)) k = key;  // replaygain_track_gain → REPLAYGAIN_TRACK_GAIN
            else {
                auto it = key_map.find(key);
                k = it != key_map.end() ? it->second : key;
            }
            items.emplace_back(k, val);
        }
    }
    auto lyr = g.fields.find("lyrics");
    if (lyr != g.fields.end() && !lyr->second.empty())
        items.emplace_back("LYRICS", lyr->second[0]);
    if (!g.cue_sheet.empty()) items.emplace_back("CUESHEET", g.cue_sheet);

    std::string vendor = "LLAO";
    wr32le(out, (uint32_t)vendor.size());
    out.insert(out.end(), vendor.begin(), vendor.end());
    wr32le(out, (uint32_t)items.size());
    for (const auto& [k, v] : items) {
        std::string item = util::to_lower(k);
        if (is_replaygain(k)) item = k;
        else {
            for (char& ch : item)
                if (ch == ' ') ch = '_';
            for (auto& ch : item) ch = (char)::toupper((unsigned char)ch);
        }
        item += "=";
        item += v;
        wr32le(out, (uint32_t)item.size());
        out.insert(out.end(), item.begin(), item.end());
    }
}

static bool vorbis_items(const uint8_t* d, size_t n, Group& g) {
    if (n < 8) return false;
    size_t o = 0;
    uint32_t vlen = rd32le(d + o);  // длина vendor
    o += 4;
    if (o + vlen > n) return false;
    o += vlen;
    if (o + 4 > n) return false;
    uint32_t items = rd32le(d + o);
    o += 4;
    for (uint32_t i = 0; i < items; i++) {
        if (o + 4 > n) return false;
        uint32_t len = rd32le(d + o);
        o += 4;
        if (o + len > n) return false;
        std::string item((const char*)d + o, len);
        o += len;
        size_t eq = item.find('=');
        std::string key = eq == std::string::npos ? item : item.substr(0, eq);
        std::string val = eq == std::string::npos ? "" : item.substr(eq + 1);
        if (util::to_lower(key) == "metadata_block_picture") {
            // base64 картинки в комментарии
            std::vector<uint8_t> pic;
            if (util::from_base64(val, &pic)) {
                // начало — описание блока PICTURE: type, mime, desc, размеры, данные
                if (pic.size() > 8) {
                    size_t p = 0;
                    uint32_t ptype = rd32be(pic.data() + p);
                    p += 4;
                    uint32_t mlen = rd32be(pic.data() + p);
                    p += 4;
                    if (p + mlen + 8 <= pic.size()) {
                        std::string mime((const char*)pic.data() + p, mlen);
                        p += mlen;
                        uint32_t dlen = rd32be(pic.data() + p);
                        p += 4;
                        if (p + dlen + 12 <= pic.size()) {
                            p += dlen;
                            p += 12;  // width, height, depth, colors
                            uint32_t blen = rd32be(pic.data() + p);
                            p += 4;
                            if (p + blen <= pic.size()) {
                                Picture picout;
                                picout.type = (int)ptype;
                                picout.mime = mime;
                                picout.data.assign(pic.begin() + p, pic.begin() + p + blen);
                                g.pictures.push_back(std::move(picout));
                            }
                        }
                    }
                }
            }
        } else {
            g_put(g, key, val);
        }
    }
    return true;
}

// Разбор метаданных FLAC (блоки). Возвращает указатель на звуковые данные
// (после последнего блока) либо -1 при ошибке. Заполняет g картинками и тегами.
int64_t flac_metadata(const uint8_t* d, size_t n, Group& g) {
    if (n < 4 || memcmp(d, "fLaC", 4) != 0) return -1;
    size_t o = 4;
    for (;;) {
        if (o + 4 > n) return -1;
        uint8_t hdr = d[o];
        uint8_t type = hdr & 0x7f;
        uint32_t len = ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
        o += 4;
        if (o + len > n) return -1;
        if (type == 4) {
            vorbis_items(d + o, len, g);
        } else if (type == 6) {
            const uint8_t* p = d + o;
            size_t L = len;
            if (L < 8) return -1;
            size_t pos = 0;
            Picture pic;
            pic.type = (int)rd32be(p + pos);
            pos += 4;
            uint32_t mlen = rd32be(p + pos);
            pos += 4;
            if (pos + mlen + 4 > L) return -1;
            pic.mime.assign((const char*)p + pos, mlen);
            pos += mlen;
            uint32_t dlen = rd32be(p + pos);
            pos += 4;
            if (pos + dlen + 16 > L) return -1;
            pic.description.assign((const char*)p + pos, dlen);
            pos += dlen;
            pos += 16;  // width height depth colors
            uint32_t blen = rd32be(p + pos);
            pos += 4;
            if (pos + blen > L) return -1;
            pic.data.assign(p + pos, p + pos + blen);
            g.pictures.push_back(std::move(pic));
        }
        if (hdr & 0x80) return (int64_t)(o + len);
        o += len;
    }
}

// Размеры PNG/JPEG для записи FLAC PICTURE.
static void picture_dims(const Picture& p, uint32_t* w, uint32_t* h, uint32_t* depth) {
    *w = *h = *depth = 0;
    if (p.data.size() >= 24 && p.data[0] == 0x89 && p.data[1] == 'P' && p.data[2] == 'N' &&
        p.data[3] == 'G') {
        *w = rd32be(p.data.data() + 16);
        *h = rd32be(p.data.data() + 20);
        *depth = p.data[24];
        return;
    }
    if (p.data.size() >= 2 && p.data[0] == 0xff && p.data[1] == 0xd8) {
        // Ищем SOF-маркер для размеров
        for (size_t i = 2; i + 9 < p.data.size();) {
            if (p.data[i] != 0xff) {
                i++;
                continue;
            }
            uint8_t m = p.data[i + 1];
            if (m == 0xd8 || m == 0xd9 || (m >= 0x01 && m <= 0x0f)) {
                i += 2;
                continue;
            }
            if (i + 4 > p.data.size()) break;
            uint16_t seg = rd16be(p.data.data() + i + 2);
            if (seg < 2) break;
            if (m == 0xc0 || m == 0xc1 || m == 0xc2 || m == 0xc3 || m == 0xc5 || m == 0xc6 ||
                m == 0xc7 || m == 0xc9 || m == 0xca || m == 0xcb || m == 0xcd || m == 0xce ||
                m == 0xcf) {
                if (i + 9 > p.data.size()) break;
                *h = rd16be(p.data.data() + i + 5);
                *w = rd16be(p.data.data() + i + 7);
                *depth = p.data[i + 9];
                return;
            }
            i += 2 + seg;
        }
    }
}

void build_flac_picture(const Picture& pic, std::vector<uint8_t>& out) {
    uint32_t w, h, depth;
    picture_dims(pic, &w, &h, &depth);
    wr32be(out, (uint32_t)pic.type);
    wr32be(out, (uint32_t)pic.mime.size());
    out.insert(out.end(), pic.mime.begin(), pic.mime.end());
    wr32be(out, (uint32_t)pic.description.size());
    out.insert(out.end(), pic.description.begin(), pic.description.end());
    wr32be(out, w);
    wr32be(out, h);
    wr32be(out, depth);
    wr32be(out, 0);
    wr32be(out, (uint32_t)pic.data.size());
    out.insert(out.end(), pic.data.begin(), pic.data.end());
}

// ---------------------------------------------------------------------------
// OGG
// ---------------------------------------------------------------------------

void ogg_parse(const uint8_t* d, size_t n, Group& g) {
    // Ищем первую страницу Ogg.
    size_t o = 0;
    while (o + 27 <= n && memcmp(d + o, "OggS", 4) != 0) o++;
    if (o + 27 > n) return;
    uint32_t serial = rd32le(d + o + 14);
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> cur;
    int guard = 0;
    while (o + 27 <= n && guard++ < 5000) {
        if (memcmp(d + o, "OggS", 4) != 0) {
            o++;
            continue;
        }
        uint32_t s = rd32le(d + o + 14);
        if (s != serial) {
            o++;
            continue;
        }
        uint8_t segc = d[o + 26];
        const uint8_t* table = d + o + 27;
        const uint8_t* body = table + segc;
        size_t blen = 0;
        for (uint8_t i = 0; i < segc; i++) blen += table[i];
        if ((size_t)(body - d) + blen > n) break;
        size_t lpos = 0;
        for (uint8_t i = 0; i < segc; i++) {
            uint8_t l = table[i];
            cur.insert(cur.end(), body + lpos, body + lpos + l);
            lpos += l;
            if (l < 255) {
                packets.push_back(std::move(cur));
                cur.clear();
                if (packets.size() >= 2) break;
            }
        }
        if (packets.size() >= 2) break;
        o += 27 + segc + blen;
        if (blen == 0 && segc == 0) o++;
    }
    if (packets.size() < 2) return;
    // Второй пакет потока — комментарии (Vorbis) или OpusTags (Opus).
    const std::vector<uint8_t>& p = packets[1];
    if (p.size() >= 8 && memcmp(p.data(), "OpusTags", 8) == 0) {
        vorbis_items(p.data() + 8, p.size() - 8, g);
    } else if (p.size() >= 7 && p[0] == 0x03 && memcmp(p.data() + 1, "vorbis", 6) == 0) {
        vorbis_items(p.data() + 7, p.size() - 7, g);
    }
}

}  // namespace tags