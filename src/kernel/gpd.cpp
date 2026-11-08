/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// XDBF reading and GPD writing (gpd.h). Follows Xenia's xdbf_io.cc,
// gpd_info.cc and gpd_info_title.cc; differs in laying a file out afresh
// at each write (Xenia keeps a free list and reuses the holes), which any
// reader of the format handles.

#include "gpd.h"

#include <algorithm>
#include <cstring>

namespace xf::kernel {
namespace {

constexpr uint32_t XDBF_MAGIC = 0x58444246; // "XDBF"
constexpr uint32_t XDBF_VERSION = 0x10000;
constexpr uint32_t BASE_ENTRY_COUNT = 512;
constexpr size_t ACHIEVEMENT_HEADER = 0x1C;

uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t rd64(const uint8_t *p) { return uint64_t(rd32(p)) << 32 | rd32(p + 4); }
void wr16(std::vector<uint8_t> &v, size_t at, uint16_t x) { v[at] = uint8_t(x >> 8), v[at + 1] = uint8_t(x); }
void wr32(std::vector<uint8_t> &v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; i++) v[at + i] = uint8_t(x >> (24 - 8 * i));
}
void wr64(std::vector<uint8_t> &v, size_t at, uint64_t x) {
    wr32(v, at, uint32_t(x >> 32)), wr32(v, at + 4, uint32_t(x));
}

void put_u16(std::vector<uint8_t> &v, std::u16string_view s) {
    for (char16_t ch : s) v.push_back(uint8_t(ch >> 8)), v.push_back(uint8_t(ch));
    v.push_back(0), v.push_back(0);
}

// a terminated UTF-16 string at p, within end; p moves past it
std::u16string get_u16(const uint8_t *&p, const uint8_t *end) {
    std::u16string s;
    while (p + 2 <= end) {
        char16_t ch = char16_t(rd16(p));
        p += 2;
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

std::u16string utf8_to_u16(const uint8_t *p, size_t n) {
    std::u16string out;
    for (size_t i = 0; i < n;) {
        uint32_t ch = p[i++];
        int more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC0 ? 1 : 0;
        ch &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0xFF;
        while (more-- > 0 && i < n) ch = ch << 6 | (p[i++] & 0x3F);
        if (ch >= 0x10000) {
            ch -= 0x10000;
            out.push_back(char16_t(0xD800 + (ch >> 10))), out.push_back(char16_t(0xDC00 + (ch & 0x3FF)));
        } else {
            out.push_back(char16_t(ch));
        }
    }
    return out;
}

} // namespace

bool Gpd::parse(const uint8_t *data, size_t size) {
    if (size < 24 || rd32(data) != XDBF_MAGIC) return false;
    uint32_t entry_count = rd32(data + 8), entry_used = rd32(data + 12), free_count = rd32(data + 16);
    uint64_t table = 24, body = table + 18ull * entry_count + 8ull * free_count;
    if (body > size) return false;
    for (uint32_t i = 0; i < entry_used && i < entry_count; i++) {
        const uint8_t *e = data + table + 18ull * i;
        uint64_t offset = body + rd32(e + 10), length = rd32(e + 14);
        if (offset + length > size) continue; // a damaged entry is dropped, the rest kept
        entries[{rd16(e), rd64(e + 2)}].assign(data + offset, data + offset + length);
    }
    return true;
}

const std::vector<uint8_t> *Gpd::find(uint16_t section, uint64_t id) const {
    auto it = entries.find({section, id});
    return it == entries.end() ? nullptr : &it->second;
}

std::vector<uint8_t> Gpd::serialize() const {
    uint32_t used = uint32_t(entries.size());
    uint32_t entry_count = std::max(BASE_ENTRY_COUNT, (used + BASE_ENTRY_COUNT) / BASE_ENTRY_COUNT * BASE_ENTRY_COUNT);
    uint32_t free_count = BASE_ENTRY_COUNT;
    size_t data_size = 0;
    for (const auto &e : entries) data_size += e.second.size();
    size_t body = 24 + 18ull * entry_count + 8ull * free_count;
    std::vector<uint8_t> out(body + data_size);
    wr32(out, 0, XDBF_MAGIC), wr32(out, 4, XDBF_VERSION), wr32(out, 8, entry_count), wr32(out, 12, used);
    wr32(out, 16, free_count), wr32(out, 20, 1);
    size_t at = 24, offset = 0;
    for (const auto &[key, bytes] : entries) {
        wr16(out, at, key.first), wr64(out, at + 2, key.second);
        wr32(out, at + 10, uint32_t(offset)), wr32(out, at + 14, uint32_t(bytes.size()));
        std::memcpy(out.data() + body + offset, bytes.data(), bytes.size());
        offset += bytes.size(), at += 18;
    }
    // the one free entry: from the end of the data to the end of the offsets' range
    size_t free_at = 24 + 18ull * entry_count;
    wr32(out, free_at, uint32_t(offset)), wr32(out, free_at + 4, uint32_t(0xFFFFFFFFu - offset));
    return out;
}

std::vector<uint8_t> Achievement::encode() const {
    std::vector<uint8_t> v(ACHIEVEMENT_HEADER);
    wr32(v, 0, uint32_t(ACHIEVEMENT_HEADER)), wr32(v, 4, id), wr32(v, 8, image_id), wr32(v, 12, gamerscore);
    wr32(v, 16, flags), wr64(v, 20, unlock_time);
    put_u16(v, label), put_u16(v, description), put_u16(v, unachieved);
    return v;
}

bool Achievement::decode(const std::vector<uint8_t> &b) {
    if (b.size() < ACHIEVEMENT_HEADER) return false;
    id = rd32(&b[4]), image_id = rd32(&b[8]), gamerscore = rd32(&b[12]), flags = rd32(&b[16]);
    unlock_time = rd64(&b[20]);
    const uint8_t *p = b.data() + ACHIEVEMENT_HEADER, *end = b.data() + b.size();
    label = get_u16(p, end), description = get_u16(p, end), unachieved = get_u16(p, end);
    return true;
}

std::vector<Achievement> spa_achievements(const Gpd &spa, uint32_t language) {
    std::vector<Achievement> out;
    const std::vector<uint8_t> *table = spa.find(SPA_METADATA, SPA_ID_XACH);
    // XACH: magic, version, size, the count (16 bits); then 36-byte entries: id, label id,
    // description id, unachieved id (16 bits each), image id (32), gamerscore (16), 2 unknown, flags (32)
    if (!table || table->size() < 14 || rd32(table->data()) != 0x58414348) return out;
    // the strings: XSTC's default language, XSTR by language: magic, version, size, count (16 bits), then
    // each string's id, length and UTF-8 bytes
    uint32_t default_language = 1;
    if (const auto *xstc = spa.find(SPA_METADATA, SPA_ID_XSTC); xstc && xstc->size() >= 16)
        default_language = rd32(xstc->data() + 12);
    std::map<uint16_t, std::u16string> strings;
    for (uint32_t lang : {language, default_language, 1u}) {
        const auto *xstr = spa.find(SPA_STRINGS, lang);
        if (!xstr || xstr->size() < 14 || rd32(xstr->data()) != 0x58535452) continue;
        uint16_t count = rd16(xstr->data() + 12);
        const uint8_t *p = xstr->data() + 14, *end = xstr->data() + xstr->size();
        for (uint16_t i = 0; i < count && p + 4 <= end; i++) {
            uint16_t sid = rd16(p), n = rd16(p + 2);
            if (p + 4 + n > end) break;
            strings[sid] = utf8_to_u16(p + 4, n);
            p += 4 + n;
        }
        break;
    }
    auto str = [&](uint16_t sid) {
        auto it = strings.find(sid);
        return it == strings.end() ? std::u16string() : it->second;
    };
    uint16_t count = rd16(table->data() + 12);
    for (uint16_t i = 0; i < count && 14 + 36u * (i + 1) <= table->size(); i++) {
        const uint8_t *e = table->data() + 14 + 36u * i;
        Achievement a;
        a.id = rd16(e), a.label = str(rd16(e + 2)), a.description = str(rd16(e + 4)), a.unachieved = str(rd16(e + 6));
        a.image_id = rd32(e + 8), a.gamerscore = rd16(e + 12), a.flags = rd32(e + 16);
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<uint8_t> encode_setting(uint32_t id, uint8_t type, const uint8_t value[8],
                                    const std::vector<uint8_t> &data) {
    std::vector<uint8_t> v(0x18);
    wr32(v, 0, id), v[8] = type;
    std::memcpy(v.data() + 0x10, value, 8);
    v.insert(v.end(), data.begin(), data.end());
    return v;
}

bool decode_setting(const std::vector<uint8_t> &b, uint32_t &id, uint8_t &type, uint8_t value[8],
                    std::vector<uint8_t> &data) {
    if (b.size() < 0x18) return false;
    id = rd32(b.data()), type = b[8];
    std::memcpy(value, b.data() + 0x10, 8);
    data.assign(b.begin() + 0x18, b.end());
    return true;
}

} // namespace xf::kernel
