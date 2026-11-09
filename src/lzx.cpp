// LZX, from [MS-PATCH] (sections 2.1 to 2.7). The bitstream is 16-bit
// little-endian words read from their most significant bit. A header bit
// says whether x86 call (E8) translation was applied, then blocks follow,
// each a 3-bit type and a 24-bit size in bytes of output:
// - uncompressed: the bitstream padded to a word (1 to 16 bits), the three
//   repeated offsets and the bytes themselves, then a byte of padding if the
//   size is odd;
// - verbatim: the main tree (literals, then matches: a position slot and a
//   length header) and the length tree, each as path lengths coded against
//   the previous block's through a pretree, then the tokens;
// - aligned offset: the same, after an 8-element tree that codes the low 3
//   bits of the larger offsets.
// Every 32 KB of output (a chunk) ends the bitstream's word, and no match
// crosses that boundary. Section 2.6.3 says which offsets use the aligned
// tree (footers of 3 bits or more); the pseudocode in 2.7 says "more than 3",
// which disagrees with it and with the cabinet format: 2.6.3 is followed.

#include "xenoforge/lzx.h"

#include "xenoforge/bytes.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace xf {
namespace {

constexpr uint32_t CHUNK = 32768;
constexpr int NUM_CHARS = 256;
constexpr int PRETREE_SIZE = 20;
constexpr int LENGTH_SIZE = 249; // length footers 0..248: matches of 9 to 257 bytes
constexpr int ALIGNED_SIZE = 8;
constexpr int MAX_SLOTS = 290; // a 32 MB window (LZX DELTA's largest; a cabinet's is 2 MB, 50)
constexpr int MAIN_MAX = NUM_CHARS + 8 * MAX_SLOTS;
// A run of path lengths (17, 18 or 19) may go up to 50 past the tree it
// codes: what it writes there stays, as the decoder Xenia uses keeps it.
constexpr int RUN_MARGIN = 51;
constexpr int MAX_CODE = 16;

enum BlockType { VERBATIM = 1, ALIGNED = 2, UNCOMPRESSED = 3 };

class BitReader {
  public:
    explicit BitReader(std::span<const uint8_t> in) : in_(in) {}

    // n <= 32 bits, zeros past the end (consuming them is an overrun)
    uint32_t peek(int n) {
        if (count_ < n) refill();
        return n ? uint32_t(buf_ >> (64 - n)) : 0;
    }
    void skip(int n) {
        if (count_ < n) refill();
        buf_ <<= n;
        count_ -= n;
    }
    uint32_t read(int n) {
        uint32_t v = peek(n);
        skip(n);
        return v;
    }
    // drops what is left of the current word (0 to 15 bits)
    void align() { skip(count_ % 16); }
    // before an uncompressed block's bytes: 1 to 16 bits (a whole word when
    // already at a word's start)
    void pad_to_bytes() { skip(count_ % 16 ? count_ % 16 : 16); }
    // where the bytes after the current word start (once aligned)
    size_t byte_position() const { return pos_ - size_t(count_ / 8); }
    void seek(size_t byte) {
        pos_ = byte;
        buf_ = 0;
        count_ = 0;
    }
    // more consumed than the stream holds, beyond one missing last word (its
    // bits read as zeros: Xenia's decoder allows that too)
    bool overrun() const { return pos_ * 8 - size_t(count_) > in_.size() * 8 + 16; }

  private:
    void refill() {
        while (count_ <= 48) {
            uint64_t word = 0;
            if (pos_ + 1 < in_.size())
                word = le16(in_.data() + pos_);
            else if (pos_ < in_.size())
                word = in_[pos_];
            buf_ |= word << (48 - count_);
            count_ += 16;
            pos_ += 2;
        }
    }

    std::span<const uint8_t> in_;
    size_t pos_ = 0;   // the next byte to load
    uint64_t buf_ = 0; // the bits loaded, from bit 63
    int count_ = 0;
};

// A canonical Huffman code from its path lengths: shorter codes first, and
// among codes of one length the lower element first (section 2.4).
class Tree {
  public:
    // A tree must be complete; only the length tree may be empty (then
    // nothing may be decoded with it).
    bool build(const uint8_t *lengths, int size, bool may_be_empty) {
        std::memset(count_, 0, sizeof(count_));
        for (int i = 0; i < size; i++) {
            if (lengths[i] > MAX_CODE) return false;
            count_[lengths[i]]++;
        }
        empty_ = count_[0] == size;
        if (empty_) return may_be_empty;
        int left = 1;
        for (int len = 1; len <= MAX_CODE; len++) {
            left = (left << 1) - count_[len];
            if (left < 0) return false; // over-subscribed
        }
        if (left != 0) return false; // incomplete
        uint16_t offset[MAX_CODE + 2] = {};
        for (int len = 1; len <= MAX_CODE; len++) offset[len + 1] = uint16_t(offset[len] + count_[len]);
        for (int i = 0; i < size; i++)
            if (lengths[i]) symbols_[offset[lengths[i]]++] = uint16_t(i);
        return true;
    }

    // -1 if the tree is empty
    int decode(BitReader &in) const {
        if (empty_) return -1;
        uint32_t bits = in.peek(MAX_CODE);
        uint32_t first = 0, index = 0;
        for (int len = 1; len <= MAX_CODE; len++) {
            uint32_t code = bits >> (MAX_CODE - len);
            if (code - first < count_[len]) {
                in.skip(len);
                return symbols_[index + code - first];
            }
            index += count_[len];
            first = (first + count_[len]) << 1;
        }
        return -1; // not reached for a complete tree
    }

  private:
    uint16_t count_[MAX_CODE + 1] = {};
    uint16_t symbols_[MAIN_MAX] = {};
    bool empty_ = true;
};

// Path lengths [first, last) of a tree, each coded against the previous
// block's through a pretree (section 2.5).
bool read_lengths(BitReader &in, uint8_t *lengths, int first, int last) {
    uint8_t pre_lengths[PRETREE_SIZE];
    for (uint8_t &l : pre_lengths) l = uint8_t(in.read(4));
    Tree pretree;
    if (!pretree.build(pre_lengths, PRETREE_SIZE, false)) return false;
    for (int x = first; x < last;) {
        int code = pretree.decode(in);
        if (code < 0) return false;
        if (code == 17 || code == 18) {
            int zeros = code == 17 ? 4 + int(in.read(4)) : 20 + int(in.read(5));
            while (zeros--) lengths[x++] = 0;
        } else if (code == 19) {
            int same = 4 + int(in.read(1));
            code = pretree.decode(in);
            if (code < 0 || code > 16) return false;
            uint8_t value = uint8_t((lengths[x] + 17 - code) % 17);
            while (same--) lengths[x++] = value;
        } else {
            lengths[x] = uint8_t((lengths[x] + 17 - code) % 17);
            x++;
        }
    }
    return true;
}

// The position slots' base and footer bits (section 2.6.2)
struct Slots {
    uint32_t base[MAX_SLOTS + 1];
    uint8_t footer[MAX_SLOTS + 1];
    Slots() {
        uint32_t b = 0;
        for (int s = 0; s <= MAX_SLOTS; s++) {
            footer[s] = uint8_t(s < 4 ? 0 : std::min((s - 2) / 2, 17));
            base[s] = b;
            b += 1u << footer[s];
        }
    }
};
const Slots SLOTS;

// Reverses the E8 translation of one chunk (section 2.2.2). The window keeps
// what was decoded; only the output is translated.
void undo_e8(uint8_t *chunk, uint32_t size, uint32_t chunk_offset, int32_t file_size) {
    if (chunk_offset >= 0x40000000 || size <= 10) return;
    for (uint32_t i = 0; i < size - 10; i++) {
        if (chunk[i] != 0xE8) continue;
        int32_t current = int32_t(chunk_offset + i);
        int32_t value = int32_t(le32(chunk + i + 1));
        if (value >= -current && value < file_size) {
            int32_t displacement = value >= 0 ? value - current : value + file_size;
            for (int b = 0; b < 4; b++) chunk[i + 1 + b] = uint8_t(uint32_t(displacement) >> (8 * b));
        }
        i += 4;
    }
}

} // namespace

namespace {

// What a decoder carries from one chunk to the next: the code lengths and
// trees of the block it is in, the repeated offsets, where the block ends.
struct DecoderState {
    uint32_t window_size = 0;
    int slots = 0, main_size = 0;
    uint8_t main_lengths[MAIN_MAX + RUN_MARGIN] = {};
    uint8_t length_lengths[LENGTH_SIZE + RUN_MARGIN] = {};
    uint8_t aligned_lengths[ALIGNED_SIZE] = {};
    Tree main_tree, length_tree, aligned_tree;
    uint32_t r[3] = {1, 1, 1};
    bool e8 = false, header_read = false;
    int32_t e8_size = 0;
    int block_type = 0;
    uint32_t block_size = 0, block_left = 0;
    size_t raw = 0; // in an uncompressed block: where its next byte is
};

bool start_state(DecoderState &s, uint32_t window_size, std::string &error) {
    if (window_size < (1u << 15) || window_size > (1u << 25) || (window_size & (window_size - 1))) {
        error = "the LZX window is not a power of two from 32 KB to 32 MB";
        return false;
    }
    s.window_size = window_size;
    while (SLOTS.base[s.slots] < window_size) s.slots++;
    s.main_size = NUM_CHARS + 8 * s.slots;
    return true;
}

// The stream's first bits: whether the E8 calls are translated, and the size
// the translation is for.
void read_header(DecoderState &s, BitReader &bits) {
    s.header_read = true;
    s.e8 = bits.read(1) != 0;
    s.e8_size = 0;
    if (s.e8) {
        uint32_t high = bits.read(16);
        s.e8_size = int32_t(high << 16 | bits.read(16));
    }
}

// The decoder over a flat buffer: out is the first output byte and `prefix`
// bytes before it are readable (reference data, or zeros), so a match may
// reach back into them. Decodes `size` bytes in chunks of 32 KB, the last
// one short, each chunk ending its word.
bool run(DecoderState &s, BitReader &bits, std::span<const uint8_t> in, uint8_t *out, size_t size, size_t prefix,
         std::string &error) {
    const uint32_t window_size = s.window_size;
    const int main_size = s.main_size;
    uint8_t *main_lengths = s.main_lengths, *length_lengths = s.length_lengths, *aligned_lengths = s.aligned_lengths;
    Tree &main_tree = s.main_tree, &length_tree = s.length_tree, &aligned_tree = s.aligned_tree;
    uint32_t *r = s.r;
    int &block_type = s.block_type;
    uint32_t &block_size = s.block_size, &block_left = s.block_left;
    size_t &raw = s.raw;
    for (size_t pos = 0; pos < size;) {
        const size_t chunk_end = std::min(size, pos + CHUNK);
        while (pos < chunk_end) {
            if (block_left == 0) {
                // an uncompressed block of an odd size is followed by a byte of padding
                if (block_type == UNCOMPRESSED && (block_size & 1)) bits.seek(raw + 1);
                block_type = int(bits.read(3));
                uint32_t high = bits.read(16);
                block_size = block_left = high << 8 | bits.read(8);
                if (block_size == 0) {
                    error = "an LZX block of no bytes";
                    return false;
                }
                if (block_type == ALIGNED) {
                    for (int i = 0; i < ALIGNED_SIZE; i++) aligned_lengths[i] = uint8_t(bits.read(3));
                    if (!aligned_tree.build(aligned_lengths, ALIGNED_SIZE, false)) {
                        error = "an LZX aligned offset tree is not a valid code";
                        return false;
                    }
                }
                if (block_type == VERBATIM || block_type == ALIGNED) {
                    if (!read_lengths(bits, main_lengths, 0, NUM_CHARS) ||
                        !read_lengths(bits, main_lengths, NUM_CHARS, main_size) ||
                        !main_tree.build(main_lengths, main_size, false) ||
                        !read_lengths(bits, length_lengths, 0, LENGTH_SIZE) ||
                        !length_tree.build(length_lengths, LENGTH_SIZE, true)) {
                        error = "an LZX block's trees are not valid codes";
                        return false;
                    }
                } else if (block_type == UNCOMPRESSED) {
                    bits.pad_to_bytes();
                    raw = bits.byte_position();
                    if (raw + 12 > in.size()) {
                        error = "an LZX uncompressed block runs past the stream";
                        return false;
                    }
                    for (int i = 0; i < 3; i++) r[i] = le32(in.data() + raw + 4 * i);
                    raw += 12;
                } else {
                    error = "an LZX block of an unknown type";
                    return false;
                }
            }

            const size_t run_end = pos + std::min<size_t>(block_left, chunk_end - pos);
            if (block_type == UNCOMPRESSED) {
                size_t n = run_end - pos;
                if (raw + n > in.size()) {
                    error = "an LZX uncompressed block runs past the stream";
                    return false;
                }
                std::memcpy(out + pos, in.data() + raw, n);
                raw += n;
                pos += n;
                block_left -= uint32_t(n);
                if (block_left == 0) bits.seek(raw);
                continue;
            }

            const size_t run_start = pos;
            while (pos < run_end) {
                int symbol = main_tree.decode(bits);
                if (symbol < 0) {
                    error = "an LZX code not in the main tree";
                    return false;
                }
                if (symbol < NUM_CHARS) {
                    out[pos++] = uint8_t(symbol);
                    continue;
                }
                symbol -= NUM_CHARS;
                uint32_t length = uint32_t(symbol & 7) + 2;
                if (length == 9) {
                    int footer = length_tree.decode(bits);
                    if (footer < 0) {
                        error = "an LZX length not in the length tree";
                        return false;
                    }
                    length += uint32_t(footer);
                }
                int slot = symbol >> 3;
                uint32_t offset;
                if (slot < 3) {
                    offset = r[slot];
                    r[slot] = r[0];
                    r[0] = offset;
                } else {
                    int footer = SLOTS.footer[slot];
                    uint32_t formatted = SLOTS.base[slot];
                    if (block_type == ALIGNED && footer >= 3) {
                        formatted += bits.read(footer - 3) << 3;
                        int low = aligned_tree.decode(bits);
                        if (low < 0) {
                            error = "an LZX code not in the aligned offset tree";
                            return false;
                        }
                        formatted += uint32_t(low);
                    } else {
                        formatted += bits.read(footer);
                    }
                    offset = formatted - 2;
                    r[2] = r[1];
                    r[1] = r[0];
                    r[0] = offset;
                }
                if (length > run_end - pos) {
                    error = "an LZX match runs past its block or chunk";
                    return false;
                }
                if (offset == 0 || offset > pos + prefix || offset > window_size) {
                    error = "an LZX match copies from before the stream or the window";
                    return false;
                }
                uint8_t *to = out + pos;
                const uint8_t *from = to - offset;
                for (uint32_t i = 0; i < length; i++) to[i] = from[i]; // may overlap
                pos += length;
            }
            block_left -= uint32_t(pos - run_start);
        }
        // each chunk ends its word (an uncompressed block is bytes, not bits)
        if (!(block_type == UNCOMPRESSED && block_left)) bits.align();
        if (bits.overrun()) {
            error = "the LZX stream ends early";
            return false;
        }
    }

    return true;
}

bool decode(std::span<const uint8_t> in, uint8_t *out, size_t size, size_t prefix, uint32_t window_size,
            std::string &error) {
    DecoderState s;
    if (!start_state(s, window_size, error)) return false;
    BitReader bits(in);
    read_header(s, bits);
    if (!run(s, bits, in, out, size, prefix, error)) return false;
    if (s.e8)
        for (size_t at = 0; at < size; at += CHUNK)
            undo_e8(out + at, uint32_t(std::min<size_t>(CHUNK, size - at)), uint32_t(at), s.e8_size);
    return true;
}

} // namespace

struct LzxStream::Impl {
    DecoderState state;
    std::vector<uint8_t> flat; // the window's last bytes, then room for a frame
    size_t filled = 0;         // bytes of flat in use
    uint64_t total = 0;        // bytes decoded since the start
};

LzxStream::LzxStream() : impl_(std::make_unique<Impl>()) {}
LzxStream::~LzxStream() = default;

bool LzxStream::start(uint32_t window_size, std::string &error) {
    impl_ = std::make_unique<Impl>();
    if (!start_state(impl_->state, window_size, error)) return false;
    impl_->flat.assign(size_t(window_size) + CHUNK, 0);
    return true;
}

bool LzxStream::frame(std::span<const uint8_t> in, std::span<uint8_t> out, std::string &error) {
    Impl &m = *impl_;
    const uint32_t window = m.state.window_size;
    if (!window) return error = "the LZX stream was not started", false;
    if (out.size() > CHUNK) return error = "an LZX frame is at most 32 KB", false;
    if (m.filled + out.size() > m.flat.size()) { // slide: keep the last window
        std::memmove(m.flat.data(), m.flat.data() + m.filled - window, window);
        m.filled = window;
    }
    BitReader bits(in);
    if (!m.state.header_read) read_header(m.state, bits);
    DecoderState saved = m.state; // a refused frame leaves the stream where it was
    if (!run(m.state, bits, in, m.flat.data() + m.filled, out.size(), m.filled, error)) {
        m.state = saved;
        return false;
    }
    if (m.state.e8) undo_e8(m.flat.data() + m.filled, uint32_t(out.size()), uint32_t(m.total), m.state.e8_size);
    std::memcpy(out.data(), m.flat.data() + m.filled, out.size());
    m.filled += out.size();
    m.total += out.size();
    return true;
}

bool lzx_decompress(std::span<const uint8_t> in, std::span<uint8_t> out, uint32_t window_size, std::string &error) {
    return decode(in, out.data(), out.size(), 0, window_size, error);
}

bool lzx_decompress_delta(std::span<const uint8_t> in, std::span<const uint8_t> reference, std::span<uint8_t> out,
                          uint32_t window_size, std::string &error) {
    // The window holds the reference at its end (zeros before it, if it is
    // shorter than the window) and the output follows: what a decoder with
    // reference data sees, as one flat buffer.
    const size_t kept = std::min<size_t>(reference.size(), window_size);
    std::vector<uint8_t> flat(size_t(window_size) + out.size());
    if (kept) std::memcpy(flat.data() + window_size - kept, reference.data() + reference.size() - kept, kept);
    if (!decode(in, flat.data() + window_size, out.size(), window_size, window_size, error)) return false;
    std::memcpy(out.data(), flat.data() + window_size, out.size());
    return true;
}

} // namespace xf
