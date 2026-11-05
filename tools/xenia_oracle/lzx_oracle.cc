// lzx_oracle: the image of an executable packed with normal (LZX)
// compression as Xenia unpacks it, for comparing with what
// `xfinspect --dump-image` writes. The session key and the decryption as in
// Xenia's XexModule::ReadImage (its AES, third_party/crypto), the block chain
// checked and joined as in XexModule::ReadImageCompressed, and the stream
// decompressed by Xenia's own lzx_decompress (libmspack), into the pages'
// size (XexModule::image_size()). The XEX code here is copied from
// src/xenia/cpu/xex_module.cc (BSD, see LICENSE-xenia). Built by build.bat,
// not part of XenoForge's build.
//
// usage: lzx_oracle <file.xex> <image.bin>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/util/xex2_info.h"

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/crypto/rijndael-alg-fst.c"
#include "third_party/crypto/rijndael-alg-fst.h"

// src/xenia/cpu/lzx.cc, in xenia-cpu.lib
int lzx_decompress(const void *lzx_data, size_t lzx_len, void *dest, size_t dest_len, uint32_t window_size,
                   void *window_data, size_t window_data_len);

static constexpr uint8_t xe_xex2_retail_key[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                                   0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
static constexpr uint8_t xe_xex2_devkit_key[16] = {};

static void aes_decrypt_buffer(const uint8_t *session_key, const uint8_t *input_buffer, const size_t input_size,
                               uint8_t *output_buffer, const size_t output_size) {
    uint32_t rk[4 * (MAXNR + 1)];
    uint8_t ivec[16] = {0};
    int32_t Nr = rijndaelKeySetupDec(rk, session_key, 128);
    const uint8_t *ct = input_buffer;
    uint8_t *pt = output_buffer;
    for (size_t n = 0; n < input_size; n += 16, ct += 16, pt += 16) {
        rijndaelDecrypt(rk, Nr, ct, pt);
        for (size_t i = 0; i < 16; i++) {
            pt[i] ^= ivec[i];
            ivec[i] = ct[i];
        }
    }
}

// The page size of the heap an executable loads into (Memory::Initialize)
static uint32_t heap_page_size(uint32_t address) {
    if (address >= 0x90000000 && address < 0xA0000000) return 4096;
    if (address >= 0x40000000 && address < 0xA0000000) return 64 * 1024;
    return 4096;
}

static const void *opt_header(const xe::xex2_header *header, uint32_t key) {
    for (uint32_t i = 0; i < header->header_count; i++)
        if (header->headers[i].key == key) {
            if ((key & 0xFF) == 0x00 || (key & 0xFF) == 0x01) return &header->headers[i].value;
            return reinterpret_cast<const uint8_t *>(header) + header->headers[i].offset;
        }
    return nullptr;
}

// ReadImageCompressed with the key given; 0 when the image starts with "MZ"
static int read_image(const std::vector<uint8_t> &xex, const uint8_t *key, std::vector<uint8_t> &image) {
    auto *header = reinterpret_cast<const xe::xex2_header *>(xex.data());
    auto *security = reinterpret_cast<const xe::xex2_security_info *>(xex.data() + header->security_offset);
    auto *format = static_cast<const xe::xex2_opt_file_format_info *>(opt_header(header, 0x000003FF));
    if (!format || format->compression_type != xe::XEX_COMPRESSION_NORMAL) return 10;

    uint8_t session_key[16];
    aes_decrypt_buffer(key, reinterpret_cast<const uint8_t *>(security->aes_key), 16, session_key, 16);

    const uint32_t exe_length = static_cast<uint32_t>(xex.size() - header->header_size);
    const uint8_t *exe_buffer = xex.data() + header->header_size;
    std::vector<uint8_t> input(exe_buffer, exe_buffer + exe_length);
    if (format->encryption_type == xe::XEX_ENCRYPTION_NORMAL)
        aes_decrypt_buffer(session_key, exe_buffer, exe_length, input.data(), exe_length);

    const auto *compression_info = &format->compression_info;
    const xe::xex2_compressed_block_info *cur_block = &compression_info->normal.first_block;
    std::vector<uint8_t> compress_buffer(exe_length);
    const uint8_t *p = input.data();
    uint8_t *d = compress_buffer.data();
    sha1::SHA1 s;
    uint8_t block_calced_digest[0x14];
    while (cur_block->block_size) {
        const uint8_t *pnext = p + cur_block->block_size;
        const auto *next_block = (const xe::xex2_compressed_block_info *)p;
        s.reset();
        s.processBytes(p, cur_block->block_size);
        s.finalize(block_calced_digest);
        if (memcmp(block_calced_digest, cur_block->block_hash, 0x14) != 0) return 2;
        p += 4;
        p += 20;
        while (true) {
            const size_t chunk_size = (p[0] << 8) | p[1];
            p += 2;
            if (!chunk_size) break;
            memcpy(d, p, chunk_size);
            p += chunk_size;
            d += chunk_size;
        }
        p = pnext;
        cur_block = next_block;
    }

    uint32_t base_address = security->load_address;
    if (auto *base = static_cast<const xe::be<uint32_t> *>(opt_header(header, 0x00010201))) base_address = *base;
    uint32_t uncompressed_size = 0;
    for (uint32_t i = 0; i < security->page_descriptor_count; i++) {
        uint32_t value = security->page_descriptors[i].value;
        uncompressed_size += (value >> 4) * heap_page_size(base_address);
    }
    image.assign(uncompressed_size, 0);
    int result = lzx_decompress(compress_buffer.data(), d - compress_buffer.data(), image.data(), uncompressed_size,
                                compression_info->normal.window_size, nullptr, 0);
    if (result) return result;
    return image.size() >= 2 && image[0] == 'M' && image[1] == 'Z' ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: lzx_oracle <file.xex> <image.bin>\n");
        return 2;
    }
    std::vector<uint8_t> xex;
    if (FILE *f = std::fopen(argv[1], "rb")) {
        uint8_t buffer[65536];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) xex.insert(xex.end(), buffer, buffer + n);
        std::fclose(f);
    }
    if (xex.size() < 0x18 || std::memcmp(xex.data(), "XEX2", 4) != 0) {
        std::fprintf(stderr, "%s: not an XEX2 executable\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> image;
    int result = read_image(xex, xe_xex2_retail_key, image);
    const char *key = "retail";
    if (result) {
        result = read_image(xex, xe_xex2_devkit_key, image);
        key = "devkit";
    }
    if (result) {
        std::fprintf(stderr, "%s: Xenia could not unpack it (%d)\n", argv[1], result);
        return 1;
    }
    FILE *out = std::fopen(argv[2], "wb");
    if (!out || std::fwrite(image.data(), 1, image.size(), out) != image.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::fclose(out);
    std::printf("%s: 0x%zX bytes (%s key)\n", argv[2], image.size(), key);
    return 0;
}
