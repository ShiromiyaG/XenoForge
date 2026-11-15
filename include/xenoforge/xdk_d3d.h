// The guest's Direct3D as the XDK builds it (docs/sdk.md, "The XDK's
// Direct3D"): what a port capturing or replacing a game's D3D reads of the
// D3DDevice at a draw, by the XDK build the game's D3D library comes from.
// What the GPU would get for the draw is the device's shadow of the GPU
// registers, its float and fetch constants, and the bound shader objects
// (their microcode, and the literal constants the D3D loads with them), read
// on the game's render thread before the D3D's draw function.
//
//   const xf::xdk::D3DLayout *layout = xf::xdk::d3d_layout(21250); // the D3D9 library's build
//   xf::xdk::D3DReader reader(*layout);
//   // in a hook before D3DDevice_DrawIndexedVertices (dev in r3):
//   auto vs = reader.shader(c, reader.vertex_shader(c, dev), true, !reader.pixel_shader(c, dev));
//   reader.constants(c, dev, vs_floats, ps_floats);  // c0-c255 of each stage, the literals laid over
//   reader.fetch_constants(c, dev, fetch);           // 32 x 6 words, as the GPU gets them
//   reader.registers(c, dev, regs);                  // 0x2000-0x23FF, the blocks the shadow holds
//
// The D3D's own calls a port hooks, by their arguments (the same for every
// title of an XDK build; their addresses are each title's):
// - D3DDevice_DrawIndexedVertices(dev, primitive, start index, index count)
//   and DrawVertices(dev, primitive, start vertex, vertex count): from the
//   bound vertex buffers (the vertex fetch constants) and index buffer
//   (index_buffer).
// - D3DDevice_BeginVertices(dev, primitive, vertex count, stride): returns in
//   r3 the room in the ring buffer the game writes the vertices into right
//   away (no End call: the game moves the ring's write pointer itself);
//   vertex fetch constant 95 points at it for the draw.
// - D3DDevice_BeginIndexedVertices(dev, primitive, base vertex, vertex count,
//   index count, index format (6 32-bit, 1 16-bit), stride, where it writes
//   the indices' place (r10), and as a ninth argument (the caller's parameter
//   area, the word at r1 + 0x54) where it writes the vertices' place): returns
//   0 when it wrote them.
// - D3DDevice_ClearF(dev, flags (0xF the color targets, 0x10 depth, 0x20
//   stencil), rectangle or null, color (4 floats) or null, Z in f1, stencil):
//   the area is clear_area's.
// - D3DDevice_Resolve and D3DDevice_Swap: the GPU's resolve and swap packets.

#pragma once

#include "xenoforge/native_shaders.h"
#include "xenoforge/port.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace xf::xdk {

// Where the D3DDevice keeps what a draw reads (byte offsets in the device),
// and where a shader object keeps its microcode and literals.
struct D3DLayout {
    uint32_t xdk_build = 0; // the D3D library's build (its import library version's third number)
    uint32_t pixel_shader = 0, vertex_shader = 0; // the bound shader objects
    uint32_t fetch_constants = 0;                 // 32 fetch constants of 6 words (GPU 0x4800)
    uint32_t vs_constants = 0, ps_constants = 0;  // 256 float4 each (GPU 0x4000 and 0x4400)
    uint32_t index_buffer = 0;   // the bound index buffer object: bit 31 of its first word 32-bit indices, the data at +0x18
    uint32_t viewport = 0;       // x, y, width, height (floats)
    uint32_t scissor_enabled = 0, scissor = 0; // a word; x1, y1, x2, y2
    // the shadow's register blocks the GPU gets as they are: {first register, count, byte offset}
    struct Block {
        uint32_t first, count, offset;
    };
    Block blocks[8] = {};
    // A vertex shader object: its microcode's descriptor ({offset, bytes} from
    // the data at [object + vs_data]) at object + vs_header + [object +
    // vs_descriptor]; with no pixel shader bound and bit vs_depth_only_flag of
    // [object + vs_header], the depth-only variant's at object + vs_header +
    // [object + vs_depth_only]. A pixel shader's: object + ps_header + [object
    // + ps_descriptor], the data at [object + ps_data]. The literals: a block
    // at header + [header + 0x14] (data from the object's data), from block +
    // 0x14, [block + 0x10] bytes of {u16 destination: a float4 constant of the
    // GPU's 512 shifted left by 2, with the type in bits 16-23 of that, u16
    // dwords, u32 the data's offset}, ending at one of 0 dwords.
    uint32_t vs_header = 0, vs_descriptor = 0, vs_depth_only = 0, vs_depth_only_flag = 0, vs_data = 0;
    uint32_t ps_header = 0, ps_descriptor = 0, ps_data = 0;
};

// The layout of an XDK build's D3D (D3D9 and D3D9LTCG alike), null when the
// SDK does not know it. Known: 21250 (2.0.21250.0).
const D3DLayout *d3d_layout(uint32_t xdk_build);

// A shader object's microcode as the GPU runs it, and what it reads.
struct ShaderObject {
    uint64_t hash = 0;           // native_shaders.h's shader_hash
    std::vector<uint32_t> words; // host order
    native::ShaderUses uses;
    bool bad = false; // its descriptor did not look right
};

class D3DReader {
  public:
    explicit D3DReader(const D3DLayout &layout) : l_(layout) {}
    const D3DLayout &layout() const { return l_; }

    uint32_t vertex_shader(const port::Context &c, uint32_t dev) const {
        return port::load<uint32_t>(c, dev + l_.vertex_shader);
    }
    uint32_t pixel_shader(const port::Context &c, uint32_t dev) const {
        return port::load<uint32_t>(c, dev + l_.pixel_shader);
    }
    // The bound object's (null: none bound), kept by the object and its
    // microcode's place (per thread: the game's render thread). A vertex
    // shader drawn with no pixel shader bound runs its depth-only variant
    // when the object has one.
    std::shared_ptr<const ShaderObject> shader(const port::Context &c, uint32_t object, bool vertex,
                                               bool no_pixel_shader = false) const;
    // The GPU's float constants for a draw (c0-c255 of each stage, host
    // order): the shadow's, with the bound shaders' literal constants laid
    // over (the D3D has the GPU load them from the shader's own data as it
    // loads the shader, never through the shadow).
    void constants(const port::Context &c, uint32_t dev, uint32_t vs[1024], uint32_t ps[1024]) const;
    // The 32 fetch constants (192 words, host order) as the GPU gets them: a
    // slot whose type is 0 has the texture type set (the shadow keeps it 0).
    void fetch_constants(const port::Context &c, uint32_t dev, uint32_t out[192]) const;
    // The render state 0x2000-0x23FF (out[register - 0x2000]): the blocks the
    // shadow holds as the GPU gets them, the rest 0.
    void registers(const port::Context &c, uint32_t dev, uint32_t out[0x400]) const;
    // The bound index buffer: its data's address (0: none) and width.
    struct IndexBuffer {
        uint32_t address = 0;
        bool index32 = false;
    };
    IndexBuffer index_buffer(const port::Context &c, uint32_t dev) const;
    // A ClearF's area (x1, y1, x2, y2): its rectangle (null: the whole), within
    // the device's viewport and, when on, its scissor.
    void clear_area(const port::Context &c, uint32_t dev, uint32_t rect, int32_t out[4]) const;

  private:
    void literals(const port::Context &c, uint32_t object, bool vertex, uint32_t vs[1024], uint32_t ps[1024]) const;
    D3DLayout l_;
};

} // namespace xf::xdk
