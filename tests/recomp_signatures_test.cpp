// Library functions by signature on small images assembled here, no game
// data: the same function linked elsewhere (other call targets, other
// addresses built by lis and read through them) keeps its hash; another
// constant changes it but keeps it alike; the signature file reads back as
// written; identify names the reference's functions in an image where they
// lie at other addresses among decoys, exactly where the words match and by
// their features where another build changed them.

#include "check.h"
#include "xenoforge/recomp.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace xf;
using namespace xf::recomp;

namespace {

constexpr uint32_t T = 0x82000000;

uint32_t addi(uint32_t d, uint32_t a, int32_t v) { return 14u << 26 | d << 21 | a << 16 | (uint32_t(v) & 0xFFFF); }
uint32_t li(uint32_t d, int32_t v) { return addi(d, 0, v); }
uint32_t lis(uint32_t d, uint32_t v) { return 15u << 26 | d << 21 | (v & 0xFFFF); }
uint32_t ori(uint32_t a, uint32_t s, uint32_t v) { return 24u << 26 | s << 21 | a << 16 | (v & 0xFFFF); }
uint32_t lwz(uint32_t d, int32_t off, uint32_t a) { return 32u << 26 | d << 21 | a << 16 | (uint32_t(off) & 0xFFFF); }
uint32_t stw(uint32_t s, int32_t off, uint32_t a) { return 36u << 26 | s << 21 | a << 16 | (uint32_t(off) & 0xFFFF); }
uint32_t add(uint32_t d, uint32_t a, uint32_t b) { return 31u << 26 | d << 21 | a << 16 | b << 11 | 266u << 1; }
uint32_t b(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC); }
uint32_t bl(uint32_t from, uint32_t to) { return b(from, to) | 1; }
constexpr uint32_t BLR = 0x4E800020;

struct Builder {
    Image image;
    Discovery d;
    Builder() {
        image.base = T;
        image.bytes.assign(0x4000, 0);
        image.sections.push_back({".text", T, 0x4000, true, false});
    }
    void put(uint32_t at, uint32_t w) {
        const uint32_t o = at - T;
        image.bytes[o] = uint8_t(w >> 24), image.bytes[o + 1] = uint8_t(w >> 16);
        image.bytes[o + 2] = uint8_t(w >> 8), image.bytes[o + 3] = uint8_t(w);
    }
    // a function: its words, a discovered function over them
    void function(uint32_t at, const std::vector<uint32_t> &words) {
        for (size_t k = 0; k < words.size(); k++) put(at + 4 * uint32_t(k), words[k]);
        Function f;
        f.address = at, f.size = uint32_t(words.size()) * 4, f.source = Source::Pdata;
        d.functions[at] = f;
    }
};

// A device setter as a library links it: a global read through lis, a register number and a packet header
// loaded, a call out, fields of the device written. `global` is the global's address, `callee` where the
// call goes, `reg` the register number it writes, `field` the device field's offset.
std::vector<uint32_t> setter(uint32_t at, uint32_t global, uint32_t callee, int32_t reg, int32_t field) {
    return {lis(11, global >> 16),
            lwz(10, int32_t(global & 0xFFFF), 11),
            li(4, reg),
            lis(5, 0xC000),
            ori(5, 5, 0x2D00),
            stw(4, field, 3),
            stw(5, field + 4, 3),
            add(6, 4, 5),
            bl(at + 32, callee),
            lwz(3, 8, 10),
            BLR};
}

} // namespace

int main() {
    // the same function linked twice: other places, other call targets, other globals
    {
        Builder bld;
        bld.function(T + 0x100, setter(T + 0x100, 0x82100040, T + 0x800, 0x2200, 0x30));
        bld.function(T + 0x200, setter(T + 0x200, 0x82340980, T + 0x900, 0x2200, 0x30));
        bld.function(T + 0x300, setter(T + 0x300, 0x82100040, T + 0x800, 0x2201, 0x30));
        const Signature a = signature_of(bld.image, T + 0x100, 44), b2 = signature_of(bld.image, T + 0x200, 44);
        const Signature c = signature_of(bld.image, T + 0x300, 44);
        CHECK(a.length == 11 && a.calls == 1);
        CHECK(a.hash == b2.hash);
        CHECK(a.hash != c.hash);
        CHECK(signature_similarity(a, b2) == 1.0);
        const double near = signature_similarity(a, c);
        CHECK(near > 0.7 && near < 1.0);
        CHECK(a.constants.count(2u << 16 | 0x2200) && a.constants.count(1u << 16 | 0xC000) &&
              a.constants.count(3u << 16 | 0x2D00));
        CHECK(!a.constants.count(1u << 16 | 0x8210)); // an address's upper half is not a constant

        // written and read back
        std::vector<Signature> in = {a, c}, back;
        in[0].name = "Lib_SetA", in[1].name = "Lib_SetB";
        std::string error;
        CHECK(read_signatures(write_signatures(in), back, error));
        CHECK(back.size() == 2 && back[0].name == "Lib_SetA" && back[0].hash == a.hash &&
              back[0].kinds == a.kinds && back[0].constants == a.constants && back[1].length == c.length);
    }

    // identify: the reference's two setters among decoys, one linked as it was, one from another build
    // (its device field moved, an instruction more)
    {
        Builder ref;
        ref.function(T + 0x100, setter(T + 0x100, 0x82100040, T + 0x800, 0x2200, 0x30));
        std::vector<uint32_t> other = setter(T + 0x200, 0x82100040, T + 0x800, 0x2104, 0x40);
        other.insert(other.end() - 1, li(7, 0x10));
        ref.function(T + 0x200, other);
        std::vector<Signature> reference = {signature_of(ref.image, T + 0x100, 44),
                                            signature_of(ref.image, T + 0x200, uint32_t(other.size()) * 4)};
        reference[0].name = "Lib_SetA", reference[1].name = "Lib_SetB";

        Builder game;
        // decoys: unrelated code
        game.function(T + 0x1000, {li(3, 1), li(4, 2), add(3, 3, 4), add(3, 3, 3), BLR});
        game.function(T + 0x1100, {lwz(3, 0, 3), lwz(4, 4, 3), add(3, 3, 4), stw(3, 8, 5), BLR});
        // the setters elsewhere: A as linked, B with its field at 0x48 (another build's device)
        game.function(T + 0x2000, setter(T + 0x2000, 0x82500010, T + 0x3000, 0x2200, 0x30));
        std::vector<uint32_t> moved = setter(T + 0x2100, 0x82500010, T + 0x3000, 0x2104, 0x48);
        moved.insert(moved.end() - 1, li(7, 0x10));
        game.function(T + 0x2100, moved);
        const std::vector<SignatureMatch> found = identify(game.image, game.d, reference);
        CHECK(found.size() == 2);
        CHECK(found[0].name == "Lib_SetA" && found[0].address == T + 0x2000 && found[0].exact && found[0].sure);
        CHECK(found[1].name == "Lib_SetB" && found[1].address == T + 0x2100 && !found[1].exact);
        CHECK(found[1].score >= 0.9);
    }
    return finish("recomp_signatures");
}
