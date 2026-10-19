// scmhd.sdp - ScmDraft 2 (0.9.10): Remastered / Carbot terrain, units and sprites at every zoom.
//
// How it works (see ../../re/NOTES.md for the reverse engineering):
//  * ScmDraft renders the map into 8-bit CRenderBuffers. Terrain is cached in its own buffer and
//    copied into the backbuffer in 16x16 blocks by 0x44cff0 (stdcall src->dst); units, sprites
//    and overlays are drawn on top. CRenderEngine::Present (0x44d450) and other repaint paths
//    (colour cycling ~13 Hz, brush updates) StretchBlt the backbuffer to the map window.
//  * We hook the block copy (MinHook) and patch ScmDraft's StretchBlt import. A magnified blit out
//    of a known render buffer is replaced by our own full-resolution composite, so SD pixels never
//    reach the window (drawing HD over ScmDraft's blit afterwards visibly flickered): every pixel
//    that still equals the identified megatile's SD pixel (i.e. uncovered terrain) samples the HD
//    texture; everything else keeps ScmDraft's palette colour.
//  * The megatile shown in each 32x32 terrain block is identified by hashing its SD pixels and
//    looking the hash up in a table built from every tileset's vx4ex + vr4. No map structures
//    are needed.
//  * HD textures come straight from the CASC storage ("TileSet\x.dds.vr4", "HD2\...", Carbot).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <algorithm>
#include "CascLib.h"
#include "MinHook.h"
#include "hd_sprites.h"
#include "map_sampling.h"

FILE *g_log;

// ScmDraft presents the map view from a worker thread while our Test Mining timer, menus and clicks run
// on the UI thread, and both touch the same state (texture cache, output bitmap, live game, overlay
// data, button rectangles). Everything shared goes through this lock. Never hold it while calling back
// into ScmDraft (e.g. SaveToMemory): its render thread may own ScmDraft's locks while waiting for ours.
CRITICAL_SECTION g_drawLock;
struct DrawLock {
    DrawLock() { EnterCriticalSection(&g_drawLock); }
    ~DrawLock() { LeaveCriticalSection(&g_drawLock); }
    DrawLock(const DrawLock &) = delete;
};
void scmhdLock() { EnterCriticalSection(&g_drawLock); }
void scmhdUnlock() { LeaveCriticalSection(&g_drawLock); }
void logf(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

namespace sym { bool install(); void requestMirroredCopy(HWND mainWnd, void *map); void replaceUnitsBySprites(HWND mainWnd, void *map); }
namespace { bool cascRead(const std::string &name, std::vector<uint8_t> &out); }
bool scmhdCascRead(const std::string &name, std::vector<uint8_t> &out) { return cascRead(name, out); }
namespace { std::string starcraftPath(); }
std::string scmhdStarcraftPath() { return starcraftPath(); }
namespace live {
const uint8_t *shadowRemap(int tileset);
bool enabled(); bool running(); void setEnabled(bool on, HWND mainWnd); bool onTimer(); UINT_PTR timerId();
void drawSprites(void *back, uint8_t *buf, int pitch, int w, int h, int originX, int originY);
void drawUI(HDC dc, HWND wnd, int xd, int yd, int wd, int hd, double xs, double ys, double ws, double hs, int originX, int originY);
bool onClick(HWND wnd, int x, int y); bool overButton(HWND wnd, int x, int y);
}
namespace mining {
void run(); void init(HMODULE self, HWND mainWnd);
void drawOverlay(HDC dc, int xd, int yd, int wd, int hd, double xs, double ys, double ws, double hs, int originX, int originY, double srcScale);
}
namespace hitboxes {
void init(HWND); void toggle(HWND, bool); bool enabled(bool); UINT_PTR timerId(); void update();
void draw(HDC, void *, int, int, int, int, double, double, double, double, int, int);
}
uint8_t *g_activeBackbuffer;  // backbuffer of the map view drawn last (set by the blit hooks)
// CEngineMap's CHK object for the map view drawn last: engine = backbuffer - 0x970, map = engine+0xb0,
// chk = map+0x6fc.
// CEngineMap of the map view drawn last (engine = backbuffer - 0x970, map = engine+0xb0)
void *scmhdActiveMap() {
    if (!g_activeBackbuffer) return nullptr;
    uint8_t *engine = g_activeBackbuffer - 0x970;
    if (IsBadReadPtr(engine + 0xb0, 4)) return nullptr;
    return *reinterpret_cast<void **>(engine + 0xb0);
}

void *scmhdActiveMapChk() {
    if (!g_activeBackbuffer) return nullptr;
    uint8_t *engine = g_activeBackbuffer - 0x970;
    if (IsBadReadPtr(engine + 0xb0, 4)) return nullptr;
    uint8_t *map = *reinterpret_cast<uint8_t **>(engine + 0xb0);
    if (!map || IsBadReadPtr(map + 0x6fc, 4)) return nullptr;
    uint8_t *chk = *reinterpret_cast<uint8_t **>(map + 0x6fc);
    if (!chk || IsBadReadPtr(chk + 0x50, 0x20)) return nullptr;
    int w = *reinterpret_cast<int *>(chk + 0x58), h = *reinterpret_cast<int *>(chk + 0x5c);
    return w > 0 && w <= 256 && h > 0 && h <= 256 ? chk : nullptr;
}

namespace {

// ---- addresses in ScmDraft 2 Beta 0.9.10 (sha256 751060e3...) ----------------------------------
constexpr uintptr_t kCopyBlocks = 0x0044cff0; // HRESULT __stdcall CopyDirtyBlocks(dirty*, ?, ?, src, dst)
constexpr uint32_t kExpectedTimestamp = 0x5EF3C9F5; // PE header TimeDateStamp, 2020-06-24 21:47:33

// CRenderBuffer fields
constexpr int E_BACKBUF_OFFSET = 0x970;  // CRenderEngine::backbuffer (CRenderBuffer)
constexpr int B_W = 0xc, B_STRIDE = 0x10, B_H = 0x14, B_FMT = 0x18, B_BITS = 0x1c, B_HDC = 0x2c;

template <class T> T &at(void *p, int off) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off); }

// ---- menu ids (FourCC) --------------------------------------------------------------------------
constexpr DWORD kMenuOff = 'HDOF', kMenuHD = 'HDHD', kMenuCarbot = 'HDCB', kMenuLive = 'LIVE', kMenuMirror = 'MIRR', kMenuUnitSprite = 'USPR', kMenuBuildingBoxes = 'BBOX', kMenuUnitBoxes = 'UBOX';
const char *kMirrorMenuText = "Copy Mirrored (Left-Right)\tCtrl+Shift+C";
const char *kUnitSpriteMenuText = "Replace by Unit Sprite";
const char *kLiveMenuText = "Test Mining";
const char *kBuildingBoxesText = "Show Building Hitboxes";
const char *kUnitBoxesText = "Show Unit Hitboxes";
enum Mode { MODE_OFF = 0, MODE_HD = 1, MODE_CARBOT = 2 };

HMODULE g_self;
int g_mode = MODE_HD;


void loadMode() {
    DWORD v = MODE_HD, sz = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\ScmDraft HD", "Mode", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v <= 2)
        g_mode = (int)v;
}
void saveMode() {
    DWORD v = (DWORD)g_mode;
    RegSetKeyValueA(HKEY_CURRENT_USER, "Software\\ScmDraft HD", "Mode", REG_DWORD, &v, sizeof v);
}

// ---- CASC ---------------------------------------------------------------------------------------
HANDLE g_casc;
bool g_cascFailed;

std::string starcraftPath() {
    char buf[MAX_PATH] = ""; DWORD sz = sizeof buf;
    // Release Setup stores an explicit per-user location; graphics and mining share this lookup.
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\ScmDraft HD", "StarcraftPath",
                     RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS && buf[0]) return buf;
    const char *keys[][2] = {{"SOFTWARE\\Blizzard Entertainment\\Starcraft", "InstallPath"},
                             {"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Starcraft", "InstallLocation"}};
    for (auto &k : keys) {
        sz = sizeof buf;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, k[0], k[1], RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS && buf[0]) return buf;
    }
    return "C:\\Program Files (x86)\\StarCraft";
}

bool cascOpen() {
    if (g_casc) return true;
    if (g_cascFailed) return false;
    std::string p = starcraftPath();
    if (!CascOpenStorage(p.c_str(), 0, &g_casc)) {
        logf("CascOpenStorage(%s) failed: %u", p.c_str(), GetCascError());
        g_cascFailed = true; g_casc = nullptr; return false;
    }
    logf("opened CASC storage at %s", p.c_str());
    return true;
}

bool cascRead(const std::string &name, std::vector<uint8_t> &out) {
    out.clear();
    if (!cascOpen()) return false;
    HANDLE f;
    if (!CascOpenFile(g_casc, name.c_str(), 0, CASC_OPEN_BY_NAME, &f)) { logf("missing in CASC: %s", name.c_str()); return false; }
    ULONGLONG size = 0; CascGetFileSize64(f, &size);
    out.resize((size_t)size);
    DWORD rd = 0; size_t got = 0;
    while (got < out.size() && CascReadFile(f, out.data() + got, (DWORD)(out.size() - got), &rd) && rd) got += rd;
    CascCloseFile(f);
    out.resize(got);
    return got == size;
}

// ---- tilesets & megatile identification --------------------------------------------------------
const char *kTilesets[8] = {"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"};

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
// Hash a 32x32 SD block, skipping every pixel on an 8-pixel boundary row/column: ScmDraft draws
// its grid lines into the terrain buffer, and any grid size that is a multiple of 8 lands there.
uint64_t blockHash(const uint8_t *p, int stride) {
    uint64_t h = kFnvBasis;
    for (int y = 0; y < 32; y++) {
        if (!(y & 7)) continue;
        const uint8_t *r = p + y * stride;
        for (int x = 0; x < 32; x += 8) {
            uint64_t w; memcpy(&w, r + x, 8);
            w &= ~0xffULL;  // byte 0 of each 8-pixel group is on a grid column (little-endian)
            h = (h ^ w) * 0x100000001b3ULL;
            h ^= h >> 29;
        }
    }
    return h;
}

struct Tileset { std::vector<uint8_t> vx, vr; size_t nMega = 0, nMini = 0; };
Tileset g_ts[8];

void renderSD(const Tileset &t, size_t m, uint8_t *tile) {
    const uint32_t *mega = reinterpret_cast<const uint32_t *>(t.vx.data()) + m * 16;
    for (int i = 0; i < 16; i++) {
        uint32_t e = mega[i], mini = e >> 1;
        if (mini >= t.nMini) mini = 0;
        const uint8_t *src = t.vr.data() + mini * 64;
        int ox = (i & 3) * 8, oy = (i >> 2) * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                tile[(oy + y) * 32 + ox + x] = src[y * 8 + ((e & 1) ? 7 - x : x)];
    }
}

struct TileRef { uint8_t ts; uint16_t mt; };
std::unordered_map<uint64_t, std::vector<TileRef>> g_tileIndex;
bool g_indexBuilt, g_indexFailed;
int g_curTileset = -1;

bool buildIndex() {
    if (g_indexBuilt) return true;
    if (g_indexFailed) return false;
    DWORD t0 = GetTickCount();
    size_t total = 0;
    uint8_t tile[32 * 32];
    for (int ts = 0; ts < 8; ts++) {
        Tileset &t = g_ts[ts];
        if (!cascRead(std::string("TileSet\\") + kTilesets[ts] + ".vx4ex", t.vx) ||
            !cascRead(std::string("TileSet\\") + kTilesets[ts] + ".vr4", t.vr)) { t = Tileset(); continue; }
        t.nMega = std::min<size_t>(t.vx.size() / 64, 65536); t.nMini = t.vr.size() / 64;
        for (size_t m = 0; m < t.nMega; m++) {
            renderSD(t, m, tile);
            auto &v = g_tileIndex[blockHash(tile, 32)];
            bool dup = false;
            for (auto &r : v) dup |= r.ts == ts;
            if (!dup) v.push_back({(uint8_t)ts, (uint16_t)m});
            total++;
        }
    }
    if (!total) { g_indexFailed = true; logf("tile index: no tilesets loaded"); return false; }
    g_indexBuilt = true;
    logf("tile index: %zu megatiles, %zu unique SD images, %lu ms", total, g_tileIndex.size(), GetTickCount() - t0);
    return true;
}

// SD pixels of a megatile, cached (used to tell uncovered terrain from things drawn on top).
std::unordered_map<uint32_t, std::vector<uint8_t>> g_sdCache;
const uint8_t *sdTile(int ts, int mt) {
    uint32_t key = (uint32_t)ts << 16 | (uint32_t)mt;
    auto it = g_sdCache.find(key);
    if (it != g_sdCache.end()) return it->second.data();
    if (g_sdCache.size() > 8192) g_sdCache.clear();
    auto &v = g_sdCache[key];
    v.resize(1024);
    renderSD(g_ts[ts], mt, v.data());
    return v.data();
}

// ---- HD texture sets (.dds.vr4) ----------------------------------------------------------------
struct TexSet {
    bool tried = false, ok = false;
    std::vector<uint8_t> data;
    std::vector<uint32_t> frameOff; // offset of each DDS blob
    int size = 0;                   // texture edge length (64 for HD2, 128 for HD)
};
// [variant][tileset]; variant = carbot*2 + (hd ? 1 : 0)
TexSet g_tex[4][8];

const char *variantPath(int v) {
    static const char *p[4] = {"HD2\\TileSet\\", "TileSet\\", "HD2\\Carbot\\TileSet\\", "Carbot\\TileSet\\"};
    return p[v];
}

TexSet *texSet(int variant, int ts) {
    TexSet &t = g_tex[variant][ts];
    if (t.tried) return t.ok ? &t : nullptr;
    t.tried = true;
    DWORD t0 = GetTickCount();
    std::string name = std::string(variantPath(variant)) + kTilesets[ts] + ".dds.vr4";
    if (!cascRead(name, t.data) || t.data.size() < 8) return nullptr;
    uint16_t count = *reinterpret_cast<uint16_t *>(&t.data[4]);
    size_t off = 8;
    for (int i = 0; i < count && off + 12 <= t.data.size(); i++) {
        uint16_t w = *reinterpret_cast<uint16_t *>(&t.data[off + 4]);
        uint32_t sz = *reinterpret_cast<uint32_t *>(&t.data[off + 8]);
        if (!t.size) t.size = w;
        t.frameOff.push_back((uint32_t)(off + 12));
        off += 12 + sz;
    }
    t.ok = t.size > 0 && !t.frameOff.empty();
    logf("loaded %s: %zu frames of %dpx, %zu bytes, %lu ms", name.c_str(), t.frameOff.size(), t.size, t.data.size(), GetTickCount() - t0);
    return t.ok ? &t : nullptr;
}

inline uint32_t rgb565(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 + 15) / 31) << 16 | ((g * 255 + 31) / 63) << 8 | ((b * 255 + 15) / 31);
}
inline uint32_t mix(uint32_t a, uint32_t b, int wa, int wb, int d) {
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) out |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / d) << s;
    return out;
}

// Decode one DDS (DXT1 / DXT5; alpha ignored) into BGRX pixels.
bool decodeDds(const uint8_t *p, size_t avail, int size, std::vector<uint32_t> &out) {
    if (avail < 128 || memcmp(p, "DDS ", 4)) return false;
    uint32_t h = *reinterpret_cast<const uint32_t *>(p + 12), w = *reinterpret_cast<const uint32_t *>(p + 16);
    uint32_t fourcc = *reinterpret_cast<const uint32_t *>(p + 84);
    if ((int)w != size || (int)h != size) return false;
    int blockBytes;
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) blockBytes = 8;
    else if (fourcc == MAKEFOURCC('D', 'X', 'T', '5') || fourcc == MAKEFOURCC('D', 'X', 'T', '3')) blockBytes = 16;
    else return false;
    const uint8_t *b = p + 128;
    if (avail < 128 + (size_t)(w / 4) * (h / 4) * blockBytes) return false;
    out.assign(w * h, 0);
    for (uint32_t by = 0; by < h; by += 4)
        for (uint32_t bx = 0; bx < w; bx += 4, b += blockBytes) {
            const uint8_t *c = b + blockBytes - 8;
            uint16_t c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
            uint32_t bits = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
            uint32_t pal[4] = {rgb565(c0), rgb565(c1), 0, 0};
            if (c0 > c1 || blockBytes == 16) { pal[2] = mix(pal[0], pal[1], 2, 1, 3); pal[3] = mix(pal[0], pal[1], 1, 2, 3); }
            else { pal[2] = mix(pal[0], pal[1], 1, 1, 2); pal[3] = 0; }
            for (int k = 0; k < 16; k++) out[(by + k / 4) * w + bx + k % 4] = pal[(bits >> (2 * k)) & 3];
        }
    return true;
}

// Decoded-texture LRU cache.
struct CacheEntry { std::vector<uint32_t> px; std::list<uint64_t>::iterator lru; };
std::unordered_map<uint64_t, CacheEntry> g_cache;
std::list<uint64_t> g_lru;
size_t g_cacheBytes;
constexpr size_t kCacheLimit = 96u << 20;

const uint32_t *cachePut(uint64_t key, std::vector<uint32_t> &&px) {
    g_cacheBytes += px.size() * 4;
    g_lru.push_front(key);
    CacheEntry &slot = g_cache[key];
    slot.px = std::move(px);
    slot.lru = g_lru.begin();
    while (g_cacheBytes > kCacheLimit && g_lru.size() > 1) {
        uint64_t old = g_lru.back(); g_lru.pop_back();
        auto it = g_cache.find(old);  // never operator[]: it would insert an entry with a dangling LRU link
        if (it == g_cache.end() || it->first == key) continue;
        g_cacheBytes -= it->second.px.size() * 4;
        g_cache.erase(it);
    }
    return slot.px.data();
}

// HD texture of a megatile, box-filtered down to the smallest power-of-two size that still has at
// least `want` texels per tile (zoomed-out views would otherwise alias badly). *size gets the edge.
const uint32_t *hdTexture(int variant, int ts, int mt, int want, int *size) {
    TexSet *t = texSet(variant, ts);
    if (!t || mt < 0 || (size_t)mt >= t->frameOff.size()) return nullptr;
    int level = 0, sz = t->size;
    while (level < 4 && sz / 2 >= want && sz / 2 >= 4) { sz /= 2; level++; }
    *size = sz;
    uint64_t key = (uint64_t)variant << 40 | (uint64_t)ts << 32 | (uint64_t)level << 24 | (uint64_t)mt;
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        g_lru.splice(g_lru.begin(), g_lru, it->second.lru);
        return it->second.px.data();
    }
    std::vector<uint32_t> px;
    if (level == 0) {
        size_t off = t->frameOff[mt];
        if (!decodeDds(t->data.data() + off, t->data.size() - off, t->size, px)) return nullptr;
        return cachePut(key, std::move(px));
    }
    int psz;
    const uint32_t *src = hdTexture(variant, ts, mt, sz * 2, &psz);
    if (!src || psz != sz * 2) return nullptr;
    px.resize((size_t)sz * sz);
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++) {
            const uint32_t *q = src + (y * 2) * psz + x * 2;
            uint32_t c[4] = {q[0], q[1], q[psz], q[psz + 1]}, out = 0;
            for (int sh = 0; sh < 24; sh += 8)
                out |= ((((c[0] >> sh) & 255) + ((c[1] >> sh) & 255) + ((c[2] >> sh) & 255) + ((c[3] >> sh) & 255) + 2) / 4) << sh;
            px[y * sz + x] = out;
        }
    return cachePut(key, std::move(px));
}

// ---- hooks --------------------------------------------------------------------------------------
using CopyFn = HRESULT(__stdcall *)(int *, int, int, uint8_t *, uint8_t *);
using StretchBltFn = BOOL(WINAPI *)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
using BitBltFn = BOOL(WINAPI *)(HDC, int, int, int, int, HDC, int, int, DWORD);
CopyFn g_origCopy;
StretchBltFn g_origStretchBlt;
BitBltFn g_origBitBlt;

// buffer -> the buffer it is composed from (learned from the block copies); root = terrain + grid
std::unordered_map<uint8_t *, uint8_t *> g_terrainFor;
// memory DC of every composed buffer -> its CRenderBuffer
std::unordered_map<HDC, uint8_t *> g_bufferForDC;

HRESULT __stdcall CopyHook(int *dirty, int a2, int a3, uint8_t *src, uint8_t *dst) {
    DrawLock lock;
    if (src && dst) {
        auto &slot = g_terrainFor[dst];
        if (slot != src) {
            logf("block copy %p (%dx%d fmt %d) -> %p (%dx%d fmt %d)", src, at<int>(src, B_W), at<int>(src, B_H), at<int>(src, B_FMT),
                 dst, at<int>(dst, B_W), at<int>(dst, B_H), at<int>(dst, B_FMT));
            slot = src;
        }
        if (HDC dc = at<HDC>(dst, B_HDC)) g_bufferForDC[dc] = dst;
    }
    hd_sprites::copyBlocks(dirty, (void *)a2, a3, src, dst);
    return g_origCopy(dirty, a2, a3, src, dst);
}

struct OutSurface { HDC dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr; uint32_t *px = nullptr; int w = 0, h = 0; } g_out;

bool ensureOut(int w, int h) {
    if (g_out.dc && g_out.w >= w && g_out.h >= h) return true;
    w = std::max(w, g_out.w); h = std::max(h, g_out.h);
    if (g_out.dc) { SelectObject(g_out.dc, g_out.old); DeleteObject(g_out.bmp); DeleteDC(g_out.dc); g_out = {}; }
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void *bits = nullptr;
    g_out.bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_out.bmp) return false;
    g_out.dc = CreateCompatibleDC(nullptr);
    g_out.old = SelectObject(g_out.dc, g_out.bmp);
    g_out.px = static_cast<uint32_t *>(bits); g_out.w = w; g_out.h = h;
    return true;
}

struct TileInfo { const uint32_t *tex; int size; const uint8_t *sd; };

double g_lastMs;

// Draw the magnified blit (dst rect <- back[xs..xs+ws, ys..ys+hs]) with HD terrain. Returns false to
// let ScmDraft's own StretchBlt run instead.
void failWhy(int why) {
    static int last = 0;
    if (why != last) { logf("composite skipped: reason %d", why); last = why; }
}

// When set, compositeBlit reads the backbuffer pixels from here (a copy with Test Mining sprites).
const uint8_t *g_bitsOverride;

bool compositeBlit(HDC hdcDst, int xd, int yd, int wd, int hd, uint8_t *back, int xs, int ys, int ws, int hs) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    // Buffers form a chain (terrain -> terrain+static layers -> backbuffer); the root is pure terrain.
    uint8_t *terr = nullptr;
    for (uint8_t *b = back; ; ) {
        auto ti = g_terrainFor.find(b);
        if (ti == g_terrainFor.end() || ti->second == back || ti->second == terr) break;
        terr = b = ti->second;
    }
    if (!terr) { failWhy(1); return false; }
    int bw = at<int>(back, B_W), bh = at<int>(back, B_H), bstride = at<int>(back, B_STRIDE);
    if (at<int>(back, B_FMT) != 1 || at<int>(terr, B_FMT) != 1 || at<int>(terr, B_W) != bw || at<int>(terr, B_H) != bh) { failWhy(2); return false; }
    const uint8_t *bpx = g_bitsOverride ? g_bitsOverride : at<uint8_t *>(back, B_BITS), *tpx = at<uint8_t *>(terr, B_BITS);
    const bool hdOn = g_mode != MODE_OFF;
    int tstride = at<int>(terr, B_STRIDE);
    if (!bpx || !tpx || wd <= 0 || hd <= 0 || ws <= 0 || hs <= 0) { failWhy(3); return false; }
    if (hdOn && !buildIndex()) { failWhy(4); return false; }

    RGBQUAD pal[256];
    if (GetDIBColorTable(at<HDC>(back, B_HDC), 0, 256, pal) != 256) { failWhy(6); return false; }
    const uint32_t *pal32 = reinterpret_cast<const uint32_t *>(pal);

    // Render buffers include padding and can be smaller than the requested zoomed-out
    // source rectangle. Neither their last pixel nor their padding extends the real map.
    auto *engine = back - E_BACKBUF_OFFSET;
    auto *map = at<uint8_t *>(engine, 0xb0);
    auto *chk = map ? at<uint8_t *>(map, 0x6fc) : nullptr;
    int mapW = chk ? at<int>(chk, 0x58) * 32 : bw;
    int mapH = chk ? at<int>(chk, 0x5c) * 32 : bh;
    auto validX = map_sampling::bounds(bw, at<int>(engine, 0x1280) * 32, mapW);
    auto validY = map_sampling::bounds(bh, at<int>(engine, 0x1284) * 32, mapH);

    float sx = (float)ws / wd, sy = (float)hs / hd;
    // only compose what is visible: ScmDraft's 50% path blits twice the window size, for instance
    int dx0 = 0, dy0 = 0, dx1 = wd, dy1 = hd;
    RECT cb;
    if (GetClipBox(hdcDst, &cb) != ERROR) {
        dx0 = std::max(0, (int)cb.left - xd); dx1 = std::min(wd, (int)cb.right - xd);
        dy0 = std::max(0, (int)cb.top - yd); dy1 = std::min(hd, (int)cb.bottom - yd);
    }
    if (dx1 <= dx0 || dy1 <= dy0) return true;
    if (!ensureOut(dx1 - dx0, dy1 - dy0)) { failWhy(5); return false; }
    // HD2 (2x) up to 200%, HD (4x) beyond
    // texels needed per 32px tile; HD (128px) beyond 64, otherwise HD2 (64px) and its mips
    int want = (int)ceilf(32.f / sx);
    int variant = (want > 64 ? 1 : 0) + (g_mode == MODE_CARBOT ? 2 : 0);

    // identify the megatile of every terrain block touched by the source rect
    int sx0 = (int)(xs + dx0 * sx), sy0 = (int)(ys + dy0 * sy), sx1 = (int)ceilf(xs + dx1 * sx), sy1 = (int)ceilf(ys + dy1 * sy);
    int tx0 = std::max(validX.first, sx0) >> 5, ty0 = std::max(validY.first, sy0) >> 5;
    int tx1 = (std::min(sx1, validX.last) + 31) >> 5, ty1 = (std::min(sy1, validY.last) + 31) >> 5;
    int tcols = tx1 - tx0, trows = ty1 - ty0;
    if (tcols <= 0 || trows <= 0) {
        RECT empty{xd + dx0, yd + dy0, xd + dx1, yd + dy1};
        FillRect(hdcDst, &empty, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return true;
    }
    std::vector<TileInfo> tiles(tcols * trows, TileInfo{nullptr, 0, nullptr});
    std::vector<const std::vector<TileRef> *> refs(tiles.size(), nullptr);
    int votes[8] = {};
    const auto *terrainSprites = hdOn ? hd_sprites::prepare(terr) : nullptr;
    uint8_t uncovered[1024];
    for (int ty = ty0; ty < ty1 && hdOn; ty++)
        for (int tx = tx0; tx < tx1; tx++) {
            const uint8_t *tilePixels = tpx + ty * 32 * tstride + tx * 32;
            int tilePitch = tstride;
            // A cached terrain layer may already contain static sprite images. Recover the
            // pre-sprite SD pixels before identifying its megatile, rather than hashing the art.
            if (terrainSprites && (terrainSprites->tile(tx * 32, ty * 32) || terrainSprites->tile(tx * 32 + 16, ty * 32) ||
                terrainSprites->tile(tx * 32, ty * 32 + 16) || terrainSprites->tile(tx * 32 + 16, ty * 32 + 16))) {
                for (int yy = 0; yy < 32; ++yy) for (int xx = 0; xx < 32; ++xx) {
                    int x = tx * 32 + xx, y = ty * 32 + yy;
                    uncovered[yy * 32 + xx] = hd_sprites::basePixel(terrainSprites->tile(x, y), x, y, tilePixels[yy * tstride + xx]);
                }
                tilePixels = uncovered; tilePitch = 32;
            }
            auto it = g_tileIndex.find(blockHash(tilePixels, tilePitch));
            if (it == g_tileIndex.end()) continue;
            refs[(ty - ty0) * tcols + tx - tx0] = &it->second;
            if (it->second.size() == 1) votes[it->second[0].ts]++;
        }
    int best = g_curTileset;
    for (int i = 0; i < 8; i++) if (best < 0 || votes[i] > votes[best]) best = i;
    if (best >= 0 && votes[best] > 0 && best != g_curTileset) { g_curTileset = best; logf("active tileset: %s", kTilesets[best]); }
    int hits = 0;
    for (size_t i = 0; i < tiles.size(); i++) {
        if (!refs[i]) continue;
        const TileRef *r = &(*refs[i])[0];
        for (auto &c : *refs[i]) if (c.ts == g_curTileset) r = &c;
        int sz = 0;
        if (const uint32_t *tex = hdTexture(variant, r->ts, r->mt, want, &sz)) { tiles[i] = {tex, sz, sdTile(r->ts, r->mt)}; hits++; }
    }

    // per-column / per-row sampling tables (10-bit sub-tile fractions)
    std::vector<int> colB(wd), colT(wd), colU(wd), rowB(hd), rowT(hd), rowV(hd);
    for (int dx = dx0; dx < dx1; dx++) {
        float fx = xs + (dx + 0.5f) * sx;
        int bx = validX.sample(fx);
        colB[dx] = bx;
        if (bx < 0) continue;
        colT[dx] = std::max(0, std::min((bx >> 5) - tx0, tcols - 1));
        colU[dx] = std::max(0, std::min(1023, (int)((fx - (bx & ~31)) * 32.f)));
    }
    for (int dy = dy0; dy < dy1; dy++) {
        float fy = ys + (dy + 0.5f) * sy;
        int by = validY.sample(fy);
        rowB[dy] = by;
        if (by < 0) continue;
        rowT[dy] = std::max(0, std::min((by >> 5) - ty0, trows - 1));
        rowV[dy] = std::max(0, std::min(1023, (int)((fy - (by & ~31)) * 32.f)));
    }
    const uint8_t *shadow = hdOn && g_curTileset >= 0 ? live::shadowRemap(g_curTileset) : nullptr;
    const auto *spriteSurface = hdOn ? hd_sprites::prepare(back) : nullptr;
    int spriteLevel = 0;
    while (spriteLevel < 4 && (4 >> spriteLevel) > 1.0f / sx) ++spriteLevel;
    int spriteSize = 64 >> spriteLevel;
    std::vector<int> spriteU(wd), spriteV(hd);
    for (int x = dx0; x < dx1; ++x) spriteU[x] = std::clamp(int((xs + (x + 0.5f) * sx - (colB[x] & ~15)) * spriteSize / 16), 0, spriteSize - 1);
    for (int y = dy0; y < dy1; ++y) spriteV[y] = std::clamp(int((ys + (y + 0.5f) * sy - (rowB[y] & ~15)) * spriteSize / 16), 0, spriteSize - 1);
    for (int dy = dy0; dy < dy1; dy++) {
        if (rowB[dy] < 0) {
            std::fill_n(g_out.px + (dy - dy0) * g_out.w, dx1 - dx0, 0u);
            continue;
        }
        const uint8_t *brow = bpx + rowB[dy] * bstride;
        int sdRow = (rowB[dy] & 31) * 32;
        uint32_t *o = g_out.px + (dy - dy0) * g_out.w;
        const TileInfo *trowTiles = &tiles[rowT[dy] * tcols];
        int v = rowV[dy];
        for (int dx = dx0; dx < dx1; dx++) {
            int bx = colB[dx];
            if (bx < 0) { o[dx - dx0] = 0; continue; }
            uint8_t actual = brow[bx];
            auto *spriteTile = spriteSurface ? spriteSurface->tile(bx, rowB[dy]) : nullptr;
            uint8_t b = hd_sprites::basePixel(spriteTile, bx, rowB[dy], actual);
            const TileInfo &t = trowTiles[colT[dx]];
            uint8_t terrainSD = t.sd ? t.sd[sdRow + (bx & 31)] : 0;
            bool darkened = shadow && b != terrainSD && b == shadow[terrainSD];
            if (t.tex && (b == terrainSD || darkened)) {
                int u = (colU[dx] * t.size) >> 10, vv = (v * t.size) >> 10;
                o[dx - dx0] = t.tex[vv * t.size + u];
                if (darkened) o[dx - dx0] = ((o[dx - dx0] & 0xfefefe) >> 1);
            } else {
                o[dx - dx0] = pal32[b] & 0xffffff;
            }
            if (spriteTile) o[dx - dx0] = hd_sprites::over(hd_sprites::pixel(spriteTile, bx, rowB[dy],
                spriteU[dx], spriteV[dy], spriteLevel, actual), o[dx - dx0]) & 0xffffff;
        }
    }
    BitBlt(hdcDst, xd + dx0, yd + dy0, dx1 - dx0, dy1 - dy0, g_out.dc, 0, 0, SRCCOPY);

    LARGE_INTEGER t1, fq; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    g_lastMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
    static int lastHits = -1, lastVariant = -1, lastWant = -1;
    if (hits != lastHits || variant != lastVariant || want != lastWant || getenv("SCMHD_DUMP")) {
        lastWant = want;
        logf("blit: %dx%d <- (%d,%d %dx%d) back %p (%dx%d) tiles %dx%d hd %d/%zu variant %d tex %d, %.1f ms%s", wd, hd, xs, ys, ws, hs, back, bw, bh, tcols, trows, hits, tiles.size(), variant, want, g_lastMs, g_bitsOverride ? " +live" : "");
        lastHits = hits; lastVariant = variant;
    }
    return true;
}

// ScmDraft's StretchBlt and BitBlt imports are patched. Copies out of a map render buffer onto a
// window are drawn by compositeBlit instead, so SD pixels never reach the window (drawing HD on top
// after ScmDraft's own blit flickered, and several repaint paths bypass CRenderEngine::Present).
// ScmDraft presents per zoom level: >100% and 66/33/25% StretchBlt the backbuffer, 100% BitBlts it,
// and 50% BitBlts a separate pre-shrunk half-size buffer (CRenderBuffer at engine+0xdd4, i.e.
// backbuffer+0x464; its HDC at backbuffer+0x490), whose coordinates are half the backbuffer's.
constexpr int kHalfBufferDC = 0xe00 - E_BACKBUF_OFFSET;

bool isMapWindow(HWND wnd) {
    char name[64] = "";
    return wnd && GetClassNameA(wnd, name, sizeof name) && !strcmp(name, "ScmDraft Map Window");
}

// ---- map-view blits: HD terrain, Test Mining sprites and overlays ---------------------------------
void bufferOrigin(uint8_t *back, int *ox, int *oy) {
    uint8_t *engine = back - E_BACKBUF_OFFSET;
    *ox = *reinterpret_cast<int *>(engine + 0x1280) * 32; *oy = *reinterpret_cast<int *>(engine + 0x1284) * 32;
}

LRESULT CALLBACK MapWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
std::unordered_map<HWND, WNDPROC> g_mapWndProcs;
void subclassMapWindow(HWND w) {
    if (!w || g_mapWndProcs.count(w)) return;
    g_mapWndProcs[w] = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MapWndProc)));
}

// The last map-view blit, so the Test Mining timer can redraw between ScmDraft's own repaints.
struct LastBlit { HWND wnd = nullptr; int xd, yd, wd, hd; uint8_t *back = nullptr; double xs, ys, ws, hs; } g_last;

// Draws a map-view blit ourselves (dst rect <- full-res backbuffer rect). Returns false to fall back.
bool drawMapBlit(HDC dst, int xd, int yd, int wd, int hd, uint8_t *back, double xs, double ys, double ws, double hs) {
    bool liveRunning = live::running();
    bool ok = false;
    if (g_mode != MODE_OFF || liveRunning) {
        static std::vector<uint8_t> scratch;
        if (liveRunning) {  // a copy of the backbuffer with the simulated units drawn in
            int stride = at<int>(back, B_STRIDE), bh = at<int>(back, B_H);
            const uint8_t *bits = at<uint8_t *>(back, B_BITS);
            scratch.assign(bits, bits + (size_t)stride * bh);
            int ox, oy; bufferOrigin(back, &ox, &oy);
            hd_sprites::beginLive(back);
            live::drawSprites(back, scratch.data(), stride, at<int>(back, B_W), bh, ox, oy);
            g_bitsOverride = scratch.data();
        }
        ok = compositeBlit(dst, xd, yd, wd, hd, back, (int)xs, (int)ys, (int)ws, (int)hs);
        g_bitsOverride = nullptr;
        hd_sprites::endLive();
    }
    return ok;
}

void afterMapBlit(HDC dst, HWND wnd, int xd, int yd, int wd, int hd, uint8_t *back, double xs, double ys, double ws, double hs) {
    int ox, oy; bufferOrigin(back, &ox, &oy);
    hitboxes::draw(dst, scmhdActiveMap(), xd, yd, wd, hd, xs, ys, ws, hs, ox, oy);
    mining::drawOverlay(dst, xd, yd, wd, hd, xs, ys, ws, hs, ox, oy, 1.0);
    live::drawUI(dst, wnd, xd, yd, wd, hd, xs, ys, ws, hs, ox, oy);
    g_last = {wnd, xd, yd, wd, hd, back, xs, ys, ws, hs};
    subclassMapWindow(wnd);
}

void redrawLast() {
    DrawLock lock;
    if (!g_last.wnd || !IsWindow(g_last.wnd) || !g_last.back) return;
    HDC dc = GetDC(g_last.wnd);
    if (!dc) return;
    if (drawMapBlit(dc, g_last.xd, g_last.yd, g_last.wd, g_last.hd, g_last.back, g_last.xs, g_last.ys, g_last.ws, g_last.hs))
        afterMapBlit(dc, g_last.wnd, g_last.xd, g_last.yd, g_last.wd, g_last.hd, g_last.back, g_last.xs, g_last.ys, g_last.ws, g_last.hs);
    ReleaseDC(g_last.wnd, dc);
}

BOOL WINAPI StretchBltHook(HDC hdcDst, int xd, int yd, int wd, int hd, HDC hdcSrc, int xs, int ys, int ws, int hs, DWORD rop) {
    DrawLock lock;
    uint8_t *back = nullptr;
    HWND wnd = WindowFromDC(hdcDst);
    if (isMapWindow(wnd) && rop == SRCCOPY) { auto it = g_bufferForDC.find(hdcSrc); if (it != g_bufferForDC.end()) back = g_activeBackbuffer = it->second; }
    BOOL r = (back && drawMapBlit(hdcDst, xd, yd, wd, hd, back, xs, ys, ws, hs)) ? TRUE
             : g_origStretchBlt(hdcDst, xd, yd, wd, hd, hdcSrc, xs, ys, ws, hs, rop);
    if (back) afterMapBlit(hdcDst, wnd, xd, yd, wd, hd, back, xs, ys, ws, hs);
    return r;
}

BOOL WINAPI BitBltHook(HDC hdcDst, int xd, int yd, int w, int h, HDC hdcSrc, int xs, int ys, DWORD rop) {
    DrawLock lock;
    uint8_t *back = nullptr; bool half = false;
    HWND wnd = hdcSrc && rop == SRCCOPY ? WindowFromDC(hdcDst) : nullptr;
    if (isMapWindow(wnd)) {
        auto it = g_bufferForDC.find(hdcSrc);
        if (it != g_bufferForDC.end()) back = it->second;
        else for (auto &kv : g_bufferForDC) if (at<HDC>(kv.second, kHalfBufferDC) == hdcSrc) { back = kv.second; half = true; break; }
        if (back) g_activeBackbuffer = back;
    }
    if (!back) return g_origBitBlt(hdcDst, xd, yd, w, h, hdcSrc, xs, ys, rop);
    // the 50% view blits from a half-size copy; work in full-resolution backbuffer coordinates
    double k = half ? 2.0 : 1.0;
    BOOL r = drawMapBlit(hdcDst, xd, yd, w, h, back, xs * k, ys * k, w * k, h * k) ? TRUE
             : g_origBitBlt(hdcDst, xd, yd, w, h, hdcSrc, xs, ys, rop);
    afterMapBlit(hdcDst, wnd, xd, yd, w, h, back, xs * k, ys * k, w * k, h * k);
    return r;
}

bool mirroredCopyKey(UINT msg, WPARAM key) {
    return msg == WM_KEYDOWN && key == 'C' && (GetKeyState(VK_CONTROL) & 0x8000) &&
           (GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000);
}

// CMainGUI::PreTranslateMessage (0x50f7d0) translates accelerators before dispatching to
// MapWndProc. Catch our chord here so ScmDraft cannot turn it into an ordinary Copy first.
using TranslateAcceleratorFn = int (WINAPI *)(HWND, HACCEL, LPMSG);
TranslateAcceleratorFn g_origTranslateAccelerator;
int WINAPI TranslateAcceleratorHook(HWND mainWnd, HACCEL accel, LPMSG msg) {
    if (msg && mirroredCopyKey(msg->message, msg->wParam) && GetAncestor(msg->hwnd, GA_ROOT) == mainWnd) {
        wchar_t cls[64] = L"";
        GetClassNameW(msg->hwnd, cls, 64);
        if (!lstrcmpiW(cls, L"ScmDraft Map Window")) {
            // Consume repeats too, but only copy once per press.
            if (!(msg->lParam & (1L << 30))) {
                logf("mirror: Ctrl+Shift+C accelerator");
                sym::requestMirroredCopy(mainWnd, scmhdActiveMap());
            }
            return 1;
        }
    }
    return g_origTranslateAccelerator(mainWnd, accel, msg);
}

// Map window: clicks on Test Mining buttons are ours and must not reach ScmDraft (it would paint).
LRESULT CALLBACK MapWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static bool swallowUp;
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && live::onClick(h, x, y)) { swallowUp = true; redrawLast(); return 0; }
    if (msg == WM_LBUTTONUP && swallowUp) { swallowUp = false; return 0; }
    if (mirroredCopyKey(msg, wp)) {
        if (!(lp & (1L << 30))) sym::requestMirroredCopy(GetAncestor(h, GA_ROOT), scmhdActiveMap());
        return 0;
    }
    // keep ScmDraft's brush preview from following the mouse over our controls
    if (msg == WM_MOUSEMOVE && live::overButton(h, x, y)) return 0;
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        if (live::overButton(h, pt.x, pt.y)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
    }
    auto it = g_mapWndProcs.find(h);
    return it != g_mapWndProcs.end() ? CallWindowProcW(it->second, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

// Point the exe's import of module!name at hook; returns the original.
void *patchImport(const char *module, const char *name, void *hook) {
    auto *base = reinterpret_cast<uint8_t *>(GetModuleHandleA(nullptr));
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA(reinterpret_cast<char *>(base + imp->Name), module)) continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk);
        auto *iat = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto *ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char *>(ibn->Name), name)) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            void *orig = reinterpret_cast<void *>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<uintptr_t>(hook);
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return orig;
        }
    }
    return nullptr;
}

bool installHooks() {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(GetModuleHandleA(nullptr));
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(dos) + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kExpectedTimestamp) {
        logf("unexpected ScmDraft build (timestamp %08lx); HD rendering disabled", nt->FileHeader.TimeDateStamp);
        return false;
    }
    if (MH_Initialize() != MH_OK) return false;
    bool ok = MH_CreateHook(reinterpret_cast<void *>(kCopyBlocks), reinterpret_cast<void *>(&CopyHook), reinterpret_cast<void **>(&g_origCopy)) == MH_OK &&
              sym::install() && hd_sprites::install() &&
              MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    g_origStretchBlt = reinterpret_cast<StretchBltFn>(patchImport("GDI32.dll", "StretchBlt", reinterpret_cast<void *>(&StretchBltHook)));
    g_origBitBlt = reinterpret_cast<BitBltFn>(patchImport("GDI32.dll", "BitBlt", reinterpret_cast<void *>(&BitBltHook)));
    g_origTranslateAccelerator = reinterpret_cast<TranslateAcceleratorFn>(patchImport("USER32.dll", "TranslateAcceleratorW", reinterpret_cast<void *>(&TranslateAcceleratorHook)));
    ok = ok && g_origStretchBlt && g_origBitBlt && g_origTranslateAccelerator;
    logf("hooks %s", ok ? "installed" : "FAILED");
    return ok;
}

} // namespace

int scmhdGraphicsMode() { return g_mode; }
void scmhdLinkBuffer(void *source, void *destination) {
    auto *src = static_cast<uint8_t *>(source), *dst = static_cast<uint8_t *>(destination);
    if (!src || !dst || src == dst || at<int>(src, B_FMT) != 1 || at<int>(dst, B_FMT) != 1 ||
        at<int>(src, B_W) != at<int>(dst, B_W) || at<int>(src, B_H) != at<int>(dst, B_H)) return;
    auto *p = src;
    for (int n = 0; n < 16; ++n) {
        if (p == dst) return;
        auto it = g_terrainFor.find(p); if (it == g_terrainFor.end()) break; p = it->second;
    }
    if (g_terrainFor[dst] != src) {
        g_terrainFor[dst] = src;
        logf("full buffer link %p -> %p", src, dst);
    }
    if (HDC dc = at<HDC>(dst, B_HDC)) g_bufferForDC[dc] = dst;
}
void scmhdForgetBuffer(void *buffer) {
    auto *p = static_cast<uint8_t *>(buffer);
    for (auto it = g_terrainFor.begin(); it != g_terrainFor.end();) {
        if (it->first == p || it->second == p) it = g_terrainFor.erase(it); else ++it;
    }
    for (auto it = g_bufferForDC.begin(); it != g_bufferForDC.end();) {
        if (it->second == p) it = g_bufferForDC.erase(it); else ++it;
    }
    if (g_activeBackbuffer == p) g_activeBackbuffer = nullptr;
    if (g_last.back == p) g_last = {};
}

// ---- menu handling ------------------------------------------------------------------------------
namespace {
const char *kModeNames[3] = {"HD Graphics: Off", "HD Graphics: Remastered", "HD Graphics: Carbot"};
HWND g_mainWnd;
WNDPROC g_origWndProc;

void setMode(int m) {
    uint8_t *engine = nullptr;
    {
        DrawLock lock;
        g_mode = m; hd_sprites::reset();
        if (g_activeBackbuffer) engine = g_activeBackbuffer - E_BACKBUF_OFFSET;
        saveMode();
    }
    // CRenderEngine::InvalidateTerrain sets +0x12d0. Rebuild static sprite caches too, so
    // switching art styles cannot leave the previous style baked into copied blocks.
    if (engine) {
        reinterpret_cast<HRESULT(__fastcall *)(void *, void *)>(0x44d870)(engine, nullptr);
        reinterpret_cast<HRESULT(__fastcall *)(void *, void *, int)>(0x44d930)(engine, nullptr, 1);
    }
    logf("mode -> %d", g_mode);
    if (g_mainWnd) RedrawWindow(g_mainWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

int modeForText(const char *t) {
    for (int m = 0; m < 3; m++) if (!strcmp(t, kModeNames[m])) return m;
    return -1;
}

LRESULT CALLBACK MainWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // Join search threads on the UI thread, before DLL teardown takes the loader lock.
    if (msg == WM_DESTROY && live::enabled()) live::setEnabled(false, h);
    if (msg == WM_COMMAND && HIWORD(wp) == 0 && !lp) {
        char t[64] = "";
        if (GetMenuStringA(GetMenu(h), LOWORD(wp), t, sizeof t, MF_BYCOMMAND) > 0) {
            int m = modeForText(t);
            if (m >= 0) { setMode(m); return 0; }
            if (!strcmp(t, kBuildingBoxesText)) { hitboxes::toggle(h, true); redrawLast(); return 0; }
            if (!strcmp(t, kUnitBoxesText)) { hitboxes::toggle(h, false); redrawLast(); return 0; }
            if (!strcmp(t, kLiveMenuText)) { live::setEnabled(!live::enabled(), h); return 0; }
            if (!strcmp(t, kMirrorMenuText)) { sym::requestMirroredCopy(h, scmhdActiveMap()); return 0; }
            if (!strcmp(t, kUnitSpriteMenuText)) { sym::replaceUnitsBySprites(h, scmhdActiveMap()); return 0; }
        }
    } else if (msg == WM_TIMER && wp == hitboxes::timerId()) {
        hitboxes::update();
        redrawLast();
        return 0;
    } else if (msg == WM_TIMER && wp == live::timerId()) {
        if (live::onTimer()) redrawLast();
        return 0;
    } else if (msg == WM_INITMENUPOPUP) {
        HMENU menu = reinterpret_cast<HMENU>(wp);
        for (int i = 0, n = GetMenuItemCount(menu); i < n; i++) {
            char t[64] = "";
            if (GetMenuStringA(menu, i, t, sizeof t, MF_BYPOSITION) > 0) {
                int m = modeForText(t);
                if (m >= 0) CheckMenuItem(menu, i, MF_BYPOSITION | (m == g_mode ? MF_CHECKED : MF_UNCHECKED));
                if (!strcmp(t, kBuildingBoxesText)) CheckMenuItem(menu, i, MF_BYPOSITION | (hitboxes::enabled(true) ? MF_CHECKED : MF_UNCHECKED));
                if (!strcmp(t, kUnitBoxesText)) CheckMenuItem(menu, i, MF_BYPOSITION | (hitboxes::enabled(false) ? MF_CHECKED : MF_UNCHECKED));
                if (!strcmp(t, kLiveMenuText)) CheckMenuItem(menu, i, MF_BYPOSITION | (live::enabled() ? MF_CHECKED : MF_UNCHECKED));
            }
        }
    }
    return CallWindowProcW(g_origWndProc, h, msg, wp, lp);
}

void subclassMain(HWND h) {
    if (!h || g_origWndProc) return;
    g_mainWnd = h;
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainWndProc)));
    logf("subclassed main window %p", h);
}
} // namespace

// ---- ScmDraft plugin API (version 2) ------------------------------------------------------------
extern "C" {
__declspec(dllexport) DWORD __stdcall GetPluginVersion() { return 2; }

__declspec(dllexport) BOOL __stdcall InitPlugin(HWND mainWnd, HINSTANCE, void *, void *, void *, DWORD *menuIds) {
    // ScmDraft frees plugins inside the original main-window destroy callback. Our subclass
    // still has to return through this DLL afterward, and its hooks remain installed. Keep
    // their code alive until process exit rather than unloading under an active callback.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&InitPlugin), &pinned)) return FALSE;
    menuIds[0] = kMenuOff; menuIds[1] = kMenuHD; menuIds[2] = kMenuCarbot; menuIds[3] = kMenuLive; menuIds[4] = kMenuMirror; menuIds[5] = kMenuUnitSprite; menuIds[6] = kMenuBuildingBoxes; menuIds[7] = kMenuUnitBoxes;
    mining::init(g_self, mainWnd);
    hitboxes::init(mainWnd);
    subclassMain(mainWnd);
    return TRUE;
}

__declspec(dllexport) BOOL __stdcall PluginGetMenuString(DWORD id, char *buf, WORD len) {
    const char *s = id == kMenuOff ? kModeNames[MODE_OFF] : id == kMenuHD ? kModeNames[MODE_HD] : id == kMenuCarbot ? kModeNames[MODE_CARBOT]
                  : id == kMenuLive ? kLiveMenuText : id == kMenuMirror ? kMirrorMenuText
                  : id == kMenuUnitSprite ? kUnitSpriteMenuText : id == kMenuBuildingBoxes ? kBuildingBoxesText
                  : id == kMenuUnitBoxes ? kUnitBoxesText : nullptr;
    if (!s) return FALSE;
    lstrcpynA(buf, s, len);
    return TRUE;
}

// Normally unreachable: the subclassed main window handles our menu commands first, because going
// through RunPlugin makes ScmDraft re-import the trigger sections and mark the map as modified.
__declspec(dllexport) BOOL __stdcall RunPlugin(void *, DWORD id, void *, void *, void *, void *, DWORD) {
    if (id == kMenuBuildingBoxes || id == kMenuUnitBoxes) { hitboxes::toggle(g_mainWnd, id == kMenuBuildingBoxes); return TRUE; }
    if (id == kMenuLive) { live::setEnabled(!live::enabled(), g_mainWnd); return TRUE; }
    if (id == kMenuMirror) { sym::requestMirroredCopy(g_mainWnd, scmhdActiveMap()); return TRUE; }
    if (id == kMenuUnitSprite) { sym::replaceUnitsBySprites(g_mainWnd, scmhdActiveMap()); return TRUE; }
    setMode(id == kMenuOff ? MODE_OFF : id == kMenuCarbot ? MODE_CARBOT : MODE_HD);
    return TRUE;
}
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_drawLock);
        g_self = h;
        DisableThreadLibraryCalls(h);
        char path[MAX_PATH];
        GetModuleFileNameA(h, path, MAX_PATH);
        strcpy(strrchr(path, '.'), ".log");
        g_log = fopen(path, "w");
        loadMode();
        logf("scmhd loaded, mode %d", g_mode);
        installHooks();
    }
    return TRUE;
}
