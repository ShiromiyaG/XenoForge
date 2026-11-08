// Windows cursor files (cursor_file.h).

#include "cursor_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xf::ui {

namespace {

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// the DIB of one entry: BITMAPINFOHEADER (its height counts the mask too), the pixels, then the mask
bool read_dib(const uint8_t *dib, size_t size, CursorImage &out, std::string &error) {
    if (size < 40 || u32(dib) < 40) return error = "no bitmap header", false;
    const uint32_t header = u32(dib);
    const int32_t width = int32_t(u32(dib + 4)), double_height = int32_t(u32(dib + 8));
    const uint16_t bits = u16(dib + 14);
    const uint32_t compression = u32(dib + 16);
    if (width <= 0 || width > 256 || double_height == 0 || std::abs(double_height) / 2 > 256) return error = "bad size", false;
    if (compression != 0 && compression != 3) return error = "compressed bitmap", false;
    if (bits != 32 && bits != 24) return error = std::to_string(bits) + "-bit bitmap", false;
    const int height = std::abs(double_height) / 2;
    const bool bottom_up = double_height > 0;
    const size_t row = ((size_t(width) * bits + 31) / 32) * 4, mask_row = ((size_t(width) + 31) / 32) * 4;
    const size_t pixels_at = header + (compression == 3 && header == 40 ? 12 : 0);
    if (pixels_at + row * size_t(height) > size) return error = "truncated pixels", false;
    const uint8_t *pixels = dib + pixels_at;
    const uint8_t *mask = pixels + row * size_t(height);
    const bool has_mask = pixels_at + row * size_t(height) + mask_row * size_t(height) <= size;
    out.width = width, out.height = height;
    out.rgba.assign(size_t(width) * size_t(height) * 4, 0);
    bool any_alpha = false;
    for (int y = 0; y < height; y++) {
        const int src_y = bottom_up ? height - 1 - y : y;
        const uint8_t *src = pixels + row * size_t(src_y);
        for (int x = 0; x < width; x++) {
            uint8_t *dst = &out.rgba[(size_t(y) * size_t(width) + size_t(x)) * 4];
            const uint8_t *p = src + size_t(x) * (bits / 8);
            dst[0] = p[2], dst[1] = p[1], dst[2] = p[0];
            dst[3] = bits == 32 ? p[3] : 255;
            any_alpha |= bits == 32 && p[3] != 0;
        }
    }
    // an image without alpha (24 bits, or 32 with every alpha 0) takes its shape from the mask: a set bit is see-through
    if (!any_alpha)
        for (int y = 0; y < height; y++) {
            const uint8_t *m = mask + mask_row * size_t(bottom_up ? height - 1 - y : y);
            for (int x = 0; x < width; x++)
                out.rgba[(size_t(y) * size_t(width) + size_t(x)) * 4 + 3] = has_mask && (m[x / 8] >> (7 - x % 8) & 1) ? 0 : 255;
        }
    return true;
}

} // namespace

bool parse_cursor(const uint8_t *data, size_t size, CursorImage &out, std::string &error) {
    if (size < 6 || u16(data) != 0 || (u16(data + 2) != 1 && u16(data + 2) != 2)) return error = "not a cursor file", false;
    const bool is_cursor = u16(data + 2) == 2;
    const size_t count = u16(data + 4);
    if (count == 0 || size < 6 + 16 * count) return error = "no images", false;
    // the largest image that can be read; the reason the last one could not, if none can
    size_t best = 0;
    std::string why = "no image";
    CursorImage image;
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *e = data + 6 + 16 * i;
        const size_t bytes = u32(e + 8), at = u32(e + 12);
        if (at > size || bytes > size - at) {
            why = "an image outside the file";
            continue;
        }
        CursorImage candidate;
        if (bytes >= 8 && std::memcmp(data + at, "\x89PNG", 4) == 0) {
            why = "a PNG image";
            continue;
        }
        if (!read_dib(data + at, bytes, candidate, why)) continue;
        if (is_cursor) candidate.hot_x = u16(e + 4), candidate.hot_y = u16(e + 6);
        if (!found || size_t(candidate.width) * size_t(candidate.height) > best) {
            best = size_t(candidate.width) * size_t(candidate.height);
            image = std::move(candidate);
            found = true;
        }
    }
    if (!found) return error = why, false;
    image.hot_x = std::clamp(image.hot_x, 0, image.width - 1), image.hot_y = std::clamp(image.hot_y, 0, image.height - 1);
    out = std::move(image);
    return true;
}

bool parse_animated_cursor(const uint8_t *data, size_t size, AnimatedCursor &out, std::string &error) {
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "ACON", 4) != 0) return error = "not an animated cursor", false;
    AnimatedCursor result;
    uint32_t frames = 0, steps = 0, jiffies = 10, flags = 0;
    std::vector<uint32_t> rates, sequence;
    bool header = false;
    for (size_t p = 12; p + 8 <= size;) {
        const uint8_t *chunk = data + p;
        size_t n = u32(chunk + 4);
        if (n > size - p - 8) n = size - p - 8;
        const uint8_t *body = chunk + 8;
        if (std::memcmp(chunk, "anih", 4) == 0 && n >= 36) {
            frames = u32(body + 4), steps = u32(body + 8), jiffies = u32(body + 28), flags = u32(body + 32);
            header = true;
        } else if (std::memcmp(chunk, "rate", 4) == 0) {
            for (size_t i = 0; i + 4 <= n; i += 4) rates.push_back(u32(body + i));
        } else if (std::memcmp(chunk, "seq ", 4) == 0) {
            for (size_t i = 0; i + 4 <= n; i += 4) sequence.push_back(u32(body + i));
        } else if (std::memcmp(chunk, "LIST", 4) == 0 && n >= 4 && std::memcmp(body, "fram", 4) == 0) {
            for (size_t q = 4; q + 8 <= n;) {
                size_t m = u32(body + q + 4);
                if (m > n - q - 8) m = n - q - 8;
                if (std::memcmp(body + q, "icon", 4) == 0) {
                    CursorImage frame;
                    if (!parse_cursor(body + q + 8, m, frame, error)) return error = "a frame: " + error, false;
                    result.frames.push_back(std::move(frame));
                }
                q += 8 + m + (m & 1);
            }
        }
        p += 8 + n + (n & 1);
    }
    if (!header || result.frames.empty()) return error = "no frames", false;
    if (!(flags & 1)) return error = "frames that are not icons", false;
    (void)frames;
    const size_t count = steps && (flags & 2) && !sequence.empty() ? std::min<size_t>(steps, sequence.size()) : result.frames.size();
    for (size_t i = 0; i < count; i++) {
        const int frame = (flags & 2) && !sequence.empty() ? int(sequence[i]) : int(i);
        if (frame < 0 || size_t(frame) >= result.frames.size()) return error = "a step past the frames", false;
        result.steps.push_back(frame);
        const uint32_t rate = i < rates.size() ? rates[i] : jiffies;
        result.duration_ms.push_back(std::max(int(rate * 1000 / 60), 1));
    }
    for (const CursorImage &f : result.frames)
        if (f.width != result.frames[0].width || f.height != result.frames[0].height) return error = "frames of different sizes", false;
    out = std::move(result);
    return true;
}

CursorImage scaled_cursor(const CursorImage &image, int factor) {
    if (factor <= 1 || image.width <= 0) return image;
    CursorImage out;
    out.width = image.width * factor, out.height = image.height * factor;
    out.hot_x = image.hot_x * factor + factor / 2, out.hot_y = image.hot_y * factor + factor / 2;
    out.rgba.resize(size_t(out.width) * size_t(out.height) * 4);
    for (int y = 0; y < out.height; y++)
        for (int x = 0; x < out.width; x++) {
            // sample centres, so the picture does not shift
            const float sx = (float(x) + 0.5f) / float(factor) - 0.5f, sy = (float(y) + 0.5f) / float(factor) - 0.5f;
            const int x0 = std::clamp(int(std::floor(sx)), 0, image.width - 1), y0 = std::clamp(int(std::floor(sy)), 0, image.height - 1);
            const int x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
            const float fx = std::clamp(sx - float(x0), 0.f, 1.f), fy = std::clamp(sy - float(y0), 0.f, 1.f);
            auto at = [&](int px, int py, int c) { return float(image.rgba[(size_t(py) * size_t(image.width) + size_t(px)) * 4 + size_t(c)]); };
            // colour weighted by alpha, so a clear pixel's colour does not bleed into its neighbours
            float alpha = 0, rgb[3] = {0, 0, 0};
            const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
            const int px[4] = {x0, x1, x0, x1}, py[4] = {y0, y0, y1, y1};
            for (int k = 0; k < 4; k++) {
                const float a = at(px[k], py[k], 3) * w[k];
                alpha += a;
                for (int c = 0; c < 3; c++) rgb[c] += at(px[k], py[k], c) * a;
            }
            uint8_t *dst = &out.rgba[(size_t(y) * size_t(out.width) + size_t(x)) * 4];
            for (int c = 0; c < 3; c++) dst[c] = alpha > 0 ? uint8_t(std::clamp(rgb[c] / alpha + 0.5f, 0.f, 255.f)) : 0;
            dst[3] = uint8_t(std::clamp(alpha + 0.5f, 0.f, 255.f));
        }
    return out;
}

} // namespace xf::ui
