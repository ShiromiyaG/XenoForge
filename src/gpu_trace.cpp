// Xenia's .xtr read. Translated from src/xenia/gpu/trace_protocol.h and
// trace_reader.cc (BSD; see LICENSE-xenia): a 48-byte header, then commands,
// each a little-endian struct followed by its data, snappy-compressed when
// the writer chose to.

#include "xenoforge/gpu_trace.h"

#include "xenoforge/bytes.h"
#include "xenoforge/snappy.h"

#include <fstream>

namespace xf {
namespace {

constexpr uint32_t FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = 48; // version, 40 characters of commit, title id
constexpr uint32_t ENCODING_NONE = 0, ENCODING_SNAPPY = 1;

bool decode(std::span<const uint8_t> encoded, uint32_t encoding, std::vector<uint8_t> &out) {
    if (encoding == ENCODING_NONE) {
        out.assign(encoded.begin(), encoded.end());
        return true;
    }
    return encoding == ENCODING_SNAPPY && snappy_decompress(encoded, out);
}

} // namespace

bool open_gpu_trace(const std::filesystem::path &path, GpuTrace &out, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    out = GpuTrace{};
    out.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.data.size() < HEADER_SIZE) {
        error = "too short for a trace";
        return false;
    }
    out.version = le32(out.data.data());
    out.commit.assign(reinterpret_cast<const char *>(out.data.data() + 4), 40);
    out.title_id = le32(out.data.data() + 44);
    if (out.version != FORMAT_VERSION) {
        error = "trace format version " + std::to_string(out.version) + ", not 1";
        return false;
    }
    return true;
}

bool walk_gpu_trace(const GpuTrace &trace, TraceVisitor &visitor, std::string &error) {
    const uint8_t *data = trace.data.data();
    const size_t size = trace.data.size();
    size_t at = HEADER_SIZE;
    std::vector<uint8_t> decoded;
    auto fail = [&](const char *what) {
        error = std::string(what) + " at offset " + std::to_string(at);
        return false;
    };
    auto have = [&](size_t bytes) { return bytes <= size - at; };

    while (at < size) {
        if (!have(4)) return fail("a command cut short");
        const uint8_t *p = data + at;
        auto command = TraceCommand(le32(p));
        switch (command) {
        case TraceCommand::PrimaryBufferStart:
        case TraceCommand::IndirectBufferStart: {
            if (!have(12)) return fail("a buffer start cut short");
            uint64_t length = 12 + uint64_t(le32(p + 8)) * 4;
            if (!have(length)) return fail("a buffer start's words run past the file");
            visitor.buffer(command, le32(p + 4));
            at += length;
            break;
        }
        case TraceCommand::PrimaryBufferEnd:
        case TraceCommand::IndirectBufferEnd:
            visitor.buffer(command, 0);
            at += 4;
            break;
        case TraceCommand::PacketStart: {
            if (!have(12)) return fail("a packet cut short");
            uint64_t words = le32(p + 8);
            if (!have(12 + words * 4)) return fail("a packet's words run past the file");
            visitor.packet(le32(p + 4), {p + 12, size_t(words * 4)});
            at += 12 + words * 4;
            break;
        }
        case TraceCommand::PacketEnd: at += 4; break;
        case TraceCommand::MemoryRead:
        case TraceCommand::MemoryWrite: {
            if (!have(20)) return fail("a memory command cut short");
            uint32_t address = le32(p + 4), encoding = le32(p + 8), encoded = le32(p + 12), length = le32(p + 16);
            if (!have(20 + uint64_t(encoded))) return fail("memory data runs past the file");
            if (!decode({p + 20, encoded}, encoding, decoded) || decoded.size() != length)
                return fail("memory data does not decode");
            if (command == TraceCommand::MemoryRead) visitor.memory_read(address, decoded);
            else visitor.memory_write(address, decoded);
            at += 20 + uint64_t(encoded);
            break;
        }
        case TraceCommand::EdramSnapshot: {
            if (!have(12)) return fail("an EDRAM snapshot cut short");
            uint32_t encoding = le32(p + 4), encoded = le32(p + 8);
            if (!have(12 + uint64_t(encoded))) return fail("an EDRAM snapshot runs past the file");
            if (!decode({p + 12, encoded}, encoding, decoded)) return fail("an EDRAM snapshot does not decode");
            visitor.edram_snapshot(decoded);
            at += 12 + uint64_t(encoded);
            break;
        }
        case TraceCommand::Event:
            if (!have(8)) return fail("an event cut short");
            visitor.event(le32(p + 4));
            at += 8;
            break;
        case TraceCommand::Registers: {
            // type, first, count, execute_callbacks (a bool, padded), encoding, length
            if (!have(24)) return fail("a registers command cut short");
            uint32_t first = le32(p + 4), count = le32(p + 8), encoding = le32(p + 16), encoded = le32(p + 20);
            if (!have(24 + uint64_t(encoded))) return fail("register values run past the file");
            if (!decode({p + 24, encoded}, encoding, decoded) || decoded.size() != uint64_t(count) * 4)
                return fail("register values do not decode");
            std::vector<uint32_t> values(count);
            for (uint32_t i = 0; i < count; i++) values[i] = le32(decoded.data() + 4 * i);
            visitor.registers(first, values);
            at += 24 + uint64_t(encoded);
            break;
        }
        case TraceCommand::GammaRamp: {
            // type, the read/write component (a byte, padded), encoding, length
            if (!have(16)) return fail("a gamma ramp cut short");
            uint32_t encoded = le32(p + 12);
            if (!have(16 + uint64_t(encoded))) return fail("a gamma ramp runs past the file");
            at += 16 + uint64_t(encoded);
            break;
        }
        default: return fail("an unknown command");
        }
    }
    return true;
}

} // namespace xf
