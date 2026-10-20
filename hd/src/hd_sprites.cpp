// HD map units, unit sprites and pure sprites. ScmDraft still owns animation, hit testing and edits.
#include <windows.h>
#include "host_image.h"
#include <algorithm>
#include <list>
#include <cstdio>
#include <string>
#include <unordered_map>
#include "MinHook.h"
#include "hd_anim.h"
#include "hd_sprites.h"
void logf(const char *, ...);
void scmhdLock(); void scmhdUnlock();
int scmhdGraphicsMode();
void scmhdForgetBuffer(void *);
void scmhdLinkBuffer(void *, void *);
bool scmhdCascRead(const std::string &, std::vector<uint8_t> &);
namespace hd_sprites {
namespace {
struct Lock { Lock() { scmhdLock(); } ~Lock() { scmhdUnlock(); } };
template<class T> T &at(void *p, int off) { return *reinterpret_cast<T *>((uint8_t *)p + off); }
struct Asset { hd_anim::Anim anim; std::vector<uint8_t> bytes; };
struct Picture { hd_anim::Frame bounds; int canvasW = 0, canvasH = 0; std::vector<uint32_t> pixels; std::vector<uint8_t> team; };
std::unordered_map<unsigned, std::shared_ptr<Asset>> assets;
std::unordered_map<uint64_t, std::shared_ptr<Picture>> pictures;
std::list<unsigned> assetOrder;
std::list<uint64_t> pictureOrder;
size_t assetBytes = 0, pictureBytes = 0;
std::unordered_map<void *, Surface> surfaces;
int lastMode = -1;
Surface liveSurface;
void *liveBuffer = nullptr;

std::shared_ptr<Picture> picture(unsigned image, unsigned frame, int mode) {
    unsigned key = image | (mode == 2 ? 0x10000 : 0);
    uint64_t pk = uint64_t(key) << 32 | frame;
    auto found = pictures.find(pk); if (found != pictures.end()) return found->second;
    auto it = assets.find(key);
    if (it == assets.end()) {
        auto a = std::make_shared<Asset>(); char path[96];
        snprintf(path, sizeof path, "anim\\%smain_%03u.anim", mode == 2 ? "carbot\\" : "", image);
        if (!scmhdCascRead(path, a->bytes) || !a->anim.read(a->bytes)) {
            assets[key] = nullptr;
            // Some optional effects have no Carbot version; retain the editor's SD rendering.
            return nullptr;
        }
        // Lighting layers are irrelevant to the editor. Retain only diffuse/team data, not
        // the normal/specular/emissive atlases (which otherwise evict idle animations).
        std::vector<uint8_t> compact(4, 0);
        for (auto *texture : {&a->anim.diffuse, &a->anim.team}) if (texture->offset) {
            size_t offset = compact.size();
            compact.insert(compact.end(), a->bytes.begin() + texture->offset, a->bytes.begin() + texture->offset + texture->bytes);
            texture->offset = (uint32_t)offset;
        }
        a->bytes = std::move(compact);
        assetBytes += a->bytes.size(); assets[key] = a; assetOrder.push_back(key);
        logf("HD sprite: %s, %zu frames, canvas %dx%d", path, a->anim.frames.size(), a->anim.w, a->anim.h);
        while (assetBytes > (96u << 20) && assetOrder.size() > 1) {
            unsigned old = assetOrder.front(); assetOrder.pop_front();
            auto f = assets.find(old); if (f != assets.end() && f->second) { assetBytes -= f->second->bytes.size(); assets.erase(f); }
        }
        it = assets.find(key);
    }
    auto a = it->second;
    if (!a || frame >= a->anim.frames.size()) return nullptr;
    auto p = std::make_shared<Picture>(); p->bounds = a->anim.frames[frame];
    p->canvasW = a->anim.w; p->canvasH = a->anim.h;
    auto r = p->bounds;
    if (a->anim.scale != 4) return nullptr;
    hd_anim::Rect crop{r.tx, r.ty, r.w, r.h};
    if (!hd_anim::crop(a->bytes, a->anim.diffuse, crop, p->pixels)) return nullptr;
    if (a->anim.team.offset) {
        std::vector<uint32_t> mask;
        const auto &t = a->anim.team; const auto &d = a->anim.diffuse;
        // Remastered often stores its team mask at half the diffuse resolution.
        int mx = r.tx * t.w / d.w, my = r.ty * t.h / d.h;
        int mw = (r.tx + r.w) * t.w / d.w + 1 - mx, mh = (r.ty + r.h) * t.h / d.h + 1 - my;
        mw = std::min(mw, t.w - mx); mh = std::min(mh, t.h - my);
        if (hd_anim::crop(a->bytes, t, {mx, my, mw, mh}, mask)) {
            bool mono = !memcmp(a->bytes.data() + t.offset, "BMP ", 4);
            p->team.resize(p->pixels.size());
            for (int y = 0; y < r.h; ++y) for (int x = 0; x < r.w; ++x) {
                int u = (r.tx + x) * t.w / d.w - mx, v = (r.ty + y) * t.h / d.h - my;
                uint32_t value = mask[size_t(v) * mw + u];
                p->team[size_t(y) * r.w + x] = mono ? value >> 24 : value & 255;
            }
        }
    }
    pictureBytes += p->pixels.size() * 4 + p->team.size(); pictures[pk] = p; pictureOrder.push_back(pk);
    while (pictureBytes > (96u << 20) && pictureOrder.size() > 1) {
        auto old = pictureOrder.front(); pictureOrder.pop_front(); auto f = pictures.find(old);
        if (f != pictures.end()) { pictureBytes -= f->second->pixels.size() * 4 + f->second->team.size(); pictures.erase(f); }
    }
    return p;
}
Surface &surface(void *buffer) {
    auto &s = surfaces[buffer];
    int w = at<int>(buffer, 0xc), h = at<int>(buffer, 0x14); auto *bits = at<uint8_t *>(buffer, 0x1c);
    if (s.w != w || s.h != h || s.bits != bits) {
        s = {}; s.w = w; s.h = h; s.bits = bits; s.cols = (w + 15) / 16; s.rows = (h + 15) / 16;
        s.tiles.resize(size_t(s.cols) * s.rows);
    }
    return s;
}
Tile &writeTile(Surface &s, int x, int y) {
    auto &p = s.tiles[(y >> 4) * s.cols + (x >> 4)];
    if (!p) p = std::make_shared<Tile>();
    else if (!p.unique()) p = std::make_shared<Tile>(*p);
    p->dirty = true; return *p;
}
bool dirtyAt(const void *dirty, int x, int y, int mask) {
    if (!dirty) return true;
    auto *bytes = *reinterpret_cast<const uint8_t *const *>(dirty);
    int pitch = reinterpret_cast<const int *>(dirty)[1];
    return !bytes || (bytes[y * pitch + x] & mask);
}
void copyRect(void *src, void *dst, int x, int y, int w, int h, const void *dirty = nullptr, int mask = 0xff) {
    if (!src || !dst || src == dst || at<int>(src, 0x18) != 1 || at<int>(dst, 0x18) != 1) return;
    if (!surfaces.count(src) && !surfaces.count(dst)) return;
    auto &from = surface(src); auto &to = surface(dst);
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min({to.w, from.w, x + w}), y1 = std::min({to.h, from.h, y + h});
    for (int by = y0 / 16; by < (y1 + 15) / 16; ++by) for (int bx = x0 / 16; bx < (x1 + 15) / 16; ++bx) {
        if (!dirtyAt(dirty, bx, by, mask)) continue;
        auto &target = to.tiles[by * to.cols + bx]; auto &source = from.tiles[by * from.cols + bx];
        if (x0 <= bx * 16 && y0 <= by * 16 && x1 >= std::min(to.w, bx * 16 + 16) && y1 >= std::min(to.h, by * 16 + 16)) target = source;
        else {
            if (!target && !source) continue;
            auto &t = writeTile(to, bx * 16, by * 16);
            for (int py = std::max(y0, by * 16); py < std::min(y1, by * 16 + 16); ++py)
                for (int px = std::max(x0, bx * 16); px < std::min(x1, bx * 16 + 16); ++px) {
                    int i = (py & 15) * 16 + (px & 15);
                    t.base[i] = source ? source->base[i] : 0; t.reference[i] = source ? source->reference[i] : 0; t.covered[i] = source ? source->covered[i] : 0;
                    for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) {
                        int q = ((py & 15) * 4 + yy) * 64 + (px & 15) * 4 + xx;
                        t.rgba[q] = source ? source->rgba[q] : 0;
                    }
                }
        }
    }
}
RGBQUAD tintColor(void *buffer, uint8_t index) {
    RGBQUAD palette[256]{};
    if (GetDIBColorTable(at<HDC>(buffer, 0x2c), 0, 256, palette) == 256) return palette[index];
    // Static-sprite intermediate buffers have no DC. The PCX trailer has the shared
    // unit palette, including all player ramps; terrain colors are never taken from it.
    static std::vector<uint8_t> pcx;
    if (pcx.empty()) scmhdCascRead("game\\tunit.pcx", pcx);
    if (pcx.size() >= 769 && pcx[pcx.size() - 769] == 12) {
        const auto *c = pcx.data() + pcx.size() - 768 + index * 3;
        return RGBQUAD{c[2], c[1], c[0], 0};
    }
    return RGBQUAD{255, 255, 255, 0};
}
struct Style { bool captured = false; uint32_t flags = 0; std::array<uint8_t, 8> color{}; };
thread_local Style *currentStyle;
using GrpFn = HRESULT(__thiscall *)(void *, int, int, int, const uint8_t *, void *, unsigned, void *, void *);
GrpFn originalNormal, originalFlipped;
void captureStyle(const uint8_t *color, unsigned flags) {
    if (!currentStyle) return;
    currentStyle->captured = true; currentStyle->flags = flags;
    if ((flags & 1) && color) std::copy(color, color + 8, currentStyle->color.begin());
}
HRESULT __fastcall normalHook(void *self, void *, int frame, int x, int y, const uint8_t *color, void *remap, unsigned flags, void *dirty, void *buffer) {
    captureStyle(color, flags); return originalNormal(self, frame, x, y, color, remap, flags, dirty, buffer);
}
HRESULT __fastcall flippedHook(void *self, void *, int frame, int x, int y, const uint8_t *color, void *remap, unsigned flags, void *dirty, void *buffer) {
    captureStyle(color, flags); return originalFlipped(self, frame, x, y, color, remap, flags, dirty, buffer);
}
void paint(Surface &s, uint8_t *bits, int pitch, const std::shared_ptr<Picture> &pic, const Style &style,
           hd_anim::Rect bounds, const std::vector<uint8_t> &before, int left4, int top4, bool flip,
           const void *dirty, RGBQUAD color) {
    unsigned tint[3] = {color.rgbBlue, color.rgbGreen, color.rgbRed};
    bool shadow = (style.flags & 0x20) != 0;
    auto r = pic->bounds;
    for (int py = bounds.y; py < bounds.y + bounds.h; ++py) for (int px = bounds.x; px < bounds.x + bounds.w; ++px) {
        if (!dirtyAt(dirty, px >> 4, py >> 4, 0xc)) continue;
        uint8_t was = before[size_t(py - bounds.y) * bounds.w + px - bounds.x], now = bits[py * pitch + px];
        uint32_t pixels[16]{}; bool visible = false;
        for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) {
            int u = px * 4 + xx - left4, v = py * 4 + yy - top4;
            if (u < 0 || v < 0 || u >= r.w || v >= r.h) continue;
            if (flip) u = r.w - 1 - u;
            size_t index = size_t(v) * r.w + u; uint32_t c = pic->pixels[index]; unsigned a = c >> 24;
            if (!a) continue;
            visible = true; uint32_t out = 0;
            if (shadow) a = a * 115 / 255;
            else for (int ch = 0; ch < 3; ++ch) {
                unsigned value = c >> (ch * 8) & 255;
                if ((style.flags & 1) && !pic->team.empty()) {
                    unsigned mask = pic->team[index];
                    value = value * (255 - mask + (mask * tint[ch] + 127) / 255) / 255;
                }
                out |= (value * a + 127) / 255 << (ch * 8);
            }
            pixels[yy * 4 + xx] = out | a << 24;
        }
        if (was == now && !visible) continue;
        auto &tile = writeTile(s, px, py); int i = (py & 15) * 16 + (px & 15);
        if (!tile.covered[i] || tile.reference[i] != was) {
            tile.base[i] = was;
            for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) tile.rgba[((py & 15) * 4 + yy) * 64 + (px & 15) * 4 + xx] = 0;
        }
        tile.covered[i] = 1; tile.reference[i] = now;
        for (int yy = 0; yy < 4; ++yy) for (int xx = 0; xx < 4; ++xx) {
            auto &dest = tile.rgba[((py & 15) * 4 + yy) * 64 + (px & 15) * 4 + xx]; dest = over(pixels[yy * 4 + xx], dest);
        }
    }
}
using ImageFn = HRESULT(__thiscall *)(void *, int, int, void *, const int16_t *, void *, void *);
ImageFn originalImage;
HRESULT imageHookImpl(void *renderer, int x, int y, void *image, const int16_t *frameState, void *dirty, void *buffer, bool &rendered) {
    std::shared_ptr<Picture> pic; Style style; hd_anim::Rect bounds;
    std::vector<uint8_t> before;
    int left4 = 0, top4 = 0, mode = 0;
    bool flip = frameState && ((const uint8_t *)frameState)[2];
    {
        Lock lock;
        mode = scmhdGraphicsMode();
        if (lastMode != mode) { surfaces.clear(); lastMode = mode; }
        if (mode && image && frameState && *frameState >= 0 && buffer && at<int>(buffer, 0x18) == 1 &&
            at<int>(buffer, 0xc) > 0 && at<int>(buffer, 0xc) <= 8192 && at<int>(buffer, 0x14) > 0 && at<int>(buffer, 0x14) <= 8192) {
            auto *data = at<uint8_t *>(image, 8);
            unsigned id = data ? at<uint16_t>(data, 0x1c) : 0xffff;
            if (id < 999 && id != 0x230) pic = picture(id, (uint16_t)*frameState, mode);
            if (pic) {
                auto r = pic->bounds;
                left4 = x * 4 - pic->canvasW / 2 + r.x;
                if (flip) left4 = x * 4 + pic->canvasW / 2 - r.x - r.w;
                top4 = y * 4 - pic->canvasH / 2 + r.y;
                int sw = pic->canvasW / 4 + 4, sh = pic->canvasH / 4 + 4;
                auto *wrapper = data ? at<uint8_t *>(data, 0) : nullptr;
                auto *grp = wrapper ? at<uint8_t *>(wrapper, 0) : nullptr;
                if (grp) { sw = at<uint16_t>(grp, 2); sh = at<uint16_t>(grp, 4); }
                int l = std::max(0, std::min(left4 / 4 - 1, x - sw / 2));
                int t = std::max(0, std::min(top4 / 4 - 1, y - sh / 2));
                int right = std::min(at<int>(buffer, 0xc), std::max((left4 + r.w + 3) / 4 + 1, x + (sw + 1) / 2));
                int bottom = std::min(at<int>(buffer, 0x14), std::max((top4 + r.h + 3) / 4 + 1, y + (sh + 1) / 2));
                bounds = {l, t, std::max(0, right - l), std::max(0, bottom - t)};
                before.resize(size_t(bounds.w) * bounds.h);
                auto *bits = at<uint8_t *>(buffer, 0x1c); int pitch = at<int>(buffer, 0x10);
                for (int yy = 0; yy < bounds.h; ++yy) memcpy(before.data() + size_t(yy) * bounds.w, bits + (t + yy) * pitch + l, bounds.w);
            }
        }
    }
    auto *previous = currentStyle; currentStyle = pic ? &style : nullptr;
    rendered = true;
    HRESULT result = originalImage(renderer, x, y, image, frameState, dirty, buffer);
    currentStyle = previous;
    if (!pic || !style.captured || (style.flags & (0x80 | 0x40)) || FAILED(result)) return result;
    Lock lock;
    if (scmhdGraphicsMode() != mode) return result;
    paint(surface(buffer), at<uint8_t *>(buffer, 0x1c), at<int>(buffer, 0x10), pic, style, bounds, before,
          left4, top4, flip, dirty, tintColor(buffer, style.color[0]));
    return result;
}
HRESULT __fastcall imageHook(void *renderer, void *, int x, int y, void *image, const int16_t *frameState, void *dirty, void *buffer) {
    bool rendered = false;
    try { return imageHookImpl(renderer, x, y, image, frameState, dirty, buffer, rendered); }
    catch (...) {
        currentStyle = nullptr;
        { Lock lock; surfaces.erase(buffer); }
        logf("HD sprite: falling back after a rendering error");
        return rendered ? S_OK : originalImage(renderer, x, y, image, frameState, dirty, buffer);
    }
}
using CopyRectFn = HRESULT(__thiscall *)(void *, int, int, int, int, void *);
using ClearFn = HRESULT(__thiscall *)(void *);
CopyRectFn originalCopyRect; ClearFn originalClear, originalRelease;
HRESULT __fastcall copyRectHook(void *dst, void *, int x, int y, int w, int h, void *src) {
    { Lock lock; scmhdLinkBuffer(src, dst); copyRect(src, dst, x, y, w, h); }
    return originalCopyRect(dst, x, y, w, h, src);
}
HRESULT __fastcall releaseHook(void *buffer, void *) {
    { Lock lock; surfaces.erase(buffer); scmhdForgetBuffer(buffer); }
    return originalRelease(buffer);
}
HRESULT __fastcall clearHook(void *buffer, void *) {
    { Lock lock; surfaces.erase(buffer); }
    return originalClear(buffer);
}
} // namespace
void Tile::filter() {
    if (!dirty) return;
    const uint32_t *from = rgba.data(); int size = 64, off = 0;
    for (int level = 1; level <= 4; ++level) {
        int next = size / 2; auto *out = mip.data() + off;
        for (int y = 0; y < next; ++y) for (int x = 0; x < next; ++x) {
            auto *p = from + y * 2 * size + x * 2; uint32_t c = 0;
            for (int shift : {0, 8, 16, 24}) c |= (((p[0] >> shift & 255) + (p[1] >> shift & 255) + (p[size] >> shift & 255) + (p[size + 1] >> shift & 255) + 2) / 4) << shift;
            out[y * next + x] = c;
        }
        from = out; off += next * next; size = next;
    }
    dirty = false;
}
void copyBlocks(const int *dim, const void *dirty, int mask, void *src, void *dst) {
    // HD smoke/overlays can extend beyond their SD dirty bounds. Expand render-only dirt
    // before the background copy; the subsequent original GRP draws read the same flags.
    // This clears the previous HD frame as well as drawing the new one, without trails.
    if (scmhdGraphicsMode() && dim && dirty && (mask & 0xc)) {
        auto *grid = *reinterpret_cast<uint8_t *const *>(dirty);
        int pitch = reinterpret_cast<const int *>(dirty)[1], cols = dim[0] * 2, rows = dim[1] * 2;
        if (grid && cols > 0 && rows > 0 && pitch >= cols && pitch <= 2048 && rows <= 1024) {
            std::vector<uint8_t> flags(grid, grid + size_t(pitch) * rows);
            for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) if (flags[y * pitch + x] & mask & 0xc)
                for (int yy = std::max(0, y - 2); yy <= std::min(rows - 1, y + 2); ++yy)
                    for (int xx = std::max(0, x - 2); xx <= std::min(cols - 1, x + 2); ++xx) grid[yy * pitch + xx] |= mask & 0xc;
        }
    }
    if (dim) copyRect(src, dst, 0, 0, dim[0] * 32, dim[1] * 32, dirty, mask);
}
void reset() { surfaces.clear(); liveSurface = {}; liveBuffer = nullptr; lastMode = scmhdGraphicsMode(); }
const Surface *prepare(void *buffer) {
    auto it = surfaces.find(buffer); if (it == surfaces.end() && liveBuffer != buffer) return nullptr;
    auto &s = liveBuffer == buffer ? liveSurface : surface(buffer);
    for (auto &t : s.tiles) if (t) t->filter();
    return &s;
}
bool snapshot(int mode, unsigned image, unsigned frame, bool flip, uint32_t tint, unsigned opacity,
              hd_anim::Rect &bounds, std::vector<uint32_t> &pixels) {
    auto pic = picture(image, frame, mode); if (!pic) return false;
    auto r = pic->bounds;
    bounds = {r.x - pic->canvasW / 2, r.y - pic->canvasH / 2, r.w, r.h};
    if (flip) bounds.x = pic->canvasW / 2 - r.x - r.w;
    pixels.resize(pic->pixels.size());
    for (int y = 0; y < r.h; ++y) for (int x = 0; x < r.w; ++x) {
        size_t i = size_t(y) * r.w + (flip ? r.w - 1 - x : x);
        uint32_t c = pic->pixels[i], out = 0; unsigned a = (c >> 24) * opacity / 255;
        unsigned mask = pic->team.empty() ? 0 : pic->team[i];
        for (int shift : {0, 8, 16}) {
            unsigned v = (c >> shift & 255) * (255 - mask + (mask * (tint >> shift & 255) + 127) / 255) / 255;
            out |= (v * a + 127) / 255 << shift;
        }
        pixels[size_t(y) * r.w + x] = out | a << 24;
    }
    return true;
}
void beginLive(void *buffer) {
    liveSurface = surface(buffer); liveBuffer = buffer;
}
void endLive() { liveBuffer = nullptr; liveSurface = {}; }
void drawLive(void *buffer, uint8_t *bits, int pitch, unsigned image, unsigned frame, int x, int y,
              hd_anim::Rect sd, bool flip, const uint8_t *team, bool shadow, const std::function<void()> &drawSD) {
    auto pic = scmhdGraphicsMode() ? picture(image, frame, scmhdGraphicsMode()) : nullptr;
    if (!pic || liveBuffer != buffer) { drawSD(); return; }
    auto r = pic->bounds;
    int left4 = x * 4 - pic->canvasW / 2 + r.x;
    if (flip) left4 = x * 4 + pic->canvasW / 2 - r.x - r.w;
    int top4 = y * 4 - pic->canvasH / 2 + r.y;
    int l = std::max(0, std::min(sd.x, left4 / 4 - 1)), t = std::max(0, std::min(sd.y, top4 / 4 - 1));
    int right = std::min(liveSurface.w, std::max(sd.x + sd.w, (left4 + r.w + 3) / 4 + 1));
    int bottom = std::min(liveSurface.h, std::max(sd.y + sd.h, (top4 + r.h + 3) / 4 + 1));
    hd_anim::Rect bounds{l, t, std::max(0, right - l), std::max(0, bottom - t)};
    std::vector<uint8_t> before(size_t(bounds.w) * bounds.h);
    for (int yy = 0; yy < bounds.h; ++yy) memcpy(before.data() + size_t(yy) * bounds.w, bits + (t + yy) * pitch + l, bounds.w);
    drawSD();
    Style style; style.flags = shadow ? 0x20 : 1;
    paint(liveSurface, bits, pitch, pic, style, bounds, before, left4, top4, flip, nullptr, tintColor(buffer, team[0]));
}
bool install() {
    auto hook = [](uintptr_t address, void *replacement, void **original) { return host_image::createHook(address, replacement, original, "HD sprites"); };
    return hook(0x4b5730, (void *)&imageHook, (void **)&originalImage) &&
           hook(0x446500, (void *)&normalHook, (void **)&originalNormal) &&
           hook(0x446110, (void *)&flippedHook, (void **)&originalFlipped) &&
           hook(0x44ad20, (void *)&copyRectHook, (void **)&originalCopyRect) &&
           hook(0x44aa10, (void *)&clearHook, (void **)&originalClear) &&
           hook(0x44bdb0, (void *)&releaseHook, (void **)&originalRelease);
}
} // namespace hd_sprites
