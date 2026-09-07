#include "tags_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

#include "config.h"
#include "i18n.h"
#include "media.h"
#include "util.h"

namespace tags {

// ---------------------------------------------------------------------------
// Канонические ключи
// ---------------------------------------------------------------------------

std::string canonical_key(const std::string& key) {
    std::string c;
    for (char ch : key) {
        if (ch == ' ' || ch == '_' || ch == '-') continue;
        c.push_back((char)::tolower((unsigned char)ch));
    }
    // Синонимы канонических ключей — data-driven (formats/tag_tables.json,
    // секция canonical_aliases): нормализованное имя -> канонический ключ.
    // Кеш load_tag_tables() безопасен (конфиги не меняются на лету).
    const auto& aliases = config::load_tag_tables().canonical_aliases;
    auto it = aliases.find(c);
    if (it != aliases.end()) return it->second;
    return key;  // произвольный ключ — как в источнике
}

bool is_replaygain(const std::string& k) {
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

uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint32_t rd32le(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t rd16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t syncsafe(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

void wr32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

// Запись 32-битного big-endian по смещению (в отличие от wr32be, которая
// добавляет в конец).
void wr32be_at(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = (uint8_t)(x >> 24);
    v[off + 1] = (uint8_t)(x >> 16);
    v[off + 2] = (uint8_t)(x >> 8);
    v[off + 3] = (uint8_t)x;
}
void wr32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 24));
}
void wr16be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}
void wr_syncsafe(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)((x >> 21) & 0x7f));
    v.push_back((uint8_t)((x >> 14) & 0x7f));
    v.push_back((uint8_t)((x >> 7) & 0x7f));
    v.push_back((uint8_t)(x & 0x7f));
}

// Добавление поля в группу (канонические ключи, дедупликация значений).
void g_put(Group& g, const std::string& key, const std::string& value) {
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
// Извлечение тегов: нативные разборы в отдельные группы
// ---------------------------------------------------------------------------

// Пересобирает каноническую агрегацию из групп и детектирует противоречия.
void rebuild_canonical(TagSet& ts) {
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