// Library functions by signature (recomp.h; docs/recompiler.md, "The XDK by
// signature"): a reference executable's named functions found in another.

#include "xenoforge/recomp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace xf::recomp {
namespace {

// FNV-1a over 32-bit words
uint64_t hash_words(const std::vector<uint32_t> &w) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t x : w)
        for (int k = 0; k < 4; k++) h = (h ^ ((x >> (24 - 8 * k)) & 0xFF)) * 0x100000001B3ull;
    return h;
}

// D-form instructions whose 16-bit immediate is an offset from rA
bool d_form(uint32_t op) {
    return op == 14 || op == 24 || op == 25 || op == 26 || op == 27 || op == 28 || op == 29 ||
           (op >= 32 && op <= 55) || op == 58 || op == 62;
}

// opcodes whose kind is told by an extended opcode in bits 21-30
bool extended(uint32_t op) { return op == 31 || op == 63 || op == 59 || op == 19 || op == 30 || op == 4; }

double cosine(const std::map<uint32_t, uint32_t> &a, const std::map<uint32_t, uint32_t> &b) {
    double dot = 0, na = 0, nb = 0;
    for (const auto &[k, v] : a) {
        na += double(v) * v;
        if (auto it = b.find(k); it != b.end()) dot += double(v) * it->second;
    }
    for (const auto &[k, v] : b) nb += double(v) * v;
    return na && nb ? dot / std::sqrt(na * nb) : 0.0;
}

double jaccard(const std::set<uint32_t> &a, const std::set<uint32_t> &b) {
    if (a.empty() && b.empty()) return 1.0;
    size_t both = 0;
    for (uint32_t x : a) both += b.count(x);
    return double(both) / double(a.size() + b.size() - both);
}

} // namespace

Signature signature_of(const Image &image, uint32_t address, uint32_t size) {
    Signature s;
    std::vector<uint32_t> masked;
    uint32_t lis_regs = 0; // registers holding an address's upper half (lis), as a mask
    for (uint32_t at = address; at + 4 <= address + size; at += 4) {
        uint32_t x = image.word(at);
        const uint32_t op = x >> 26, rt = (x >> 21) & 31, ra = (x >> 16) & 31, imm = x & 0xFFFF;
        s.length++;
        uint32_t kind = extended(op) ? (op | ((x >> 1) & 0x3FF) << 6) : op;
        if (op == 18) { // b, bl: a target outside the function is the linker's
            int32_t li = int32_t(x & 0x03FFFFFC);
            if (li & 0x02000000) li -= 0x04000000;
            const uint32_t target = (x & 2) ? uint32_t(li) : at + uint32_t(li);
            if (target < address || target >= address + size) {
                x &= 0xFC000003;
                s.calls += x & 1;
            }
        } else if (op == 15 && ra == 0 && imm >= 0x8000 && imm < 0x9000) { // lis of an address's upper half
            x &= 0xFFFF0000;
            lis_regs |= 1u << rt;
        } else if (d_form(op) && ra != 0 && ((lis_regs >> ra) & 1)) { // the lower half, through it
            x &= 0xFFFF0000;
            if (op == 14 || op == 24 || op == 25) lis_regs &= ~(1u << rt);
        } else {
            if (op == 15 && ra == 0) s.constants.insert(1u << 16 | imm);
            if (op == 14 && ra == 0) s.constants.insert(2u << 16 | imm);
            if (op == 24 || op == 25) s.constants.insert(3u << 16 | imm);
            if (op == 14 || op == 15) lis_regs &= ~(1u << rt);
        }
        s.kinds[kind]++;
        masked.push_back(x);
    }
    s.hash = hash_words(masked);
    return s;
}

std::string write_signatures(const std::vector<Signature> &signatures) {
    std::string out = "# name\tlength\tcalls\thash\tkinds (kind:count)\tconstants (kind << 16 | immediate)\n";
    char buf[64];
    for (const Signature &s : signatures) {
        std::snprintf(buf, sizeof(buf), "\t%u\t%u\t%016llX\t", s.length, s.calls, (unsigned long long)s.hash);
        out += s.name + buf;
        bool first = true;
        for (const auto &[k, v] : s.kinds) {
            std::snprintf(buf, sizeof(buf), "%s%X:%u", first ? "" : ",", k, v);
            out += buf, first = false;
        }
        out += '\t';
        first = true;
        for (uint32_t c : s.constants) {
            std::snprintf(buf, sizeof(buf), "%s%X", first ? "" : ",", c);
            out += buf, first = false;
        }
        out += '\n';
    }
    return out;
}

bool read_signatures(std::string_view text, std::vector<Signature> &out, std::string &error) {
    std::istringstream in{std::string(text)};
    std::string line;
    size_t number = 0;
    while (std::getline(in, line)) {
        number++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        for (size_t at = 0;;) {
            const size_t tab = line.find('\t', at);
            f.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        if (f.size() != 6) return error = "line " + std::to_string(number) + ": not six fields", false;
        Signature s;
        s.name = f[0];
        s.length = uint32_t(std::strtoul(f[1].c_str(), nullptr, 10));
        s.calls = uint32_t(std::strtoul(f[2].c_str(), nullptr, 10));
        s.hash = std::strtoull(f[3].c_str(), nullptr, 16);
        std::istringstream kinds(f[4]);
        for (std::string item; std::getline(kinds, item, ',');) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            s.kinds[uint32_t(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16))] =
                uint32_t(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 10));
        }
        std::istringstream constants(f[5]);
        for (std::string item; std::getline(constants, item, ',');)
            if (!item.empty()) s.constants.insert(uint32_t(std::strtoul(item.c_str(), nullptr, 16)));
        out.push_back(std::move(s));
    }
    return true;
}

double signature_similarity(const Signature &a, const Signature &b) {
    if (a.hash == b.hash && a.length == b.length) return 1.0;
    if (!a.length || !b.length) return 0.0;
    const double lengths = double(std::min(a.length, b.length)) / double(std::max(a.length, b.length));
    double s = 0.45 * cosine(a.kinds, b.kinds) + 0.35 * jaccard(a.constants, b.constants) + 0.2 * lengths;
    const uint32_t calls_apart = a.calls > b.calls ? a.calls - b.calls : b.calls - a.calls;
    if (calls_apart > std::max<uint32_t>(2, a.calls / 3)) s -= 0.1;
    return std::min(s, 0.999);
}

std::vector<SignatureMatch> identify(const Image &image, const Discovery &d, const std::vector<Signature> &reference) {
    // every function of some length (not an import's thunk nor a register save helper)
    struct Candidate {
        uint32_t address;
        Signature signature;
    };
    std::vector<Candidate> all;
    std::map<uint64_t, std::vector<size_t>> by_hash;
    for (const auto &[a, fn] : d.functions) {
        if (fn.size < 8 || fn.source == Source::Import || fn.source == Source::Helper) continue;
        all.push_back({a, signature_of(image, a, fn.size)});
        by_hash[all.back().signature.hash].push_back(all.size() - 1);
    }
    std::vector<SignatureMatch> out(reference.size());
    std::vector<bool> taken(all.size(), false);
    // the same masked words, one function with them: the library's place
    uint32_t low = UINT32_MAX, high = 0;
    for (size_t r = 0; r < reference.size(); r++) {
        out[r].name = reference[r].name;
        auto it = by_hash.find(reference[r].hash);
        if (it == by_hash.end() || it->second.size() != 1) continue;
        const Candidate &c = all[it->second[0]];
        if (c.signature.length != reference[r].length || taken[it->second[0]]) continue;
        taken[it->second[0]] = true;
        out[r].address = c.address, out[r].score = 1.0, out[r].exact = out[r].sure = true;
        low = std::min(low, c.address), high = std::max(high, c.address);
    }
    // Another build matches no words: its place is where the clearest feature matches gather (the largest
    // run of them less than 256 KB apart), each the best by a margin over the next.
    if (high < low || high == 0) {
        std::vector<uint32_t> clear;
        for (size_t r = 0; r < reference.size(); r++) {
            if (!reference[r].length) continue;
            double top = 0, next = 0;
            uint32_t at = 0;
            for (const Candidate &c : all) {
                const double ratio = double(c.signature.length) / double(reference[r].length);
                if (ratio < 0.8 || ratio > 1.25) continue;
                const double s = signature_similarity(reference[r], c.signature);
                if (s > top) next = top, top = s, at = c.address;
                else if (s > next) next = s;
            }
            if (top >= 0.97 && top - next >= 0.05) clear.push_back(at);
        }
        std::sort(clear.begin(), clear.end());
        size_t best_first = 0, best_count = 0;
        for (size_t i = 0; i < clear.size();) {
            size_t j = i + 1;
            while (j < clear.size() && clear[j] - clear[j - 1] < 0x40000) j++;
            if (j - i > best_count) best_first = i, best_count = j - i;
            i = j;
        }
        if (best_count >= 2) low = clear[best_first], high = clear[best_first + best_count - 1];
    }
    // the rest by their features, inside that place (with a margin: a library's functions lie together),
    // each function given to the reference name that matches it best
    const bool placed = high >= low && high != 0;
    const uint32_t from = placed && low > 0x40000 ? low - 0x40000 : 0, to = placed ? high + 0x40000 : UINT32_MAX;
    struct Pair {
        double score;
        size_t r, c;
    };
    std::vector<Pair> pairs;
    std::vector<double> best(reference.size(), 0.0), second(reference.size(), 0.0);
    for (size_t r = 0; r < reference.size(); r++) {
        if (out[r].exact || !reference[r].length) continue;
        for (size_t c = 0; c < all.size(); c++) {
            if (taken[c] || all[c].address < from || all[c].address > to) continue;
            const double ratio = double(all[c].signature.length) / double(reference[r].length);
            if (ratio < 0.4 || ratio > 2.5) continue;
            const double s = signature_similarity(reference[r], all[c].signature);
            if (s > best[r]) second[r] = best[r], best[r] = s;
            else if (s > second[r]) second[r] = s;
            if (s >= 0.8) pairs.push_back({s, r, c});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) { return a.score > b.score; });
    for (const Pair &p : pairs) {
        if (out[p.r].address || taken[p.c]) continue;
        taken[p.c] = true;
        SignatureMatch &m = out[p.r];
        m.address = all[p.c].address, m.score = p.score;
        m.runner_up = p.score == best[p.r] ? second[p.r] : best[p.r];
        m.sure = m.score >= 0.95 && m.score - m.runner_up >= 0.03;
    }
    return out;
}

} // namespace xf::recomp
