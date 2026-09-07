#include "tags_internal.h"

#include <cctype>
#include <cstring>

#include "config.h"
#include "util.h"

namespace tags {

// ---------------------------------------------------------------------------
// ID3v2
// ---------------------------------------------------------------------------

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

void build_id3v2(const Group& g, const std::map<std::string, std::string>& key_map,
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

// ID3 frame id -> canonical key (из formats/tag_tables.json, не хардкод).
static std::string id3_key(const std::string& id) {
    const auto& m = config::load_tag_tables().id3;
    auto it = m.find(id);
    return it != m.end() ? it->second : std::string();
}

bool id3v2_parse(const uint8_t* d, size_t n, Group& g) {
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

void id3v1_write(const Group& g, std::vector<uint8_t>& out) {
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

void id3v1_parse(const uint8_t* d, size_t n, Group& g) {
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

}  // namespace tags