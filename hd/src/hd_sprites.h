#pragma once
#include <algorithm>
#include <array>
#include <functional>
#include "hd_anim.h"
#include <cstdint>
#include <memory>
#include <vector>
namespace hd_sprites {
// Sparse 16x16 logical-pixel blocks, with 4x art and premultiplied box-filtered mip levels.
struct Tile {
    std::array<uint32_t, 4096> rgba{};
    std::array<uint8_t, 256> base{}, reference{}, covered{};
    std::array<uint32_t, 1360> mip{}; // 32² + 16² + 8² + 4²
    bool dirty = true;
    void filter();
};
struct Surface {
    int w = 0, h = 0, cols = 0, rows = 0;
    const uint8_t *bits = nullptr;
    std::vector<std::shared_ptr<Tile>> tiles;
    const Tile *tile(int x, int y) const { return tiles[(y >> 4) * cols + (x >> 4)].get(); }
};
bool install();
void reset();
bool snapshot(int mode, unsigned image, unsigned frame, bool flip, uint32_t tint, unsigned opacity,
              hd_anim::Rect &bounds, std::vector<uint32_t> &pixels);
void beginLive(void *buffer);
void endLive();
void drawLive(void *buffer, uint8_t *bits, int pitch, unsigned image, unsigned frame, int x, int y,
              hd_anim::Rect sd, bool flip, const uint8_t *team, bool shadow, const std::function<void()> &drawSD);
void copyBlocks(const int *dimensions, const void *dirty, int mask, void *src, void *dst);
const Surface *prepare(void *buffer);
inline uint8_t basePixel(const Tile *t, int x, int y, uint8_t actual) {
    int i = (y & 15) * 16 + (x & 15);
    return t && t->covered[i] && t->reference[i] == actual ? t->base[i] : actual;
}
inline uint32_t over(uint32_t fg, uint32_t bg) {
    unsigned a = fg >> 24;
    if (!a) return bg;
    if (a == 255) return fg;
    unsigned inv = 256 - a;
    uint32_t rb = (fg & 0xff00ff) + (((bg & 0xff00ff) * inv >> 8) & 0xff00ff);
    uint32_t g = (fg & 0xff00) + (((bg & 0xff00) * inv >> 8) & 0xff00);
    unsigned alpha = a + ((bg >> 24) * inv >> 8);
    return rb | g | alpha << 24;
}
inline uint32_t pixel(const Tile *t, int x, int y, int sampleX, int sampleY, int level, uint8_t actual) {
    if (!t) return 0;
    int i = (y & 15) * 16 + (x & 15);
    if (t->covered[i] && t->reference[i] != actual) return 0;
    int size = 64 >> level;
    const uint32_t *p = t->rgba.data();
    if (level) { constexpr int offsets[] = {0, 0, 1024, 1280, 1344}; p = t->mip.data() + offsets[level]; }
    return p[sampleY * size + sampleX];
}
} // namespace hd_sprites
