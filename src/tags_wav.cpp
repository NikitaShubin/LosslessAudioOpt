#include "tags_internal.h"

#include <cstring>

#include "config.h"

namespace tags {

// ---------------------------------------------------------------------------
// WAV: LIST INFO + встроенные ID3v2-блоки
// ---------------------------------------------------------------------------

static const std::string* wav_info_key(const char* id) {
    // Имена в точности как отдаёт ffprobe для WAV (LIST INFO). Таблица —
    // из formats/tag_tables.json (data-driven).
    const auto& tbl = config::load_tag_tables().wav;
    auto it = tbl.find(std::string(id, 4));
    return it != tbl.end() ? &it->second : nullptr;
}

// LIST-чанки с INFO-подчанками (RIFF-структура WAV).
void wav_list_info(const uint8_t* d, size_t n, Group& g) {
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
                    const std::string* key = wav_info_key((const char*)sub);
                    if (key) {
                        std::string val((const char*)d + data, subsz);
                        size_t z = val.find('\0');
                        if (z != std::string::npos) val.resize(z);
                        g_put(g, *key, val);
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
void wav_id3v2_blocks(const uint8_t* d, size_t n, Group& g) {
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

}  // namespace tags