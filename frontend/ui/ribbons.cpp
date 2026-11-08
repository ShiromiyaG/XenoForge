#include "ribbons.h"

#include <algorithm>
#include <cmath>

namespace xf::ui {

namespace {

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-12f ? a * (1.f / l) : V3{0, 0, 1};
}

constexpr float PI = 3.14159265f;

struct Spec {
    int tone;        // 0 dark grey, 1 light grey, 2.. greens, darkest to lightest
    float half;      // half width, in window heights
    float base;      // where, in aspects from the left
    float lean;      // how far right its top is from its bottom
    float amp;       // how far it sways
    float radius;    // the spiral it winds round
    float turns;     // how many times along the window
    float z;         // how far toward the viewer it sits
    float speed;     // the spiral's turning, rad/s
    float phase;
    float twist;     // the twist's start
};

// two greys at the left, four greens at the right. They all sweep the same way (the top leaning to the
// right, a gentle bow through the middle) and differ a little in lean, sway and phase; the spiral they wind
// round is small and their twist slight, so they flow side by side and show a fold only here and there
constexpr Spec SPECS[6] = {
    {0, 0.100f, 0.05f, 0.30f, 0.030f, 0.020f, 0.6f, -0.05f, 0.10f, 0.4f, 0.20f},
    {1, 0.034f, 0.13f, 0.28f, 0.028f, 0.025f, 0.7f, 0.10f, -0.12f, 2.1f, 0.50f},
    {2, 0.085f, 0.90f, 0.42f, 0.040f, 0.022f, 0.6f, -0.10f, 0.09f, 3.3f, 0.00f},
    {3, 0.065f, 0.97f, 0.40f, 0.035f, 0.026f, 0.7f, 0.00f, -0.11f, 5.0f, 0.40f},
    {4, 0.080f, 0.82f, 0.46f, 0.045f, 0.028f, 0.5f, 0.08f, 0.08f, 0.9f, -0.30f},
    {5, 0.050f, 0.94f, 0.37f, 0.030f, 0.024f, 0.8f, 0.18f, -0.10f, 4.2f, 0.60f},
};

V3 tone_color(int tone, const float tint[3]) {
    switch (tone) {
    case 0: return {0.427f, 0.427f, 0.435f}; // #6d6d6f
    case 1: return {0.667f, 0.667f, 0.667f}; // #aaaaaa
    case 2: return {tint[0] * 0.55f, tint[1] * 0.55f, tint[2] * 0.55f};
    case 3: return {tint[0] * 0.80f, tint[1] * 0.80f, tint[2] * 0.80f};
    case 4: return {tint[0], tint[1], tint[2]};
    default: return {tint[0] + (1 - tint[0]) * 0.35f, tint[1] + (1 - tint[1]) * 0.35f, tint[2] + (1 - tint[2]) * 0.35f};
    }
}

// the centre line of a ribbon at s (0 at the top of the strip, 1 at the bottom)
V3 centre(const Spec &r, float s, float t, float aspect) {
    const float y = -0.2f + 1.4f * s;
    const float phi = 2 * PI * r.turns * s + r.speed * t + r.phase;
    V3 c;
    const float bow = 0.20f * (y - 0.5f) * (y - 0.5f); // the common curve: they all bend the same way
    c.x = aspect * r.base + r.lean * (0.5f - y) - bow + r.amp * std::sin(2.1f * y + 0.11f * t + r.phase) + r.radius * std::sin(phi);
    c.y = y;
    c.z = r.z + r.radius * 1.8f * std::cos(phi);
    return c;
}

} // namespace

void build_ribbons(float time, uint32_t width, uint32_t height, const float tint[3], float alpha,
                   std::vector<RibbonVertex> &vertices, std::vector<uint32_t> &body, std::vector<uint32_t> &fringe) {
    vertices.clear();
    body.clear();
    fringe.clear();
    if (alpha <= 0 || width == 0 || height == 0) return;
    const float H = float(height), aspect = float(width) / H;
    constexpr int N = 150;                       // sections along a ribbon
    constexpr int NV = 9;                        // across the surface
    constexpr int M = NV + 2;                    // and the two fringes
    constexpr float VS[NV] = {-1.f, -0.75f, -0.5f, -0.25f, 0.f, 0.25f, 0.5f, 0.75f, 1.f};
    constexpr float CAMERA = 2.2f;               // the viewer's distance, in window heights
    const V3 light = norm({-0.35f, -0.55f, 0.75f});
    const V3 view{0, 0, 1};
    const V3 half_vec = norm(light + view);
    constexpr float Z_SCALE = 0.8f;              // depth in 0..1 from the space's z (toward the viewer)

    for (const Spec &r : SPECS) {
        const V3 hue = tone_color(r.tone, tint);
        const uint32_t first = uint32_t(vertices.size());
        for (int i = 0; i <= N; i++) {
            const float s = float(i) / float(N);
            const V3 c = centre(r, s, time, aspect);
            // the frame: the tangent, and the width's direction turned about it
            const float ds = 1.f / float(N) * 0.5f;
            const V3 T = norm(centre(r, std::min(s + ds, 1.5f), time, aspect) - centre(r, std::max(s - ds, -0.5f), time, aspect));
            const V3 n0 = norm(V3{1, 0, 0} - T * T.x);
            const V3 m0 = cross(T, n0);
            const float phi = 2 * PI * r.turns * s + r.speed * time + r.phase;
            const float theta = r.twist + 0.30f * std::sin(0.8f * phi) + 0.25f * std::sin(3.1f * s + 0.13f * time + r.phase); // a slight twist
            const V3 B = n0 * std::cos(theta) + m0 * std::sin(theta);
            V3 face = norm(cross(T, B));
            const bool back = dot(face, view) < 0;
            if (back) face = face * -1.f;

            float sx[NV], sy[NV], sz[NV];
            for (int k = 0; k < NV; k++) {
                const V3 p = c + B * (VS[k] * r.half);
                const float scale = CAMERA / (CAMERA - p.z);
                sx[k] = (aspect * 0.5f + (p.x - aspect * 0.5f) * scale) * H;
                sy[k] = (0.5f + (p.y - 0.5f) * scale) * H;
                sz[k] = p.z;
            }
            // the fringe: a pixel and a bit out from each edge, along the way the
            // strip runs across on the screen (none where it is edge-on)
            auto outward = [&](int edge, int inner, float &ox, float &oy) {
                float dx = sx[edge] - sx[inner], dy = sy[edge] - sy[inner];
                const float l = std::sqrt(dx * dx + dy * dy);
                if (l < 1e-4f) dx = dy = 0;
                else dx /= l, dy /= l;
                ox = sx[edge] + dx * 1.3f, oy = sy[edge] + dy * 1.3f;
            };
            float lx, ly, rx, ry;
            outward(0, 1, lx, ly);
            outward(NV - 1, NV - 2, rx, ry);

            for (int k = 0; k < M; k++) {
                RibbonVertex v{};
                float x, y, z, a = 1.f, vv = 0;
                if (k == 0) x = lx, y = ly, z = sz[0], a = 0, vv = -1;
                else if (k == M - 1) x = rx, y = ry, z = sz[NV - 1], a = 0, vv = 1;
                else x = sx[k - 1], y = sy[k - 1], z = sz[k - 1], vv = VS[k - 1];
                // lit: the surface bends a little across its width, which gives the
                // sheen its band; the back face is a little darker
                const V3 nrm = norm(face + B * (back ? -0.45f * vv : 0.45f * vv));
                const float diffuse = std::max(0.f, dot(nrm, light));
                // (the sheen is the 30th power: 2 + 4 + 8 + 16, by squaring)
                const float h1 = std::max(0.f, dot(nrm, half_vec)), h2 = h1 * h1, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8;
                const float spec = h2 * h4 * h8 * h16 * 0.30f;
                const float lit = (0.78f + 0.34f * diffuse) * (back ? 0.86f : 1.f);
                const float cr = std::min(hue.x * lit + spec, 0.97f), cg = std::min(hue.y * lit + spec, 0.97f),
                            cb = std::min(hue.z * lit + spec, 0.97f);
                const float al = a * std::min(alpha, 1.f);
                v.x = x, v.y = y;
                v.r = uint8_t(std::lround(std::clamp(cr * al, 0.f, 1.f) * 255.f));
                v.g = uint8_t(std::lround(std::clamp(cg * al, 0.f, 1.f) * 255.f));
                v.b = uint8_t(std::lround(std::clamp(cb * al, 0.f, 1.f) * 255.f));
                v.a = uint8_t(std::lround(std::clamp(al, 0.f, 1.f) * 255.f));
                v.depth = std::clamp(0.5f - z * Z_SCALE, 0.f, 1.f);
                vertices.push_back(v);
            }
        }
        for (int i = 0; i < N; i++)
            for (int k = 0; k + 1 < M; k++) {
                const uint32_t a = first + uint32_t(i * M + k), b = a + 1, c = first + uint32_t((i + 1) * M + k), d = c + 1;
                // the outer quads are the fringes
                std::vector<uint32_t> &list = (k == 0 || k == M - 2) ? fringe : body;
                list.insert(list.end(), {a, b, c, b, d, c});
            }
    }
}

} // namespace xf::ui
