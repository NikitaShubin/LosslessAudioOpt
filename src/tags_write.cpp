#include "tags_internal.h"

#include <cstring>

#include "i18n.h"
#include "util.h"

namespace tags {

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

}  // namespace tags