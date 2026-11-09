// LZX on streams made here, no game data. First the example of [MS-PATCH]
// section 3. Then a small encoder, written for this test from the same
// specification (a greedy match finder, Huffman codes from the counts, the
// trees coded against the previous block's), packs generated data into
// verbatim, aligned offset and uncompressed blocks that start and end inside
// 32 KB chunks; the decoder must give the data back, and the encoder counts
// that each feature was used (repeated offsets 0 to 2, 17-bit footers,
// aligned footers of 3 bits, 257-byte matches, the pretree's runs 17, 18 and
// 19). E8 translation: data with x86 calls, translated as the
// specification's compressor does, comes back as it was. Last, what must be
// refused: a cut stream, an unknown block type, a match from before the
// start or across a chunk, a bad window.

#include "check.h"
#include "xenoforge/lzx.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

using namespace xf;

namespace {

constexpr uint32_t CHUNK = 32768;
enum { VERBATIM = 1, ALIGNED = 2, UNCOMPRESSED = 3 };

struct Random {
    uint64_t state;
    uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(state >> 33);
    }
    uint32_t below(uint32_t n) { return next() % n; }
};

// 16-bit little-endian words, filled from their most significant bit
class BitWriter {
  public:
    void put(uint32_t value, int n) {
        for (int i = n - 1; i >= 0; i--) {
            word_ = uint16_t(word_ << 1 | (value >> i & 1));
            if (++used_ == 16) {
                out.push_back(uint8_t(word_));
                out.push_back(uint8_t(word_ >> 8));
                word_ = 0;
                used_ = 0;
            }
        }
    }
    void align() {
        if (used_) put(0, 16 - used_);
    }
    void pad_to_bytes() { put(0, used_ ? 16 - used_ : 16); } // 1 to 16 bits
    bool aligned() const { return used_ == 0; }
    std::vector<uint8_t> out;

  private:
    uint16_t word_ = 0;
    int used_ = 0;
};

// Huffman path lengths for the counts, flattened until none is longer than
// limit. Fewer than two elements counted get a second one, unless the tree
// may be empty and nothing was counted.
std::vector<uint8_t> path_lengths(std::vector<uint32_t> count, int limit, bool may_be_empty) {
    size_t used = size_t(std::count_if(count.begin(), count.end(), [](uint32_t c) { return c != 0; }));
    if (used == 0 && may_be_empty) return std::vector<uint8_t>(count.size(), 0);
    for (size_t i = 0; used < 2; i++)
        if (!count[i]) count[i] = 1, used++;
    for (;;) {
        std::vector<int> parent, leaf_node(count.size(), -1);
        using Item = std::pair<uint64_t, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        for (size_t i = 0; i < count.size(); i++)
            if (count[i]) {
                leaf_node[i] = int(parent.size());
                heap.push({count[i], int(parent.size())});
                parent.push_back(-1);
            }
        while (heap.size() > 1) {
            Item a = heap.top();
            heap.pop();
            Item b = heap.top();
            heap.pop();
            int node = int(parent.size());
            parent.push_back(-1);
            parent[size_t(a.second)] = parent[size_t(b.second)] = node;
            heap.push({a.first + b.first, node});
        }
        std::vector<uint8_t> lengths(count.size(), 0);
        int longest = 0;
        for (size_t i = 0; i < count.size(); i++) {
            if (leaf_node[i] < 0) continue;
            int depth = 0;
            for (int n = leaf_node[i]; parent[size_t(n)] >= 0; n = parent[size_t(n)]) depth++;
            lengths[i] = uint8_t(depth);
            longest = std::max(longest, depth);
        }
        if (longest <= limit) return lengths;
        for (uint32_t &c : count)
            if (c) c = c / 2 + 1;
    }
}

// Canonical codes: shorter first, lower elements first among equals
std::vector<uint32_t> canonical_codes(const std::vector<uint8_t> &lengths) {
    uint32_t count[17] = {}, next[17] = {};
    for (uint8_t l : lengths)
        if (l) count[l]++;
    uint32_t code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    std::vector<uint32_t> codes(lengths.size(), 0);
    for (size_t i = 0; i < lengths.size(); i++)
        if (lengths[i]) codes[i] = next[lengths[i]]++;
    return codes;
}

struct Token {
    uint32_t length = 0, offset = 0; // length 0: a literal
    uint8_t literal = 0;
};

struct Stats {
    int repeats[3] = {};
    int long_footers[4] = {}; // 17-bit footers, by block type
    int aligned_footers_3 = 0;
    int uneven_aligned_trees = 0; // not all 3 bits (else the same as verbatim bits)
    int longest_matches = 0;      // of 257 bytes
    int runs[3] = {};             // pretree codes 17, 18, 19
    int whole_word_pads = 0;      // uncompressed blocks whose header ended a word
};

class Encoder {
  public:
    Encoder(uint32_t window, bool e8 = false, uint32_t e8_size = 0) {
        uint32_t b = 0;
        for (int s = 0; s < 300; s++) {
            footer_[s] = s < 4 ? 0 : std::min((s - 2) / 2, 17);
            base_[s] = b;
            b += 1u << footer_[s];
        }
        while (base_[slots_] < window) slots_++;
        main_prev_.assign(size_t(256 + 8 * slots_), 0);
        length_prev_.assign(249, 0);
        w_.put(e8, 1);
        if (e8) {
            w_.put(e8_size >> 16, 16);
            w_.put(e8_size & 0xFFFF, 16);
        }
    }

    void compressed(int type, const std::vector<Token> &tokens) {
        struct Coded {
            int main, length = -1, aligned = -1;
            uint32_t verbatim = 0, size = 1;
            int verbatim_bits = 0;
        };
        std::vector<Coded> coded;
        std::vector<uint32_t> main_count(main_prev_.size()), length_count(249), aligned_count(8);
        uint32_t total = 0;
        for (const Token &t : tokens) {
            total += t.length ? t.length : 1;
            if (!t.length) {
                coded.push_back({t.literal});
                main_count[t.literal]++;
                continue;
            }
            uint32_t formatted;
            if (t.offset == r[0])
                formatted = 0;
            else if (t.offset == r[1])
                formatted = 1, std::swap(r[0], r[1]);
            else if (t.offset == r[2])
                formatted = 2, std::swap(r[0], r[2]);
            else {
                formatted = t.offset + 2;
                r[2] = r[1];
                r[1] = r[0];
                r[0] = t.offset;
            }
            int slot = 0;
            while (slot + 1 < slots_ && base_[slot + 1] <= formatted) slot++;
            int header = int(std::min<uint32_t>(t.length - 2, 7));
            Coded c{256 + slot * 8 + header};
            c.size = t.length;
            if (header == 7) c.length = int(t.length - 9);
            uint32_t footer = formatted - base_[slot];
            int bits = footer_[slot];
            if (type == ALIGNED && bits >= 3) {
                c.verbatim = footer >> 3;
                c.verbatim_bits = bits - 3;
                c.aligned = int(footer & 7);
                aligned_count[footer & 7]++;
                stats.aligned_footers_3 += bits == 3;
            } else {
                c.verbatim = footer;
                c.verbatim_bits = bits;
            }
            if (formatted < 3) stats.repeats[formatted]++;
            stats.long_footers[type] += bits == 17;
            stats.longest_matches += t.length == 257;
            main_count[size_t(c.main)]++;
            if (c.length >= 0) length_count[size_t(c.length)]++;
            coded.push_back(c);
        }

        auto main_lengths = path_lengths(main_count, 16, false);
        auto length_lengths = path_lengths(length_count, 16, true);
        w_.put(uint32_t(type), 3);
        w_.put(total >> 8, 16);
        w_.put(total & 0xFF, 8);
        std::vector<uint8_t> aligned_lengths;
        if (type == ALIGNED) {
            aligned_lengths = path_lengths(aligned_count, 7, false);
            for (uint8_t l : aligned_lengths) w_.put(l, 3);
            stats.uneven_aligned_trees +=
                std::any_of(aligned_lengths.begin(), aligned_lengths.end(), [](uint8_t l) { return l != 3; });
        }
        write_lengths(main_prev_.data(), main_lengths.data(), 0, 256);
        write_lengths(main_prev_.data(), main_lengths.data(), 256, int(main_lengths.size()));
        write_lengths(length_prev_.data(), length_lengths.data(), 0, 249);
        main_prev_ = main_lengths;
        length_prev_ = length_lengths;

        auto main_codes = canonical_codes(main_lengths), length_codes = canonical_codes(length_lengths);
        auto aligned_codes = canonical_codes(aligned_lengths);
        for (const Coded &c : coded) {
            w_.put(main_codes[size_t(c.main)], main_lengths[size_t(c.main)]);
            if (c.length >= 0) w_.put(length_codes[size_t(c.length)], length_lengths[size_t(c.length)]);
            w_.put(c.verbatim, c.verbatim_bits);
            if (c.aligned >= 0) w_.put(aligned_codes[size_t(c.aligned)], aligned_lengths[size_t(c.aligned)]);
            advance(c.size);
        }
    }

    void uncompressed(const uint8_t *data, uint32_t size, const uint32_t offsets[3]) {
        w_.put(UNCOMPRESSED, 3);
        w_.put(size >> 8, 16);
        w_.put(size & 0xFF, 8);
        stats.whole_word_pads += w_.aligned();
        w_.pad_to_bytes();
        for (int i = 0; i < 3; i++) {
            r[i] = offsets[i];
            for (int b = 0; b < 4; b++) w_.out.push_back(uint8_t(offsets[i] >> 8 * b));
        }
        w_.out.insert(w_.out.end(), data, data + size);
        if (size & 1) w_.out.push_back(0);
        pos += size;
    }

    std::vector<uint8_t> finish() {
        w_.align();
        return w_.out;
    }

    uint32_t r[3] = {1, 1, 1};
    uint64_t pos = 0;
    std::vector<size_t> chunk_ends; // the stream's size at the end of each full 32 KB chunk
    Stats stats;
    int slots() const { return slots_; }

  private:
    // each 32 KB of output ends the word
    void advance(uint32_t size) {
        pos += size;
        if (pos % CHUNK == 0) w_.align(), chunk_ends.push_back(w_.out.size());
    }

    // [first, last) of a tree's path lengths, against the previous block's
    void write_lengths(const uint8_t *prev, const uint8_t *now, int first, int last) {
        struct Item {
            int code;
            uint32_t extra;
            int extra_bits, second;
        };
        std::vector<Item> items;
        std::vector<uint32_t> count(20);
        for (int x = first; x < last;) {
            int zeros = 0, same = 0;
            while (x + zeros < last && now[x + zeros] == 0) zeros++;
            while (x + same < last && now[x + same] == now[x]) same++;
            if (zeros >= 20) {
                int n = std::min(zeros, 51);
                items.push_back({18, uint32_t(n - 20), 5, -1});
                stats.runs[1]++;
                x += n;
            } else if (zeros >= 4) {
                int n = std::min(zeros, 19);
                items.push_back({17, uint32_t(n - 4), 4, -1});
                stats.runs[0]++;
                x += n;
            } else if (same >= 4) {
                int n = std::min(same, 5);
                int code = (prev[x] + 17 - now[x]) % 17;
                items.push_back({19, uint32_t(n - 4), 1, code});
                count[size_t(code)]++;
                stats.runs[2]++;
                x += n;
            } else {
                items.push_back({(prev[x] + 17 - now[x]) % 17, 0, 0, -1});
                x++;
            }
            count[size_t(items.back().code)]++;
        }
        auto lengths = path_lengths(count, 15, false);
        auto codes = canonical_codes(lengths);
        for (uint8_t l : lengths) w_.put(l, 4);
        for (const Item &i : items) {
            w_.put(codes[size_t(i.code)], lengths[size_t(i.code)]);
            w_.put(i.extra, i.extra_bits);
            if (i.second >= 0) w_.put(codes[size_t(i.second)], lengths[size_t(i.second)]);
        }
    }

    BitWriter w_;
    uint32_t base_[300];
    int footer_[300];
    int slots_ = 0;
    std::vector<uint8_t> main_prev_, length_prev_;
};

// Greedy matches over d[start, end): the repeated offsets first, then the
// last 32 places with the same three bytes. Nothing crosses a chunk.
class MatchFinder {
  public:
    explicit MatchFinder(const std::vector<uint8_t> &d) : d_(d), head_(1 << 16, -1), prev_(d.size(), -1) {}

    std::vector<Token> find(size_t start, size_t end, uint32_t r[3], uint32_t window) {
        std::vector<Token> tokens;
        for (size_t p = start; p < end;) {
            uint32_t limit = uint32_t(std::min<size_t>({257, end - p, CHUNK - p % CHUNK}));
            uint32_t max_offset = uint32_t(std::min<size_t>(p, window - 3));
            auto length_at = [&](uint32_t offset) {
                uint32_t n = 0;
                while (n < limit && d_[p + n] == d_[p + n - offset]) n++;
                return n;
            };
            uint32_t best = 0, best_offset = 0;
            for (int k = 0; k < 3; k++)
                if (r[k] >= 1 && r[k] <= max_offset) {
                    uint32_t n = length_at(r[k]);
                    if (n > best) best = n, best_offset = r[k];
                }
            for (; indexed_ < p && indexed_ + 2 < d_.size(); indexed_++) {
                uint32_t h = hash(indexed_);
                prev_[indexed_] = head_[h];
                head_[h] = int64_t(indexed_);
            }
            if (p + 2 < d_.size()) {
                int64_t q = head_[hash(p)];
                for (int steps = 0; q >= 0 && steps < 32; q = prev_[size_t(q)], steps++) {
                    uint32_t offset = uint32_t(p - size_t(q));
                    if (offset > max_offset) break;
                    uint32_t n = length_at(offset);
                    if (n > best + 1) best = n, best_offset = offset;
                }
            }
            bool repeat = best_offset == r[0] || best_offset == r[1] || best_offset == r[2];
            if (best >= 3 || (best == 2 && repeat)) {
                tokens.push_back({best, best_offset});
                if (best_offset == r[1])
                    std::swap(r[0], r[1]);
                else if (best_offset == r[2])
                    std::swap(r[0], r[2]);
                else if (best_offset != r[0]) {
                    r[2] = r[1];
                    r[1] = r[0];
                    r[0] = best_offset;
                }
                p += best;
            } else {
                tokens.push_back({0, 0, d_[p]});
                p++;
            }
        }
        return tokens;
    }

  private:
    uint32_t hash(size_t p) const { return (uint32_t(d_[p]) << 8 ^ uint32_t(d_[p + 1]) << 4 ^ d_[p + 2]) & 0xFFFF; }

    const std::vector<uint8_t> &d_;
    std::vector<int64_t> head_, prev_;
    size_t indexed_ = 0;
};

// Data with literals (text-like and random), copies from a few recent
// offsets (so that they repeat and swap), far copies and long runs.
std::vector<uint8_t> make_data(size_t size, uint64_t seed) {
    Random rnd{seed};
    std::vector<uint8_t> d;
    uint32_t recent[3] = {7, 300, 5000};
    auto copy = [&](uint32_t offset, uint32_t length) {
        if (offset > d.size()) return;
        for (uint32_t i = 0; i < length; i++) d.push_back(d[d.size() - offset]);
    };
    while (d.size() < size) {
        switch (rnd.below(10)) {
        case 0:
        case 1:
        case 2: {
            bool text = rnd.below(2);
            for (uint32_t n = 1 + rnd.below(40); n--;)
                d.push_back(uint8_t(text ? 'a' + rnd.below(16) : rnd.below(256)));
            break;
        }
        case 3:
        case 4:
        case 5: copy(recent[rnd.below(3)], 2 + rnd.below(300)); break;
        case 6:
            if (d.size() > 16) {
                uint32_t offset = 1 + rnd.below(uint32_t(std::min<size_t>(d.size(), 400000)));
                if (offset >= 16) offset &= ~7u; // aligned: the low 3 bits skewed
                copy(offset, 3 + rnd.below(200));
            }
            break;
        case 7: d.insert(d.end(), 100 + rnd.below(600), uint8_t(rnd.below(256))); break;
        case 8:
            recent[2] = recent[1];
            recent[1] = recent[0];
            recent[0] = 1 + rnd.below(3000);
            if (recent[0] >= 16) recent[0] &= ~7u;
            break;
        default: std::swap(recent[0], recent[1 + rnd.below(2)]); break;
        }
    }
    d.resize(size);
    return d;
}

struct PlannedBlock {
    int type;
    uint32_t size;
};

// The data packed block by block as planned (the last block takes the rest)
std::vector<uint8_t> pack(const std::vector<uint8_t> &d, uint32_t window, const std::vector<PlannedBlock> &plan,
                          Stats &stats, bool e8 = false, uint32_t e8_size = 0, std::vector<size_t> *chunk_ends = nullptr) {
    Encoder encoder(window, e8, e8_size);
    MatchFinder finder(d);
    size_t at = 0;
    for (size_t i = 0; i < plan.size() && at < d.size(); i++) {
        size_t end = i + 1 == plan.size() ? d.size() : std::min(d.size(), at + plan[i].size);
        if (plan[i].type == UNCOMPRESSED) {
            const uint32_t offsets[3] = {encoder.r[2], encoder.r[0], encoder.r[1]};
            encoder.uncompressed(d.data() + at, uint32_t(end - at), offsets);
        } else {
            uint32_t r[3] = {encoder.r[0], encoder.r[1], encoder.r[2]};
            encoder.compressed(plan[i].type, finder.find(at, end, r, window));
        }
        at = end;
    }
    stats = encoder.stats;
    auto stream = encoder.finish();
    if (chunk_ends) *chunk_ends = encoder.chunk_ends;
    return stream;
}

bool unpacks(const std::vector<uint8_t> &stream, const std::vector<uint8_t> &expected, uint32_t window) {
    std::vector<uint8_t> out(expected.size(), 0xCC);
    std::string error;
    bool ok = lzx_decompress(stream, out, window, error);
    if (!ok) std::printf("  lzx_decompress: %s\n", error.c_str());
    return ok && out == expected;
}

void decodes_the_specification_example() {
    // "abc" in an uncompressed block (the chunk's size prefix left out)
    const std::vector<uint8_t> abc = {0x00, 0x30, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
                                      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x61, 0x62, 0x63, 0x00};
    CHECK(unpacks(abc, {'a', 'b', 'c'}, 1 << 15));
    // the padding byte after the odd block is only skipped when a block follows
    CHECK(unpacks({abc.begin(), abc.end() - 1}, {'a', 'b', 'c'}, 1 << 15));
}

void round_trips(uint32_t window, size_t size, uint64_t seed, bool far) {
    auto data = make_data(size, seed);
    const std::vector<PlannedBlock> plan = {
        {VERBATIM, 50000}, {ALIGNED, 70001},      {UNCOMPRESSED, 12345}, {VERBATIM, 32768}, {ALIGNED, 100000},
        {UNCOMPRESSED, 1}, {UNCOMPRESSED, 40000}, {VERBATIM, 60000},     {ALIGNED, 0},
    };
    Stats stats;
    auto stream = pack(data, window, plan, stats);
    CHECK(unpacks(stream, data, window));
    CHECK(stats.repeats[0] > 0 && stats.repeats[1] > 0 && stats.repeats[2] > 0);
    CHECK(stats.aligned_footers_3 > 0 && stats.uneven_aligned_trees > 0 && stats.longest_matches > 0);
    CHECK(stats.runs[0] > 0 && stats.runs[1] > 0 && stats.runs[2] > 0);
    if (far) CHECK(stats.long_footers[VERBATIM] > 0 && stats.long_footers[ALIGNED] > 0);

    // what must be refused: the stream cut short, or more output than it holds
    std::vector<uint8_t> cut(stream.begin(), stream.begin() + std::ptrdiff_t(stream.size() / 2));
    std::vector<uint8_t> out(data.size());
    std::string error;
    CHECK(!lzx_decompress(cut, out, window, error));
    out.resize(data.size() + 1000);
    CHECK(!lzx_decompress(stream, out, window, error));
}

// An uncompressed block whose header ends a word is padded with a whole
// word: the verbatim block before it varied until that happens
void pads_a_whole_word() {
    auto data = make_data(5000, 4);
    int whole = 0;
    for (uint32_t k = 1; k <= 64; k++) {
        Stats stats;
        auto stream = pack(data, 1 << 15, {{VERBATIM, k * 37}, {UNCOMPRESSED, 333}, {VERBATIM, 0}}, stats);
        CHECK(unpacks(stream, data, 1 << 15));
        whole += stats.whole_word_pads;
    }
    CHECK(whole > 0);
}

// The specification's compressor side of E8 translation, chunk by chunk
void translate_e8(std::vector<uint8_t> &d, int32_t file_size, int &translated) {
    for (size_t at = 0; at < d.size(); at += CHUNK) {
        uint32_t size = uint32_t(std::min<size_t>(CHUNK, d.size() - at));
        if (at >= 0x40000000 || size <= 10) continue;
        uint8_t *chunk = d.data() + at;
        for (uint32_t i = 0; i < size - 10; i++) {
            if (chunk[i] != 0xE8) continue;
            int32_t current = int32_t(at + i);
            int32_t displacement = int32_t(uint32_t(chunk[i + 1]) | uint32_t(chunk[i + 2]) << 8 |
                                           uint32_t(chunk[i + 3]) << 16 | uint32_t(chunk[i + 4]) << 24);
            int64_t target = int64_t(current) + displacement; // random bytes may be any displacement
            if (target >= 0 && target < int64_t(file_size) + current) {
                if (target >= file_size) target = displacement - file_size;
                for (int b = 0; b < 4; b++) chunk[i + 1 + b] = uint8_t(uint64_t(target) >> 8 * b);
                translated++;
            }
            i += 4;
        }
    }
}

void undoes_e8_translation() {
    const int32_t file_size = 200000;
    Random rnd{99};
    std::vector<uint8_t> original = make_data(3 * CHUNK + 1000, 7);
    // calls whose target is in the file, past its end, before its start
    for (int n = 0; n < 400; n++) {
        size_t at = rnd.below(uint32_t(original.size() - 5));
        int32_t current = int32_t(at), displacement;
        switch (n % 3) {
        case 0: displacement = int32_t(rnd.below(file_size)) - current; break;
        case 1: displacement = file_size + int32_t(rnd.below(uint32_t(current) + 1)) - current; break;
        default: displacement = -current - 1 - int32_t(rnd.below(1000)); break;
        }
        original[at] = 0xE8;
        for (int b = 0; b < 4; b++) original[at + 1 + b] = uint8_t(uint32_t(displacement) >> 8 * b);
    }
    original[CHUNK - 8] = 0xE8; // within the last 10 bytes of a chunk: left as is
    std::vector<uint8_t> translated = original;
    int count = 0;
    translate_e8(translated, file_size, count);
    CHECK(count > 100 && translated != original);
    Stats stats;
    auto stream = pack(translated, 1 << 16, {{VERBATIM, 40000}, {UNCOMPRESSED, 30001}, {ALIGNED, 0}}, stats, true,
                       uint32_t(file_size));
    CHECK(unpacks(stream, original, 1 << 16));
}

void refuses_bad_streams() {
    std::vector<uint8_t> out(100);
    std::string error;

    BitWriter unknown; // no E8, block type 5
    unknown.put(0, 1);
    unknown.put(5, 3);
    unknown.put(0, 16);
    unknown.put(100, 8);
    unknown.align();
    CHECK(!lzx_decompress(unknown.out, out, 1 << 15, error));

    Encoder before(1 << 15); // a match before anything was written
    before.compressed(VERBATIM, {Token{3, 5}});
    out.resize(3);
    CHECK(!lzx_decompress(before.finish(), out, 1 << 15, error));

    // a match across a chunk's end
    std::vector<Token> tokens(CHUNK - 2, Token{0, 0, 'x'});
    tokens.push_back({5, 1});
    Encoder across(1 << 15);
    across.compressed(VERBATIM, tokens);
    out.resize(CHUNK + 3);
    CHECK(!lzx_decompress(across.finish(), out, 1 << 15, error));

    // windows: not a power of two, too small, too large
    const std::vector<uint8_t> abc = {0x00, 0x30, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
                                      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x61, 0x62, 0x63, 0x00};
    out.resize(3);
    for (uint32_t window : {0x18000u, 0x4000u, 1u << 26}) CHECK(!lzx_decompress(abc, out, window, error));
}

// Reference data (LZX DELTA's): the target packed with the reference as the
// history its matches may reach into. The reference is a multiple of a chunk
// long, so that the chunks of the output line up with the match finder's.
void decodes_with_reference(uint32_t window, uint64_t seed) {
    auto base = make_data(3 * CHUNK, seed);
    auto edited = make_data(40000, seed + 100);
    std::vector<uint8_t> target(base.begin() + 1000, base.begin() + 1000 + 50000);
    std::copy(edited.begin(), edited.begin() + 3000, target.begin() + 7000);
    std::copy(edited.begin(), edited.begin() + 500, target.begin() + 33000);
    std::vector<uint8_t> all = base;
    all.insert(all.end(), target.begin(), target.end());

    Encoder encoder(window, false, 0);
    MatchFinder finder(all);
    size_t at = base.size();
    const int types[2] = {VERBATIM, ALIGNED};
    for (int i = 0; at < all.size(); i++) {
        size_t end = i == 1 ? all.size() : at + 21000;
        uint32_t r[3] = {encoder.r[0], encoder.r[1], encoder.r[2]};
        encoder.compressed(types[i], finder.find(at, end, r, window));
        at = end;
    }
    auto stream = encoder.finish();

    std::vector<uint8_t> out(target.size(), 0xCC);
    std::string error;
    CHECK(lzx_decompress_delta(stream, base, out, window, error));
    CHECK(out == target);
    // without its reference the stream reaches before its start
    CHECK(!lzx_decompress(stream, out, window, error));
    // a shorter reference (zeros before it) does not give the target back
    std::vector<uint8_t> small(base.end() - 1000, base.end());
    bool decoded = lzx_decompress_delta(stream, small, out, window, error);
    CHECK(!decoded || out != target);
}

// The stream a frame at a time (LzxStream), as the cabinet library hands it
// frames: the same bytes as the whole-stream decoder, in blocks that start and
// end inside frames, with E8 translation; a bad frame leaves the stream where
// it was; the window slides past its size.
void decodes_a_frame_at_a_time(uint32_t window, size_t size, uint64_t seed, bool e8) {
    auto original = make_data(size, seed);
    auto data = original;
    int translated = 0;
    const int32_t file_size = 1 << 24;
    if (e8) translate_e8(data, file_size, translated);
    Stats stats;
    std::vector<size_t> ends;
    auto stream = pack(data, window, {{VERBATIM, 50000}, {ALIGNED, 70001}, {VERBATIM, 30000}, {ALIGNED, 0}}, stats, e8,
                       uint32_t(file_size), &ends);
    ends.push_back(stream.size());
    LzxStream decoder;
    std::string error;
    CHECK(decoder.start(window, error));
    std::vector<uint8_t> out(size, 0xCC);
    size_t from = 0;
    for (size_t frame = 0, at = 0; at < size; frame++, at += CHUNK) {
        size_t n = std::min<size_t>(CHUNK, size - at), to = ends[std::min(frame, ends.size() - 1)];
        if (frame == 1) { // a frame cut short is refused and changes nothing
            std::vector<uint8_t> scratch(n);
            CHECK(!decoder.frame({stream.data() + from, 3}, scratch, error));
        }
        CHECK(decoder.frame({stream.data() + from, to - from}, {out.data() + at, n}, error));
        from = to;
    }
    CHECK(out == original);
    LzxStream bad;
    CHECK(!bad.start(1 << 14, error));
    std::vector<uint8_t> one(CHUNK + 1);
    CHECK(!decoder.frame({stream.data(), 4}, one, error)); // more than a frame
}

} // namespace

int main() {
    decodes_the_specification_example();
    round_trips(1 << 19, 450000, 1, true); // 38 position slots: 17-bit footers
    round_trips(1 << 15, 200000, 2, false);
    round_trips(1 << 21, 300000, 3, false); // 50 slots, the largest main tree
    round_trips(1u << 25, 300000, 4, false); // 290 slots: the largest window LZX DELTA has
    decodes_with_reference(1 << 17, 5);
    decodes_with_reference(1 << 16, 6);
    pads_a_whole_word();
    undoes_e8_translation();
    refuses_bad_streams();
    decodes_a_frame_at_a_time(1 << 17, 150000, 8, false);
    decodes_a_frame_at_a_time(1 << 15, 300000, 9, true); // the window slides
    return finish("lzx");
}
