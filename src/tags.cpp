#include "tags.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#include "config.h"
#include "media.h"
#include "util.h"

#include "miniz/miniz.h"
#include "i18n.h"

namespace tags {

namespace json = nlohmann;

// ---------------------------------------------------------------------------
// Канонические ключи
// ---------------------------------------------------------------------------

std::string canonical_key(const std::string& key) {
    std::string c;
    for (char ch : key) {
        if (ch == ' ' || ch == '_' || ch == '-') continue;
        c.push_back((char)::tolower((unsigned char)ch));
    }
    static const std::map<std::string, std::string> m = {
        {"title", "title"},
        {"artist", "artist"},
        {"album", "album"},
        {"albumartist", "album_artist"},
        {"composer", "composer"},
        {"genre", "genre"},
        {"date", "date"},
        {"year", "date"},
        {"originaldate", "date"},
        {"track", "track"},
        {"tracknumber", "track"},
        {"disc", "disc"},
        {"discnumber", "disc"},
        {"comment", "comment"},
        {"isrc", "isrc"},
        {"encoder", "encoder"},
        {"lyrics", "lyrics"},
        {"unsyncedlyrics", "lyrics"},
        {"copyright", "copyright"},
        {"copyrightmessage", "copyright"},
        {"cuesheet", "cue_sheet"},
        {"replaygain_track_gain", "replaygain_track_gain"},
        {"replaygain_track_peak", "replaygain_track_peak"},
        {"replaygain_album_gain", "replaygain_album_gain"},
        {"replaygain_album_peak", "replaygain_album_peak"},
    };
    auto it = m.find(c);
    if (it != m.end()) return it->second;
    return key;  // произвольный ключ — как в источнике
}

static bool is_replaygain(const std::string& k) {
    return k.rfind("replaygain_", 0) == 0;
}

static std::string norm_key(const std::string& key) {
    std::string c;
    for (char ch : key) {
        if (ch == ' ' || ch == '_' || ch == '-') continue;
        c.push_back((char)::tolower((unsigned char)ch));
    }
    return c;
}

// Нормализация числового значения для сравнения: M4A хранит track/disc
// бинарно, поэтому «01» читается обратно как «1». Убираем ведущие нули
// (но не у «0») у каждого числа, включая составные вида «1/10».
static std::string norm_num(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = i;
        while (j < s.size() && isdigit((unsigned char)s[j])) j++;
        if (j > i) {
            size_t k = i;
            while (k + 1 < j && s[k] == '0') k++;
            out.append(s, k, j - k);
        }
        if (j >= s.size()) break;
        out.push_back(s[j]);
        i = j + 1;
    }
    return out.empty() ? s : out;
}

// ---------------------------------------------------------------------------
// Имена типов тегов
// ---------------------------------------------------------------------------

const char* tag_type_name(TagType t) {
    switch (t) {
        case TagType::id3v2: return "id3v2";
        case TagType::riff: return "riff";
        case TagType::vorbis: return "vorbis";
        case TagType::apev2: return "apev2";
        case TagType::id3v1: return "id3v1";
        case TagType::mp4: return "mp4";
        default: return "unknown";
    }
}

TagType tag_type_from_string(const std::string& s) {
    if (s == "id3v2" || s == "id3") return TagType::id3v2;
    if (s == "riff") return TagType::riff;
    if (s == "vorbis") return TagType::vorbis;
    if (s == "apev2" || s == "ape") return TagType::apev2;
    if (s == "id3v1") return TagType::id3v1;
    if (s == "mp4") return TagType::mp4;
    return TagType::unknown;
}

bool Group::empty() const {
    if (!cue_sheet.empty()) return false;
    if (!pictures.empty()) return false;
    for (const auto& [k, vs] : fields)
        for (const auto& v : vs)
            if (!util::trim(v).empty()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Бинарные хелперы
// ---------------------------------------------------------------------------

static uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t rd32le(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t syncsafe(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static void wr32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

// Запись 32-битного big-endian по смещению (в отличие от wr32be, которая
// добавляет в конец).
static void wr32be_at(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = (uint8_t)(x >> 24);
    v[off + 1] = (uint8_t)(x >> 16);
    v[off + 2] = (uint8_t)(x >> 8);
    v[off + 3] = (uint8_t)x;
}
static void wr32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 24));
}
static void wr16be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}
static void wr_syncsafe(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)((x >> 21) & 0x7f));
    v.push_back((uint8_t)((x >> 14) & 0x7f));
    v.push_back((uint8_t)((x >> 7) & 0x7f));
    v.push_back((uint8_t)(x & 0x7f));
}

static std::string utf16le_to_utf8(const uint8_t* d, size_t n) {
    std::wstring ws;
    for (size_t i = 0; i + 1 < n; i += 2) ws.push_back((wchar_t)(d[i] | ((uint16_t)d[i + 1] << 8)));
    return util::w2u(ws);
}
static std::string utf16be_to_utf8(const uint8_t* d, size_t n) {
    std::wstring ws;
    for (size_t i = 0; i + 1 < n; i += 2) ws.push_back((wchar_t)(((uint16_t)d[i] << 8) | d[i + 1]));
    return util::w2u(ws);
}
static std::string utf16_to_utf8(const uint8_t* d, size_t n) {
    if (n >= 2 && d[0] == 0xff && d[1] == 0xfe) return utf16le_to_utf8(d + 2, n - 2);
    if (n >= 2 && d[0] == 0xfe && d[1] == 0xff) return utf16be_to_utf8(d + 2, n - 2);
    return utf16be_to_utf8(d, n);
}

// Добавление поля в группу (канонические ключи, дедупликация значений).
static void g_put(Group& g, const std::string& key, const std::string& value) {
    if (key.empty()) return;
    std::string k = canonical_key(key);
    if (k == "lyrics") {
        if (g.fields["lyrics"].empty()) g.fields["lyrics"].push_back(value);
        return;
    }
    if (k == "cue_sheet") {
        // CueSheet живёт отдельным членом группы (а не в fields): его хранят
        // парсеры (Vorbis CUESHEET, APEv2 Cuesheet, ID3v2 TXXX) и читают
        // писатели/plan/валидация через g.cue_sheet.
        if (g.cue_sheet != value) g.cue_sheet = value;
        return;
    }
    auto& v = g.fields[k];
    for (const auto& x : v)
        if (x == value) return;
    v.push_back(value);
}

static bool has_value(const std::vector<std::string>& v) {
    for (const auto& s : v)
        if (!util::trim(s).empty()) return true;
    return false;
}

// Добавление поля в каноническую агрегацию TagSet (дедупликация значений).
static void put_field(TagSet& ts, const std::string& key, const std::string& value) {
    if (key.empty()) return;
    std::string k = canonical_key(key);
    if (k == "lyrics") {
        if (ts.fields["lyrics"].empty()) ts.fields["lyrics"].push_back(value);
        return;
    }
    auto& v = ts.fields[k];
    for (const auto& x : v)
        if (x == value) return;
    v.push_back(value);
}

// ---------------------------------------------------------------------------
// Имена ключей для записи по форматам (в JSON → tag.key_map)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FLAC / Vorbis comment
// ---------------------------------------------------------------------------

static void build_vorbis_comment(const Group& g, const std::map<std::string, std::string>& key_map,
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
static int64_t flac_metadata(const uint8_t* d, size_t n, Group& g) {
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

static void build_flac_picture(const Picture& pic, std::vector<uint8_t>& out) {
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

static void build_apev2(const Group& g, const std::map<std::string, std::string>& key_map,
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

static bool apev2_parse(const uint8_t* d, size_t n, Group& g) {
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

// ---------------------------------------------------------------------------
// ID3v2
// ---------------------------------------------------------------------------

static void id3v2_text_frame(std::vector<uint8_t>& out, const char* id, const std::string& text) {
    std::vector<uint8_t> payload;
    payload.push_back(3);  // UTF-8
    payload.insert(payload.end(), text.begin(), text.end());
    out.insert(out.end(), id, id + 4);
    wr_syncsafe(out, (uint32_t)payload.size());
    wr16be(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
}

static void id3v2_apic(std::vector<uint8_t>& out, const Picture& pic) {
    std::vector<uint8_t> payload;
    payload.push_back(3);  // encoding UTF-8
    payload.insert(payload.end(), pic.mime.begin(), pic.mime.end());
    payload.push_back(0);
    payload.push_back((uint8_t)pic.type);
    payload.insert(payload.end(), pic.description.begin(), pic.description.end());
    payload.push_back(0);
    payload.insert(payload.end(), pic.data.begin(), pic.data.end());
    const uint8_t apic_id[] = {'A', 'P', 'I', 'C'};
    out.insert(out.end(), apic_id, apic_id + 4);
    wr_syncsafe(out, (uint32_t)payload.size());
    wr16be(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
}

static void id3v2_comm(std::vector<uint8_t>& out, const std::string& text) {
    std::vector<uint8_t> payload;
    payload.push_back(3);
    payload.push_back('e');
    payload.push_back('n');
    payload.push_back('g');
    payload.push_back(0);  // пустой дескриптор
    payload.insert(payload.end(), text.begin(), text.end());
    const uint8_t comm_id[] = {'C', 'O', 'M', 'M'};
    out.insert(out.end(), comm_id, comm_id + 4);
    wr_syncsafe(out, (uint32_t)payload.size());
    wr16be(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
}

static void id3v2_uslt(std::vector<uint8_t>& out, const std::string& text) {
    std::vector<uint8_t> payload;
    payload.push_back(3);
    payload.push_back('u');
    payload.push_back('n');
    payload.push_back('d');
    payload.push_back(0);  // пустой дескриптор
    payload.insert(payload.end(), text.begin(), text.end());
    const uint8_t uslt_id[] = {'U', 'S', 'L', 'T'};
    out.insert(out.end(), uslt_id, uslt_id + 4);
    wr_syncsafe(out, (uint32_t)payload.size());
    wr16be(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
}

static void id3v2_txxx(std::vector<uint8_t>& out, const std::string& desc, const std::string& text) {
    std::vector<uint8_t> payload;
    payload.push_back(3);
    payload.insert(payload.end(), desc.begin(), desc.end());
    payload.push_back(0);
    payload.insert(payload.end(), text.begin(), text.end());
    const uint8_t txxx_id[] = {'T', 'X', 'X', 'X'};
    out.insert(out.end(), txxx_id, txxx_id + 4);
    wr_syncsafe(out, (uint32_t)payload.size());
    wr16be(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
}

static void build_id3v2(const Group& g, const std::map<std::string, std::string>& key_map,
                        std::vector<uint8_t>& out) {
    std::vector<uint8_t> frames;
    for (const auto& [key, values] : g.fields) {
        if (key == "lyrics") continue;
        const char* id = nullptr;
        std::string replayg;
        if (is_replaygain(key)) {
            replayg = key;
            for (auto& ch : replayg) ch = (char)::toupper((unsigned char)ch);
        } else {
            auto it = key_map.find(key);
            if (it != key_map.end()) id = it->second.c_str();
        }
        for (const auto& val : values) {
            if (id && std::string(id) == "COMM") id3v2_comm(frames, val);
            else if (id) id3v2_text_frame(frames, id, val);
            else if (is_replaygain(key)) id3v2_txxx(frames, replayg, val);
            else id3v2_txxx(frames, key, val);
        }
    }
    auto lyr = g.fields.find("lyrics");
    if (lyr != g.fields.end() && !lyr->second.empty())
        id3v2_uslt(frames, lyr->second[0]);
    for (const auto& p : g.pictures) id3v2_apic(frames, p);

    out.insert(out.end(), {'I', 'D', '3', 0x04, 0x00, 0x00});
    wr_syncsafe(out, (uint32_t)frames.size());
    out.insert(out.end(), frames.begin(), frames.end());
}

static std::string id3_key(const std::string& id) {
    static const std::map<std::string, std::string> m = {
        {"TIT2", "title"}, {"TPE1", "artist"}, {"TPE2", "album_artist"},
        {"TALB", "album"}, {"TCOM", "composer"}, {"TCON", "genre"}, {"TDRC", "date"},
        {"TRCK", "track"}, {"TPOS", "disc"}, {"TSRC", "isrc"}, {"TENC", "encoder"},
        {"TCOP", "copyright"},
    };
    auto it = m.find(id);
    return it != m.end() ? it->second : std::string();
}

static bool id3v2_parse(const uint8_t* d, size_t n, Group& g) {
    if (n < 10 || memcmp(d, "ID3", 3) != 0) return false;
    uint32_t tag_size = syncsafe(d + 6);
    if (10 + tag_size > n) return false;
    size_t o = 10;
    size_t end = 10 + tag_size;
    bool v24 = d[3] == 4;
    while (o + 10 <= end) {
        const uint8_t* h = d + o;
        if (memcmp(h, "\0\0\0\0", 4) == 0) break;
        std::string id((const char*)h, 4);
        uint32_t fsize = v24 ? syncsafe(h + 4) : rd32be(h + 4);
        o += 10;
        if (o + fsize > end) break;
        const uint8_t* f = d + o;
        if (id == "APIC") {
            if (fsize < 4) { o += fsize; continue; }
            uint8_t enc = f[0];
            size_t p = 1;
            while (p < fsize && f[p] != 0) p++;
            std::string mime((const char*)f + 1, p - 1);
            p++;
            if (p >= fsize) { o += fsize; continue; }
            uint8_t ptype = f[p];
            p++;
            Picture pic;
            pic.type = (int)ptype;
            pic.mime = mime;
            if (enc == 1 || enc == 2) {
                size_t q = p;
                while (q + 1 < fsize && !(f[q] == 0 && f[q + 1] == 0)) q += 2;
                pic.description = utf16_to_utf8(f + p, q - p);
                p = q + 2;
            } else {
                size_t q = p;
                while (q < fsize && f[q] != 0) q++;
                pic.description.assign((const char*)f + p, q - p);
                p = q + 1;
            }
            if (p < fsize) pic.data.assign(f + p, f + fsize);
            g.pictures.push_back(std::move(pic));
        } else if (id == "USLT") {
            // encoding, язык (3 байта), дескриптор (zstring), текст. Дескриптор
            // обязан быть пропущен, иначе текст читается с ведущим NUL и не
            // совпадает с записанным (валидация «лирика не сохранилась»).
            if (fsize > 4) {
                uint8_t enc = f[0];
                size_t p = 4;
                size_t q = p;
                if (enc == 1 || enc == 2) {
                    while (q + 1 < fsize && !(f[q] == 0 && f[q + 1] == 0)) q += 2;
                } else {
                    while (q < fsize && f[q] != 0) q++;
                }
                size_t tstart = q + (enc == 1 || enc == 2 ? 2 : 1);
                if (tstart < fsize) {
                    size_t tlen = fsize - tstart;
                    std::string s;
                    if (enc == 1 || enc == 2) s = utf16_to_utf8(f + tstart, tlen);
                    else {
                        while (tlen > 0 && f[tstart + tlen - 1] == 0) tlen--;
                        s.assign((const char*)f + tstart, tlen);
                    }
                    if (g.fields["lyrics"].empty()) g.fields["lyrics"].push_back(s);
                }
            }
        } else if (id == "COMM") {
            // encoding, язык (3 байта), описание, текст
            if (fsize > 4) {
                uint8_t enc = f[0];
                size_t p = 4;
                size_t q = p;
                if (enc == 1 || enc == 2) {
                    while (q + 1 < fsize && !(f[q] == 0 && f[q + 1] == 0)) q += 2;
                } else {
                    while (q < fsize && f[q] != 0) q++;
                }
                size_t tstart = q + (enc == 1 || enc == 2 ? 2 : 1);
                if (tstart < fsize) {
                    size_t tlen = fsize - tstart;
                    std::string s;
                    if (enc == 1 || enc == 2) s = utf16_to_utf8(f + tstart, tlen);
                    else {
                        while (tlen > 0 && f[tstart + tlen - 1] == 0) tlen--;
                        s.assign((const char*)f + tstart, tlen);
                    }
                    if (!s.empty()) g_put(g, "comment", s);
                }
            }
        } else if (id == "TXXX") {
            if (fsize > 2) {
                uint8_t enc = f[0];
                if (enc == 1 || enc == 2) {
                    // UTF-16 — сложнее, пропускаем
                } else {
                    size_t p = 1;
                    size_t q = p;
                    while (q < fsize && f[q] != 0) q++;
                    std::string desc((const char*)f + p, q - p);
                    std::string val((const char*)f + q + 1, fsize - q - 1);
                    g_put(g, desc, val);
                }
            }
        } else if (id.size() == 4 && id[0] == 'T' && id != "TXXX") {
            if (fsize > 1) {
                uint8_t enc = f[0];
                const uint8_t* t = f + 1;
                size_t tl = fsize - 1;
                std::string s;
                if (enc == 1 || enc == 2) {
                    if (tl >= 2 && t[tl - 1] == 0 && t[tl - 2] == 0) tl -= 2;
                    if (tl >= 2 && t[0] == 0xff && t[1] == 0xfe) s = utf16le_to_utf8(t + 2, tl - 2);
                    else if (tl >= 2 && t[0] == 0xfe && t[1] == 0xff) s = utf16be_to_utf8(t + 2, tl - 2);
                    else s = utf16be_to_utf8(t, tl);
                } else {
                    while (tl > 0 && t[tl - 1] == 0) tl--;
                    s.assign((const char*)t, tl);
                }
                std::string key = id3_key(id);
                if (!key.empty()) g_put(g, key, s);
            }
        }
        o += fsize;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ID3v1
// ---------------------------------------------------------------------------

static void id3v1_write(const Group& g, std::vector<uint8_t>& out) {
    auto field = [&](const char* name) {
        auto it = g.fields.find(name);
        if (it == g.fields.end() || it->second.empty()) return std::string();
        return it->second[0];
    };
    out.clear();
    out.insert(out.end(), {'T', 'A', 'G'});
    auto put = [&](std::string s, int max) {
        std::string v;
        for (size_t i = 0; i < s.size() && v.size() < (size_t)max; i++) v.push_back((char)s[i]);
        out.insert(out.end(), v.begin(), v.end());
        while ((int)v.size() < max) {
            v.push_back(0);
            out.push_back(0);
        }
    };
    put(field("title"), 30);
    put(field("artist"), 30);
    put(field("album"), 30);
    std::string year = field("date");
    if (year.size() > 4) year = year.substr(0, 4);
    put(year, 4);
    std::string comment = field("comment");
    put(comment, 28);
    out.push_back(0);
    // ID3v1.1: track
    std::string track = field("track");
    try {
        out.back() = (uint8_t)std::stoi(track);
    } catch (...) {
        out.back() = 0;
    }
    out.push_back(0);  // genre
}

static void id3v1_parse(const uint8_t* d, size_t n, Group& g) {
    if (n < 128 || memcmp(d, "TAG", 3) != 0) return;
    auto txt = [&](size_t off, size_t len, const std::string& key) {
        std::string s((const char*)d + off, len);
        size_t z = s.find('\0');
        if (z != std::string::npos) s.resize(z);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (!s.empty()) g_put(g, key, s);
    };
    txt(3, 30, "title");
    txt(33, 30, "artist");
    txt(63, 30, "album");
    txt(93, 4, "date");
    bool v11 = d[125] != 0 && d[126] == 0;
    if (v11) {
        txt(97, 28, "comment");
        g_put(g, "track", std::to_string((int)d[125]));
    } else {
        txt(97, 30, "comment");
    }
}

// ---------------------------------------------------------------------------
// OGG
// ---------------------------------------------------------------------------

static void ogg_parse(const uint8_t* d, size_t n, Group& g) {
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

// ---------------------------------------------------------------------------
// MP4 ilst
// ---------------------------------------------------------------------------

struct M4aBox {
    uint64_t off = 0;
    uint64_t size = 0;
    uint8_t type[4] = {0, 0, 0, 0};
};

static std::vector<M4aBox> m4a_children(const uint8_t* d, uint64_t start, uint64_t end) {
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
static void mp4_ilst_parse(const uint8_t* d, size_t n, Group& g) {
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
                                static const std::map<std::string, std::string> mp4back = {
                                    {"\xa9"
                                     "nam",
                                     "title"},
                                    {"\xa9"
                                     "ART",
                                     "artist"},
                                    {"\xa9"
                                     "alb",
                                     "album"},
                                    {"aART", "album_artist"},
                                    {"\xa9"
                                     "wrt",
                                     "composer"},
                                    {"\xa9"
                                     "gen",
                                     "genre"},
                                    {"\xa9"
                                     "day",
                                     "date"},
                                    {"\xa9"
                                     "cmt",
                                     "comment"},
                                    {"\xa9"
                                     "too",
                                     "encoder"},
                                    {"\xa9"
                                     "lyr",
                                     "lyrics"},
                                    {"cprt", "copyright"},
                                };
                                auto it2 = mp4back.find(key);
                                g_put(g, it2 != mp4back.end() ? it2->second : key, s);
                            }
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// WAV: LIST INFO + встроенные ID3v2-блоки
// ---------------------------------------------------------------------------

static const char* wav_info_key(const char* id) {
    // Имена в точности как отдаёт ffprobe для WAV (LIST INFO).
    if (memcmp(id, "IART", 4) == 0) return "artist";
    if (memcmp(id, "INAM", 4) == 0) return "title";
    if (memcmp(id, "IPRD", 4) == 0) return "album";
    if (memcmp(id, "ITRK", 4) == 0) return "track";
    if (memcmp(id, "ICMT", 4) == 0) return "comment";
    if (memcmp(id, "ICRD", 4) == 0) return "date";
    if (memcmp(id, "IDIT", 4) == 0) return "date";
    if (memcmp(id, "IGNR", 4) == 0) return "genre";
    if (memcmp(id, "ICOP", 4) == 0) return "copyright";
    if (memcmp(id, "ISFT", 4) == 0) return "encoder";
    return nullptr;  // остальные (напр. IAAR) — под raw-именем, как в источнике
}

// LIST-чанки с INFO-подчанками (RIFF-структура WAV).
static void wav_list_info(const uint8_t* d, size_t n, Group& g) {
    size_t i = 0;
    while (i + 8 <= n) {
        const uint8_t* p = (const uint8_t*)memchr(d + i, 'L', n - i);
        if (!p) break;
        i = (size_t)(p - d);
        if (i + 8 <= n && memcmp(p, "LIST", 4) == 0) {
            uint32_t lsize = rd32le(p + 4);
            size_t body = lsize > (uint32_t)(n - i - 8) ? (size_t)(n - i - 8) : (size_t)lsize;
            if (body >= 4 && memcmp(p + 8, "INFO", 4) == 0) {
                size_t o = i + 12;
                size_t end = i + 8 + body;
                while (o + 8 <= end) {
                    const uint8_t* sub = d + o;
                    uint32_t subsz = rd32le(sub + 4);
                    size_t data = o + 8;
                    if (data + subsz > end) break;
                    const char* key = wav_info_key((const char*)sub);
                    if (key) {
                        std::string val((const char*)d + data, subsz);
                        size_t z = val.find('\0');
                        if (z != std::string::npos) val.resize(z);
                        g_put(g, key, val);
                    }
                    o = data + subsz + (subsz & 1);
                }
            }
            i += 8 + (size_t)lsize + ((size_t)lsize & 1);
        } else {
            i += 1;
        }
    }
}

// ID3v2-блоки в WAV: могут стоять в начале и/или после аудио — ищем все.
static void wav_id3v2_blocks(const uint8_t* d, size_t n, Group& g) {
    size_t pos = 0;
    while (pos + 10 <= n) {
        const uint8_t* p = (const uint8_t*)memchr(d + pos, 'I', n - pos);
        if (!p) break;
        pos = (size_t)(p - d);
        if (pos + 10 <= n && memcmp(p, "ID3", 3) == 0 && id3v2_parse(p, n - pos, g)) {
            // Продвигаемся только по валидному блоку; иначе случайное "ID3"
            // в данных PCM дало бы гигантский tag_size и пропуск реального блока.
            uint32_t tsz = syncsafe(p + 6);
            pos += 10 + tsz;
        } else {
            pos += 1;
        }
    }
}

// ---------------------------------------------------------------------------
// Извлечение тегов: нативные разборы в отдельные группы
// ---------------------------------------------------------------------------

// Пересобирает каноническую агрегацию из групп и детектирует противоречия.
static void rebuild_canonical(TagSet& ts) {
    ts.fields.clear();
    ts.pictures.clear();
    ts.cue_sheet.clear();
    ts.conflict = false;
    std::map<std::string, std::map<std::string, uint32_t>> key_srcs;
    for (size_t gi = 0; gi < ts.groups.size(); gi++) {
        const Group& g = ts.groups[gi];
        for (const auto& [k, vs] : g.fields) {
            for (const auto& v : vs) {
                put_field(ts, k, v);
                key_srcs[k][v] |= (uint32_t)(1u << gi);
            }
        }
        for (const auto& p : g.pictures) ts.pictures.push_back(p);
        if (!g.cue_sheet.empty()) {
            if (ts.cue_sheet.empty()) ts.cue_sheet = g.cue_sheet;
            else if (ts.cue_sheet != g.cue_sheet) ts.conflict = true;
        }
    }
    // Поле с ≥2 разными значениями из ≥2 разных групп — противоречие.
    for (const auto& [k, vm] : key_srcs) {
        if (vm.size() < 2) continue;
        uint32_t all = 0;
        for (const auto& [v, mask] : vm) all |= mask;
        if (__builtin_popcount(all) >= 2) ts.conflict = true;
    }
    // Слот картинки (тип+mime+описание) с разным содержимым — противоречие.
    for (size_t i = 0; i < ts.pictures.size(); i++) {
        for (size_t j = i + 1; j < ts.pictures.size(); j++) {
            const Picture& a = ts.pictures[i];
            const Picture& b = ts.pictures[j];
            if (a.type == b.type && a.mime == b.mime && a.description == b.description &&
                a.data != b.data) {
                ts.conflict = true;
            }
        }
    }
    ts.present = !(ts.fields.empty() && ts.pictures.empty() && ts.cue_sheet.empty());
}

TagSet extract_tags(const std::string& path, const media::Probe& probe, bool native_reader) {
    TagSet ts;
    auto data = util::read_file(path);
    if (data.empty()) return ts;
    std::string fmt = probe.format_name;
    bool is_matroska = fmt.find("matroska") != std::string::npos ||
                       fmt.find("webm") != std::string::npos;

    // ID3v1 в самом конце.
    if (data.size() >= 128 && memcmp(data.data() + data.size() - 128, "TAG", 3) == 0) {
        Group g;
        g.type = TagType::id3v1;
        id3v1_parse(data.data() + data.size() - 128, 128, g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
    }
    // APEv2 в конце; бывает как после ID3v1, так и без него.
    if (data.size() >= 160 && memcmp(data.data() + data.size() - 160, "APETAGEX", 8) == 0) {
        Group g;
        g.type = TagType::apev2;
        apev2_parse(data.data(), data.size() - 128, g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
    } else if (data.size() >= 32 && memcmp(data.data() + data.size() - 32, "APETAGEX", 8) == 0) {
        Group g;
        g.type = TagType::apev2;
        apev2_parse(data.data(), data.size(), g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
    }
    // ID3v2 в начале.
    if (data.size() >= 10 && memcmp(data.data(), "ID3", 3) == 0) {
        Group g;
        g.type = TagType::id3v2;
        id3v2_parse(data.data(), data.size(), g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
    }

    // Форматные парсеры.
    if (data.size() >= 4 && memcmp(data.data(), "fLaC", 4) == 0) {
        Group g;
        g.type = TagType::vorbis;
        flac_metadata(data.data(), data.size(), g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
    } else if (data.size() >= 4 && memcmp(data.data(), "OggS", 4) == 0) {
        Group g;
        g.type = TagType::vorbis;
        ogg_parse(data.data(), data.size(), g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
    } else if (fmt.find("mp4") != std::string::npos || fmt.find("m4a") != std::string::npos ||
               fmt.find("mov") != std::string::npos) {
        Group g;
        g.type = TagType::mp4;
        mp4_ilst_parse(data.data(), data.size(), g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
    } else if (fmt.find("wav") != std::string::npos || fmt.find("riff") != std::string::npos) {
        // WAV: ffprobe при дублирующих ключах тегов теряет часть значений,
        // поэтому разбираем LIST INFO и ID3v2-блоки напрямую.
        Group g;
        g.type = TagType::riff;
        wav_list_info(data.data(), data.size(), g);
        if (!g.empty()) ts.groups.push_back(std::move(g));
        Group g2;
        g2.type = TagType::id3v2;
        wav_id3v2_blocks(data.data(), data.size(), g2);
        if (!g2.empty()) ts.groups.push_back(std::move(g2));
    }

    // Неизвестный контейнер с тегами по ffprobe: берём текст, но полным разбором
    // (картинки и т.п.) не гарантируем — помечаем файл как неполный.
    if (ts.groups.empty() && !native_reader) {
        Group g;
        g.type = TagType::unknown;
        for (const auto& [k, v] : probe.tags)
            for (const auto& val : v) g_put(g, k, val);
        if (!g.empty()) {
            ts.groups.push_back(std::move(g));
            ts.complete = false;
        }
    }

    if (is_matroska) ts.complete = false;  // Matroska-теги/картинки не разбираем
    rebuild_canonical(ts);
    return ts;
}

// ---------------------------------------------------------------------------
// Объединение групп (встроенные + sidecar)
// ---------------------------------------------------------------------------

TagSet merge_tags(TagSet a, const TagSet& b) {
    for (const auto& g : b.groups) {
        bool found = false;
        for (auto& ag : a.groups) {
            if (ag.type != g.type) continue;
            for (const auto& [k, vs] : g.fields)
                for (const auto& v : vs) g_put(ag, k, v);
            for (const auto& p : g.pictures) {
                bool dup = false;
                for (const auto& ap : ag.pictures)
                    if (ap.type == p.type && ap.data == p.data) dup = true;
                if (!dup) ag.pictures.push_back(p);
            }
            if (ag.cue_sheet.empty() && !g.cue_sheet.empty()) ag.cue_sheet = g.cue_sheet;
            found = true;
            break;
        }
        if (!found) a.groups.push_back(g);
    }
    a.complete = a.complete && b.complete;
    rebuild_canonical(a);
    return a;
}

// ---------------------------------------------------------------------------
// Планирование: что встроить и что вынести в sidecar
// ---------------------------------------------------------------------------

// Вместимость типа тегов по контенту группы (форматные ограничения из JSON).
static bool content_fits_type(const config::Format& fmt, TagType /*t*/, const Group& g) {
    if (!fmt.tag_write_supported) return false;
    if (!fmt.tag_replaygain_allowed) {
        for (const auto& [k, vs] : g.fields)
            if (is_replaygain(k)) return false;
    }
    if (!fmt.tag_allowed_keys.empty()) {
        for (const auto& [k, vs] : g.fields) {
            if (std::find(fmt.tag_allowed_keys.begin(), fmt.tag_allowed_keys.end(), k) !=
                fmt.tag_allowed_keys.end())
                continue;
            if (!has_value(vs)) continue;
            return false;
        }
    }
    if (!fmt.tag_pictures_allowed && !g.pictures.empty()) return false;
    if (!fmt.tag_cue_sheet_allowed && !g.cue_sheet.empty()) return false;
    return true;
}

// Вместимость по возможностям формата (tag.capabilities).
static bool caps_ok(const Group& g, const std::map<std::string, bool>& tag_caps,
                    std::vector<std::string>* missing) {
    std::vector<std::string> need;
    auto need_cap = [&](const std::string& cap) {
        for (const auto& c : need)
            if (c == cap) return;
        need.push_back(cap);
    };
    for (const auto& [key, values] : g.fields) {
        if (is_replaygain(key)) need_cap("replay_gain");
        else if (key == "lyrics") need_cap("lyrics");
        else if (has_value(values)) need_cap("text");
    }
    if (!g.pictures.empty()) need_cap("pictures");
    if (!g.cue_sheet.empty()) need_cap("cue_sheet");

    std::vector<std::string> unsup;
    for (const auto& c : need) {
        auto it = tag_caps.find(c);
        if (it == tag_caps.end() || !it->second) unsup.push_back(c);
    }
    if (missing) *missing = unsup;
    return unsup.empty();
}

TagPlan plan_tags(const TagSet& ts, const std::vector<TagType>& target_types,
                  const config::Format& fmt, bool allow_merge) {
    TagPlan plan;
    std::vector<Group> groups;
    for (const auto& g : ts.groups) {
        if (g.empty()) continue;
        Group c = g;
        bool dirty = false;
        for (auto& [k, vs] : c.fields) {
            vs.erase(std::remove_if(vs.begin(), vs.end(),
                                    [](const std::string& s) { return s.empty(); }),
                     vs.end());
            if (vs.empty()) dirty = true;
        }
        if (dirty) {
            Group c2;
            c2.type = c.type;
            c2.cue_sheet = c.cue_sheet;
            c2.pictures = std::move(c.pictures);
            for (auto& [k, vs] : c.fields)
                if (!vs.empty()) c2.fields[k] = std::move(vs);
            c = std::move(c2);
        }
        if (!c.empty()) groups.push_back(std::move(c));
    }
    if (groups.empty()) return plan;

    auto native = [&]() -> TagType {
        for (auto t : target_types)
            if (t != TagType::unknown) return t;
        return TagType::unknown;
    };

    if (allow_merge && !ts.conflict) {
        Group merged;
        for (const auto& g : groups) {
            for (const auto& [k, vs] : g.fields)
                for (const auto& v : vs) g_put(merged, k, v);
            for (const auto& p : g.pictures) merged.pictures.push_back(p);
            if (merged.cue_sheet.empty() && !g.cue_sheet.empty()) merged.cue_sheet = g.cue_sheet;
        }
        TagType t = native();
        std::vector<std::string> missing;
        if (t != TagType::unknown && content_fits_type(fmt, t, merged) &&
            caps_ok(merged, fmt.tag_caps, &missing)) {
            plan.embed.emplace_back(t, std::move(merged));
            return plan;
        }
    }

    if (!allow_merge && groups.size() == 1) {
        TagType t = native();
        std::vector<std::string> missing;
        if (t != TagType::unknown && content_fits_type(fmt, t, groups[0]) &&
            caps_ok(groups[0], fmt.tag_caps, &missing)) {
            plan.embed.emplace_back(t, groups[0]);
            return plan;
        }
        plan.sidecar.push_back(groups[0]);
        return plan;
    }

    std::set<TagType> used;
    for (const auto& g : groups) {
        bool supp = false;
        for (auto t : target_types)
            if (t == g.type && !used.count(t)) {
                supp = true;
                used.insert(t);
                break;
            }
        std::vector<std::string> missing;
        if (supp && content_fits_type(fmt, g.type, g) && caps_ok(g, fmt.tag_caps, &missing))
            plan.embed.emplace_back(g.type, g);
        else
            plan.sidecar.push_back(g);
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Запись встроенных тегов (одна группа)
// ---------------------------------------------------------------------------

std::string write_group(const std::string& path, const config::Format& fmt, TagType type,
                        const Group& g) {
    if (g.empty()) return "";
    const auto& km = fmt.tag_key_map;
    if (fmt.tag_write_method == "id3v1_append") {
        auto data = util::read_file(path);
        if (data.size() < 128) return i18n::str("file is too small for ID3v1");
        if (data.size() >= 128 && memcmp(data.data() + data.size() - 128, "TAG", 3) == 0)
            data.resize(data.size() - 128);
        std::vector<uint8_t> tag;
        id3v1_write(g, tag);
        data.insert(data.end(), tag.begin(), tag.end());
        return util::write_file(path, data) ? "" : i18n::str("could not write ID3v1");
    }
    if (fmt.tag_write_method == "flac_metadata") {
        auto data = util::read_file(path);
        Group ignored;
        int64_t audio = flac_metadata(data.data(), data.size(), ignored);
        if (audio < 0) return i18n::str("not a FLAC file or corrupted metadata");
        std::vector<uint8_t> out;
        out.insert(out.end(), data.begin(), data.begin() + 4);
        size_t o = 4;
        while (o < (size_t)audio) {
            uint8_t hdr = data[o];
            uint8_t btype = hdr & 0x7f;
            uint32_t len =
                ((uint32_t)data[o + 1] << 16) | ((uint32_t)data[o + 2] << 8) | data[o + 3];
            if (btype == 4 || btype == 6) {
            } else {
                out.push_back((uint8_t)(btype | (hdr & 0x80)));
                out.push_back(data[o + 1]);
                out.push_back(data[o + 2]);
                out.push_back(data[o + 3]);
                out.insert(out.end(), data.begin() + o + 4, data.begin() + o + 4 + len);
            }
            o += 4 + len;
        }
        std::vector<uint8_t> vc;
        build_vorbis_comment(g, km, vc);
        out.push_back(0x04);
        out.push_back((uint8_t)(vc.size() >> 16));
        out.push_back((uint8_t)(vc.size() >> 8));
        out.push_back((uint8_t)vc.size());
        out.insert(out.end(), vc.begin(), vc.end());
        for (const auto& pic : g.pictures) {
            std::vector<uint8_t> pb;
            build_flac_picture(pic, pb);
            out.push_back(0x06);
            out.push_back((uint8_t)(pb.size() >> 16));
            out.push_back((uint8_t)(pb.size() >> 8));
            out.push_back((uint8_t)pb.size());
            out.insert(out.end(), pb.begin(), pb.end());
        }
        size_t hpos = 4;
        size_t last_hdr = hpos;
        for (size_t i = hpos; i + 4 <= out.size();) {
            last_hdr = i;
            uint32_t blen = ((uint32_t)out[i + 1] << 16) | ((uint32_t)out[i + 2] << 8) | out[i + 3];
            i += 4 + blen;
        }
        for (size_t i = hpos; i + 4 <= out.size();) {
            out[i] &= 0x7f;
            uint32_t blen = ((uint32_t)out[i + 1] << 16) | ((uint32_t)out[i + 2] << 8) | out[i + 3];
            i += 4 + blen;
        }
        out[last_hdr] |= 0x80;
        out.insert(out.end(), data.begin() + audio, data.end());
        return util::write_file(path, out) ? "" : i18n::str("could not write FLAC tags");
    }
    if (fmt.tag_write_method == "apev2_tail") {
        std::vector<uint8_t> tag;
        build_apev2(g, km, tag);
        auto data = util::read_file(path);
        if (data.size() >= 128 && memcmp(data.data() + data.size() - 128, "TAG", 3) == 0)
            data.resize(data.size() - 128);
        if (data.size() >= 32 && memcmp(data.data() + data.size() - 32, "APETAGEX", 8) == 0) {
            uint32_t tag_size = rd32le(data.data() + data.size() - 32 + 12);
            if (tag_size <= data.size()) data.resize(data.size() - tag_size);
        }
        data.insert(data.end(), tag.begin(), tag.end());
        return util::write_file(path, data) ? "" : i18n::str("could not write APEv2");
    }
    if (fmt.tag_write_method == "id3v2_header") {
        std::vector<uint8_t> tag;
        build_id3v2(g, km, tag);
        auto data = util::read_file(path);
        if (data.size() >= 10 && memcmp(data.data(), "ID3", 3) == 0) {
            uint32_t tag_size = syncsafe(data.data() + 6);
            if (10 + tag_size <= data.size()) data.erase(data.begin(), data.begin() + 10 + tag_size);
        }
        data.insert(data.begin(), tag.begin(), tag.end());
        return util::write_file(path, data) ? "" : i18n::str("could not write ID3v2");
    }
    if (fmt.tag_write_method == "mp4_ilst") {
        auto data = util::read_file(path);
        size_t mdat = std::string::npos, moov = std::string::npos;
        for (size_t i = 0; i + 4 <= data.size(); i++) {
            if (mdat == std::string::npos && memcmp(data.data() + i, "mdat", 4) == 0)
                mdat = i;
            if (moov == std::string::npos && memcmp(data.data() + i, "moov", 4) == 0)
                moov = i;
        }
        if (mdat == std::string::npos || moov == std::string::npos)
            return i18n::str("no mdat/moov in M4A");
        if (moov < mdat)
            return i18n::str("moov before mdat — offset rewriting is not supported (use a sidecar)");

        uint64_t moov_off = moov - 4;
        uint64_t moov_sz = rd32be(data.data() + moov_off);
        if (moov_sz < 8 || moov_off + moov_sz > data.size()) return i18n::str("moov is corrupted");

        M4aBox udta{0, 0, {'u', 'd', 't', 'a'}};
        M4aBox meta{0, 0, {'m', 'e', 't', 'a'}};
        M4aBox ilst{0, 0, {'i', 'l', 's', 't'}};
        bool found_udta = false, found_meta = false, found_ilst = false;
        for (const auto& b : m4a_children(data.data(), moov_off + 8, moov_off + moov_sz)) {
            if (memcmp(b.type, "udta", 4) == 0) { udta = b; found_udta = true; }
        }
        if (found_udta) {
            for (const auto& b : m4a_children(data.data(), udta.off + 8, udta.off + udta.size)) {
                if (memcmp(b.type, "meta", 4) == 0) { meta = b; found_meta = true; }
            }
        }
        if (found_meta) {
            for (const auto& b : m4a_children(data.data(), meta.off + 12, meta.off + meta.size)) {
                if (memcmp(b.type, "ilst", 4) == 0) { ilst = b; found_ilst = true; }
            }
        }
        if (!found_udta || !found_meta || !found_ilst)
            return i18n::str("no udta/meta/ilst in moov (ffmpeg should create them)");

        std::vector<uint8_t> new_ilst;
        auto add_item = [&](const std::string& key4, uint32_t dataflags,
                            const std::vector<uint8_t>& val) {
            std::vector<uint8_t> data_box;
            data_box.insert(data_box.end(), {0, 0, 0, 0});
            data_box.insert(data_box.end(), {'d', 'a', 't', 'a'});
            wr32be(data_box, dataflags);
            wr32be(data_box, 0);
            data_box.insert(data_box.end(), val.begin(), val.end());
            wr32be_at(data_box, 0, (uint32_t)data_box.size());
            std::vector<uint8_t> item;
            item.insert(item.end(), {0, 0, 0, 0});
            item.insert(item.end(), key4.begin(), key4.end());
            item.insert(item.end(), data_box.begin(), data_box.end());
            wr32be_at(item, 0, (uint32_t)item.size());
            new_ilst.insert(new_ilst.end(), item.begin(), item.end());
        };
        for (const auto& [key, values] : g.fields) {
            bool is_binary = std::find(fmt.tag_numeric_fields.begin(),
                                       fmt.tag_numeric_fields.end(), key) !=
                             fmt.tag_numeric_fields.end();
            if (is_binary) {
                for (const auto& val : values) {
                    unsigned n = 0;
                    try { n = (unsigned)std::stoul(val); } catch (...) {}
                    std::vector<uint8_t> v(8, 0);
                    v[2] = (uint8_t)(n >> 8);
                    v[3] = (uint8_t)n;
                    if (key == "track") { v.resize(8); add_item("trkn", 0, v); }
                    else if (key == "disc") { v.resize(6); add_item("disk", 0, v); }
                }
            } else if (is_replaygain(key)) {
                return i18n::str("ReplayGain is not supported in M4A (use a sidecar)");
            } else {
                std::string key4 = key;
                bool custom = false;
                auto it = km.find(key);
                if (it != km.end()) key4 = it->second;
                else custom = true;
                for (const auto& val : values) {
                    if (custom) {
                        std::vector<uint8_t> mean;
                        mean.insert(mean.end(), {0, 0, 0, 0});
                        mean.insert(mean.end(), {'m', 'e', 'a', 'n'});
                        std::string meanval = "com.apple.iTunes";
                        mean.insert(mean.end(), meanval.begin(), meanval.end());
                        wr32be_at(mean, 0, (uint32_t)mean.size());
                        std::vector<uint8_t> name;
                        name.insert(name.end(), {0, 0, 0, 0});
                        name.insert(name.end(), {'n', 'a', 'm', 'e'});
                        name.insert(name.end(), key.begin(), key.end());
                        wr32be_at(name, 0, (uint32_t)name.size());
                        std::vector<uint8_t> dbox;
                        dbox.insert(dbox.end(), {0, 0, 0, 0});
                        dbox.insert(dbox.end(), {'d', 'a', 't', 'a'});
                        wr32be(dbox, 1);
                        wr32be(dbox, 0);
                        dbox.insert(dbox.end(), val.begin(), val.end());
                        wr32be_at(dbox, 0, (uint32_t)dbox.size());
                        std::vector<uint8_t> item;
                        item.insert(item.end(), {0, 0, 0, 0});
                        item.insert(item.end(), {'-', '-', '-', '-'});
                        item.insert(item.end(), mean.begin(), mean.end());
                        item.insert(item.end(), name.begin(), name.end());
                        item.insert(item.end(), dbox.begin(), dbox.end());
                        wr32be_at(item, 0, (uint32_t)item.size());
                        new_ilst.insert(new_ilst.end(), item.begin(), item.end());
                    } else {
                        std::vector<uint8_t> v(val.begin(), val.end());
                        add_item(key4, 1, v);
                    }
                }
            }
        }
        for (const auto& pic : g.pictures) {
            uint32_t dataflags = pic.mime.find("png") != std::string::npos ? 14 : 13;
            add_item("covr", dataflags, pic.data);
        }

        std::vector<uint8_t> out;
        out.reserve(data.size() + new_ilst.size());
        out.insert(out.end(), data.begin(), data.end());
        out.erase(out.begin() + ilst.off, out.begin() + ilst.off + ilst.size);
        std::vector<uint8_t> new_ilst_box;
        new_ilst_box.insert(new_ilst_box.end(), {0, 0, 0, 0});
        new_ilst_box.insert(new_ilst_box.end(), {'i', 'l', 's', 't'});
        new_ilst_box.insert(new_ilst_box.end(), new_ilst.begin(), new_ilst.end());
        wr32be_at(new_ilst_box, 0, (uint32_t)new_ilst_box.size());
        out.insert(out.begin() + ilst.off, new_ilst_box.begin(), new_ilst_box.end());
        int64_t delta = (int64_t)new_ilst_box.size() - (int64_t)ilst.size;
        auto bump = [&](uint64_t off, int64_t add) {
            if (off + 4 > out.size()) return;
            uint32_t sz = rd32be(out.data() + off);
            if (sz != 1) {
                uint64_t ns = (uint64_t)sz + add;
                out[off] = (uint8_t)(ns >> 24);
                out[off + 1] = (uint8_t)(ns >> 16);
                out[off + 2] = (uint8_t)(ns >> 8);
                out[off + 3] = (uint8_t)ns;
            }
        };
        auto locate = [&](const char* t4) -> uint64_t {
            for (size_t i = moov_off; i + 4 <= out.size(); i++)
                if (memcmp(out.data() + i, t4, 4) == 0) return i;
            return 0;
        };
        uint64_t new_moov = locate("moov") - 4;
        uint64_t new_udta = locate("udta") - 4;
        uint64_t new_meta = locate("meta") - 4;
        bump(new_moov, delta);
        bump(new_udta, delta);
        bump(new_meta, delta);
        return util::write_file(path, out) ? "" : i18n::str("could not write M4A tags");
    }
    return i18n::fmt("format '%s' does not support built-in tags of type '%s'",
                     fmt.id.c_str(), tag_type_name(type));
}

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

// ---------------------------------------------------------------------------
// Валидация после записи
// ---------------------------------------------------------------------------

std::string validate_groups(const std::string& path, const config::Format& fmt,
                            const std::vector<std::pair<TagType, Group>>& embed,
                            const std::string& ffprobe) {
    media::Probe p;
    if (fmt.tag_validate_skip_ffprobe) {
        p.ok = true;
        p.format_name = fmt.id;
    } else {
        p = media::probe_file(path, ffprobe);
        if (!p.ok) return i18n::str("ffprobe could not read the written tags: ") + p.error;
    }
    TagSet read = extract_tags(path, p, fmt.tag_native_reader);
    std::vector<std::string> problems;
    size_t want_pics = 0;
    for (const auto& [type, g] : embed) {
        (void)type;
        want_pics += g.pictures.size();
        for (const auto& [key, values] : g.fields) {
            if (key == "lyrics" || is_replaygain(key)) continue;
            for (const auto& val : values) {
                bool found = false;
                bool numeric = std::find(fmt.tag_numeric_fields.begin(),
                                         fmt.tag_numeric_fields.end(), key) !=
                               fmt.tag_numeric_fields.end();
                for (const auto& [rk, rv] : read.fields) {
                    if (rk == key || norm_key(rk) == norm_key(key)) {
                        for (const auto& r : rv)
                            if (r == val || (numeric && norm_num(r) == norm_num(val))) { found = true; break; }
                    }
                }
                if (!found) {
                    problems.push_back(i18n::fmt("field '%s' did not survive ('%s')", key.c_str(), val.c_str()));
                }
            }
        }
        auto lyr = g.fields.find("lyrics");
        if (lyr != g.fields.end() && !lyr->second.empty()) {
            bool found = false;
            for (const auto& r : read.fields["lyrics"])
                if (r == lyr->second[0]) found = true;
            if (!found) problems.push_back(i18n::str("lyrics did not survive"));
        }
    }
    if (read.pictures.size() != want_pics) {
        problems.push_back(i18n::fmt("pictures: were %d, now %d", (int)want_pics,
                                      (int)read.pictures.size()));
    }
    std::string out;
    for (const auto& s : problems) out += s + "; ";
    if (!out.empty()) out.resize(out.size() - 2);
    return out;
}

}  // namespace tags
