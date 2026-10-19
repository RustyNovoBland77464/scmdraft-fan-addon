// Portable decoder checks, no Blizzard assets required.
// clang++ -std=c++17 -O2 hd/tests/hd_anim_test.cpp -o /tmp/hd_anim_test && /tmp/hd_anim_test
#include "../src/hd_anim.h"
#include "../src/hd_sprites.h"
#include <cassert>
#include <cstdio>
using namespace hd_anim;
void put16(std::vector<uint8_t> &b, size_t off, uint16_t n) { b[off] = n; b[off + 1] = n >> 8; }
void put32(std::vector<uint8_t> &b, size_t off, uint32_t n) { put16(b, off, n); put16(b, off + 2, n >> 16); }
std::vector<uint8_t> dds(uint32_t fourcc, int bytes) {
    std::vector<uint8_t> b(4 + 128 + bytes); memcpy(b.data() + 4, "DDS ", 4);
    put32(b, 4 + 12, 4); put32(b, 4 + 16, 4); put32(b, 4 + 84, fourcc); return b;
}
int main() {
    std::vector<uint32_t> out;
    auto b = dds(0x31545844, 8); Texture t{4, 136, 4, 4};
    put16(b, 132, 0xf800); put16(b, 134, 0x07e0); put32(b, 136, 0xe4e4e4e4);
    assert(crop(b, t, {0, 0, 4, 4}, out));
    assert(out[0] == 0xffff0000 && out[1] == 0xff00ff00 && out[2] == 0xffaa5500 && out[3] == 0xff55aa00);
    assert(crop(b, t, {1, 1, 2, 2}, out) && out.size() == 4 && out[0] == 0xff00ff00);
    assert(!crop(b, t, {-1, 0, 1, 1}, out)); assert(!crop(b, t, {3, 3, 2, 2}, out));
    put16(b, 132, 0); put16(b, 134, 0xffff); put32(b, 136, 0xffffffff);
    assert(crop(b, t, {0, 0, 4, 4}, out) && out[0] == 0);
    b = dds(0x35545844, 16); t = {4, 144, 4, 4};
    b[132] = 200; b[133] = 20; put16(b, 140, 0xffff); put16(b, 142, 0);
    uint64_t indices = 0; for (int k = 0; k < 16; ++k) indices |= uint64_t(k % 8) << (k * 3);
    for (int j = 0; j < 6; ++j) b[134 + j] = indices >> (j * 8);
    assert(crop(b, t, {0, 0, 4, 4}, out));
    assert(out[0] >> 24 == 200 && out[1] >> 24 == 20 && out[2] >> 24 == 174 && out[7] >> 24 == 45);
    b[132] = 20; b[133] = 200; assert(crop(b, t, {0, 0, 4, 4}, out));
    assert(out[6] >> 24 == 0 && out[7] >> 24 == 255);
    b.resize(140); assert(!crop(b, t, {0, 0, 4, 4}, out));
    b = dds(0x33545844, 16); t = {4, 144, 4, 4};
    for (int i = 0; i < 8; ++i) b[132 + i] = i * 2 | (i * 2 + 1) << 4;
    put16(b, 140, 0xffff);
    assert(crop(b, t, {0, 0, 4, 4}, out) && out[0] >> 24 == 0 && out[8] >> 24 == 136 && out[15] >> 24 == 255);
    b.assign(20, 0); memcpy(b.data() + 4, "BMP ", 4); b[8] = 255; t = {4, 16, 2, 2};
    assert(crop(b, t, {0, 0, 2, 2}, out) && out[0] == 0xffffffff && out[1] == 0xffffff);
    std::vector<uint8_t> a(0x158 + 12 + 16 + 136); memcpy(a.data(), "ANIM", 4); a[4] = 4; a[5] = 2;
    put16(a, 8, 1); put16(a, 10, 1); memcpy(a.data() + 12, "diffuse", 8);
    put16(a, 0x14c, 1); put16(a, 0x14e, 0xffff); put16(a, 0x150, 4); put16(a, 0x152, 4);
    put32(a, 0x154, 0x164); put32(a, 0x158, 0x174); put32(a, 0x15c, 136); put16(a, 0x160, 4); put16(a, 0x162, 4);
    put16(a, 0x168, uint16_t(-2)); put16(a, 0x16c, 4); put16(a, 0x16e, 4);
    Anim anim; assert(anim.read(a) && anim.frames[0].x == -2 && anim.frames[0].w == 4);
    put32(a, 0x154, 0xfffffff0); assert(!anim.read(a));
    assert(hd_sprites::over(0, 0x123456) == 0x123456);
    assert(hd_sprites::over(0xffabcdef, 0x123456) == 0xffabcdef);
    assert(hd_sprites::over(0x80800000, 0xff0000ff) == 0xff80007f);
    puts("ANIM, DXT1/DXT3/DXT5, monochrome, crop bounds and alpha compositing passed");
}
