#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "media.h"

namespace tags {

struct Picture {
    int type = 3;             // ID3 picture type (3 = front cover)
    std::string mime;         // "image/jpeg", "image/png", ...
    std::string description;
    std::vector<uint8_t> data;
};

// Типы тегов (форматы хранения в одном файле).
enum class TagType { id3v2, riff, vorbis, apev2, id3v1, mp4, unknown };

const char* tag_type_name(TagType t);
TagType tag_type_from_string(const std::string& s);

// Одна группа тегов одного формата. Ключи полей канонические (см. canonical_key).
struct Group {
    TagType type = TagType::unknown;
    std::map<std::string, std::vector<std::string>> fields;
    std::vector<Picture> pictures;
    std::string cue_sheet;
    bool empty() const;
};

// Канонический набор тегов файла: группы (raw-форматы) + каноническая агрегация.
struct TagSet {
    bool present = false;
    // Извлечены ли все теги (текст+картинки). false — контейнер/теги, которые мы
    // не умеем разбирать (например WebM/Matroska) → файл не заменяется.
    bool complete = true;
    // Противоречия между группами: каноническое поле/cue_sheet с ≥2 разными
    // значениями из ≥2 групп, либо слот картинки с разным содержимым.
    bool conflict = false;

    // Каноническая агрегация по всем группам (для валидации/отчётов).
    std::map<std::string, std::vector<std::string>> fields;
    std::vector<Picture> pictures;
    std::string cue_sheet;

    std::vector<Group> groups;
};

// Приводит ключ тега к каноническому виду.
std::string canonical_key(const std::string& key);

// Извлекает теги: группы каждого найденного формата (ID3v2, LIST INFO, Vorbis,
// APEv2, ID3v1, MP4) нативным разбором файла.
TagSet extract_tags(const std::string& path, const media::Probe& probe);

// Объединяет группы a и b (встроенные + sidecar). Одноимённые типы сливаются.
TagSet merge_tags(TagSet a, const TagSet& b);

// План: что встроить (тип + группа) и что вынести в sidecar.
// allow_merge=false — сжатие (мерж групп запрещён); true — восстановление.
struct TagPlan {
    std::vector<std::pair<TagType, Group>> embed;
    std::vector<Group> sidecar;
};

TagPlan plan_tags(const TagSet& ts, const std::vector<TagType>& target_types,
                  const std::map<std::string, bool>& tag_caps, bool allow_merge);

// Записывает одну группу встроенно в файл формата fmt_id (теги в типе type).
std::string write_group(const std::string& path, const std::string& fmt_id, TagType type,
                        const Group& g);

// Записывает ZIP-sidecar (tags.json + pictures/…) рядом с файлом:
// base_path + ".tags.zip". groups — только те группы, что вынесены наружу.
// Возвращает размер сайдкара (>0 при успехе).
uint64_t write_sidecar(const std::string& base_path, const std::vector<Group>& groups,
                       std::string* err);

// Читает ZIP-sidecar: base_path + ".tags.zip" (v2 — группы из write_sidecar,
// v1 — старый плоский формат, сворачивается в одну группу unknown).
// Возвращает false, если sidecar отсутствует или повреждён.
bool read_sidecar(const std::string& base_path, TagSet& ts, std::string* err);

// Сверка после записи: каждая записанная группа присутствует при перечитывании
// файла нашими парсерами. Возвращает пустую строку, если всё совпало.
std::string validate_groups(const std::string& path, const std::string& fmt_id,
                            const std::vector<std::pair<TagType, Group>>& embed,
                            const std::string& ffprobe);

}  // namespace tags
