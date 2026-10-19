// SC:R ANIM reader. Format research: https://github.com/neivv/animosity (src/anim.rs).
// Independent decoder; no game art is embedded in the plugin.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hd_anim {
inline uint16_t u16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t *p) { return u16(p) | uint32_t(u16(p + 2)) << 16; }
struct Rect { int x = 0, y = 0, w = 0, h = 0; };
struct Frame : Rect { int tx = 0, ty = 0; };
struct Texture { uint32_t offset = 0, bytes = 0; int w = 0, h = 0; };
struct Anim {
    int w = 0, h = 0, scale = 4;
    Texture diffuse, team;
    std::vector<Frame> frames;
    bool read(const std::vector<uint8_t> &data) {
        if (data.size() < 0x158 || memcmp(data.data(), "ANIM", 4) || data[5] != 2 ||
            (data[4] != 2 && data[4] != 4) || u16(data.data() + 10) != 1) return false;
        auto *p = data.data();
        int layers = u16(p + 8), count = u16(p + 0x14c);
        if (layers > 10 || !layers || !count || u16(p + 0x14e) != 0xffff ||
            data.size() < 0x158 + size_t(layers) * 12) return false;
        w = u16(p + 0x150); h = u16(p + 0x152); scale = p[4];
        uint32_t off = u32(p + 0x154);
        if (!w || !h || w > 4096 || h > 4096 || off > data.size() || size_t(count) * 16 > data.size() - off) return false;
        diffuse = {}; team = {}; frames.clear();
        for (int i = 0; i < layers; ++i) {
            auto *t = p + 0x158 + i * 12;
            Texture tex{u32(t), u32(t + 4), u16(t + 8), u16(t + 10)};
            if (tex.offset > data.size() || tex.bytes > data.size() - tex.offset || tex.w > 16384 || tex.h > 16384) return false;
            if (!strncmp((const char *)p + 12 + i * 32, "diffuse", 32)) diffuse = tex;
            if (!strncmp((const char *)p + 12 + i * 32, "teamcolor", 32)) team = tex;
        }
        for (int i = 0; i < count; ++i) {
            auto *f = p + off + i * 16;
            Frame frame; frame.tx = u16(f); frame.ty = u16(f + 2);
            frame.x = (int16_t)u16(f + 4); frame.y = (int16_t)u16(f + 6);
            frame.w = u16(f + 8); frame.h = u16(f + 10);
            if (frame.w > 4096 || frame.h > 4096) return false;
            frames.push_back(frame);
        }
        return diffuse.offset && diffuse.w && diffuse.h;
    }
};
inline uint32_t rgb565(uint16_t c) {
    return (uint32_t(((c >> 11) * 255 + 15) / 31) << 16) |
           (uint32_t((((c >> 5) & 63) * 255 + 31) / 63) << 8) | ((c & 31) * 255 + 15) / 31;
}
inline uint32_t mix(uint32_t a, uint32_t b, int wa, int wb, int d) {
    uint32_t r = 0;
    for (int s : {0, 8, 16}) r |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / d) << s;
    return r;
}
// Decode only blocks intersecting the requested frame, rather than an entire animation atlas.
inline bool crop(const std::vector<uint8_t> &data, const Texture &t, Rect r, std::vector<uint32_t> &out) {
    if (!t.offset || t.offset > data.size() || t.bytes > data.size() - t.offset || r.w <= 0 || r.h <= 0 ||
        r.x < 0 || r.y < 0 || r.x + r.w > t.w || r.y + r.h > t.h) return false;
    auto *p = data.data() + t.offset;
    out.assign(size_t(r.w) * r.h, 0);
    if (t.bytes >= 4 && !memcmp(p, "BMP ", 4)) {
        if (size_t(t.w) * t.h > t.bytes - 4) return false;
        for (int y = 0; y < r.h; ++y) for (int x = 0; x < r.w; ++x)
            out[size_t(y) * r.w + x] = 0xffffff | uint32_t(p[4 + (r.y + y) * t.w + r.x + x]) << 24;
        return true;
    }
    if (t.bytes < 128 || memcmp(p, "DDS ", 4) || u32(p + 12) != unsigned(t.h) || u32(p + 16) != unsigned(t.w)) return false;
    uint32_t fmt = u32(p + 84);
    int block = fmt == 0x31545844 ? 8 : (fmt == 0x35545844 || fmt == 0x33545844 ? 16 : 0);
    if (!block || size_t((t.w + 3) / 4) * ((t.h + 3) / 4) * block > t.bytes - 128) return false;
    for (int by = r.y / 4; by <= (r.y + r.h - 1) / 4; ++by) for (int bx = r.x / 4; bx <= (r.x + r.w - 1) / 4; ++bx) {
        auto *b = p + 128 + (size_t(by) * ((t.w + 3) / 4) + bx) * block;
        auto *c = b + block - 8;
        uint16_t c0 = u16(c), c1 = u16(c + 2);
        uint32_t colors[4] = {rgb565(c0), rgb565(c1), 0, 0};
        bool transparent = c0 <= c1 && block == 8;
        colors[2] = transparent ? mix(colors[0], colors[1], 1, 1, 2) : mix(colors[0], colors[1], 2, 1, 3);
        colors[3] = transparent ? 0 : mix(colors[0], colors[1], 1, 2, 3);
        uint32_t indices = u32(c + 4);
        unsigned alpha[8] = {b[0], b[1]}; uint64_t alphaBits = 0;
        if (fmt == 0x35545844) {
            for (int i = 2; i < 8; ++i) alpha[i] = b[0] > b[1] ? ((8 - i) * b[0] + (i - 1) * b[1]) / 7 :
                (i < 6 ? ((6 - i) * b[0] + (i - 1) * b[1]) / 5 : (i == 6 ? 0 : 255));
            for (int i = 0; i < 6; ++i) alphaBits |= uint64_t(b[2 + i]) << (i * 8);
        } else if (fmt == 0x33545844) for (int i = 0; i < 8; ++i) alphaBits |= uint64_t(b[i]) << (i * 8);
        for (int k = 0; k < 16; ++k) {
            int x = bx * 4 + k % 4 - r.x, y = by * 4 + k / 4 - r.y;
            if (x < 0 || y < 0 || x >= r.w || y >= r.h) continue;
            unsigned index = indices >> (k * 2) & 3;
            unsigned a = fmt == 0x35545844 ? alpha[alphaBits >> (k * 3) & 7] :
                (fmt == 0x33545844 ? ((alphaBits >> (k * 4)) & 15) * 17 : (transparent && index == 3 ? 0 : 255));
            out[size_t(y) * r.w + x] = colors[index] | a << 24;
        }
    }
    return true;
}
} // namespace hd_anim
