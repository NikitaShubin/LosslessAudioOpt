#pragma once
// Внутренний заголовок модуля тегов: общие хелперы и объявления функций,
// разделяемые между tags_core.cpp / tags_vorbis.cpp / tags_apev2.cpp /
// tags_id3.cpp / tags_mp4.cpp / tags_wav.cpp / tags_sidecar.cpp / tags_write.cpp.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tags.h"

namespace tags {

// Бинарные хелперы (read/write big/little-endian, syncsafe).
uint32_t rd32be(const uint8_t* p);
uint32_t rd32le(const uint8_t* p);
uint16_t rd16be(const uint8_t* p);
uint32_t syncsafe(const uint8_t* p);
void wr32be(std::vector<uint8_t>& v, uint32_t x);
void wr32be_at(std::vector<uint8_t>& v, size_t off, uint32_t x);
void wr32le(std::vector<uint8_t>& v, uint32_t x);
void wr16be(std::vector<uint8_t>& v, uint16_t x);
void wr_syncsafe(std::vector<uint8_t>& v, uint32_t x);

// Канонизация ключей/значений.
bool is_replaygain(const std::string& k);

// Добавление поля в группу (канонические ключи, дедупликация значений).
void g_put(Group& g, const std::string& key, const std::string& value);

// Разбор/построение FLAC/Vorbis comment (tags_vorbis.cpp).
void build_vorbis_comment(const Group& g, const std::map<std::string, std::string>& key_map,
                          std::vector<uint8_t>& out);
int64_t flac_metadata(const uint8_t* d, size_t n, Group& g);
void build_flac_picture(const Picture& pic, std::vector<uint8_t>& out);
void ogg_parse(const uint8_t* d, size_t n, Group& g);

// APEv2 (tags_apev2.cpp).
void build_apev2(const Group& g, const std::map<std::string, std::string>& key_map,
                 std::vector<uint8_t>& out);
bool apev2_parse(const uint8_t* d, size_t n, Group& g);

// ID3v2 / ID3v1 (tags_id3.cpp).
void build_id3v2(const Group& g, const std::map<std::string, std::string>& key_map,
                 std::vector<uint8_t>& out);
bool id3v2_parse(const uint8_t* d, size_t n, Group& g);
void id3v1_write(const Group& g, std::vector<uint8_t>& out);
void id3v1_parse(const uint8_t* d, size_t n, Group& g);

// MP4 ilst (tags_mp4.cpp).
struct M4aBox {
    uint64_t off = 0;
    uint64_t size = 0;
    uint8_t type[4] = {0, 0, 0, 0};
};
std::vector<M4aBox> m4a_children(const uint8_t* d, uint64_t start, uint64_t end);
void mp4_ilst_parse(const uint8_t* d, size_t n, Group& g);

// WAV: LIST INFO + встроенные ID3v2-блоки (tags_wav.cpp).
void wav_list_info(const uint8_t* d, size_t n, Group& g);
void wav_id3v2_blocks(const uint8_t* d, size_t n, Group& g);

// Пересобирает каноническую агрегацию из групп и детектирует противоречия.
void rebuild_canonical(TagSet& ts);

}  // namespace tags