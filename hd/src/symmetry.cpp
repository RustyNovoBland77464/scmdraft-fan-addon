// Extra terrain symmetry for ScmDraft 2 (0.9.10), part of scmhd.sdp.
//
// ScmDraft's Terrain Symmetry panel offers Mirror (X / Y / XY) and N-fold Tile/Isom rotation. This adds:
//  * Mirror-Diag (\) and Mirror-Diag (/): reflections across the map diagonals, in normalized map
//    space so they also work on non-square maps. Combined with X/Y they close to the full 8-way
//    (dihedral D4) kaleidoscope.
//  * "+ Mirror" for the rotation modes: every rotated copy is mirrored as well (dihedral D_N).
//  * "Balance walkability": isometric terrain only has clean edges along the diamond axes, so a copy
//    that is diagonal-mirrored or rotated comes out as staircases whose cliffs eat into the paths,
//    and one side ends up tighter. With balancing on, every symmetric copy of an isometric brush is
//    tried at the snapped diamond and its 8 neighbours; each trial runs ScmDraft's own ISOM and tile
//    algorithms in a sandbox, and the copy whose 8x8 walkability best matches the transformed source
//    is placed for real.
//
// How (see ../../re/NOTES.md):
//  * 0x486570  TerrainEditor::GetMirrorCoords(x, y, outX[], outY[], &count) returns at most 3 copies
//    (mirror flags at this+0x88..0x8a). We return the whole generated group instead: the first 3 the
//    normal way, and the rest are queued.
//  * 0x488700  TerrainEditor::PlaceTerrain(p1, brush, x, y): one paint step. It places the brush via
//    0x488360 PlaceAt(brush, x, y, rotSteps), then the mirror copies the same way.
//  * 0x4868d0  PlaceDiamond(a, b) = ISOM 0x449f10 on the ISOM object at editor+0x10 (grid at *[0],
//    8 bytes per cell; dirty rect at [3..6]) + 0x448a80 tile regeneration, which writes tiles via
//    0x486e40 SetTile(x, y, tile) (creates TerrainUndoNodes) and reads them via 0x49f310 GetTile.
//    Sandbox = snapshot the ISOM grid, redirect SetTile/GetTile to a shadow overlay, restore.
//  * 0x453170  CRenderEngine::DrawBrushAt (preview) gets the same extra copies (not the balancing).
//  * 0x501570  SymmetryPanel::Resize(hwnd, expanded, mode) sizes the panel to its last control. We
//    add our checkboxes and grow the panel to fit them.
#include <windows.h>
#include "host_image.h"
#include <commctrl.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <array>
#include "MinHook.h"

void *scmhdActiveMap();
void logf(const char *fmt, ...);
bool scmhdCascRead(const std::string &name, std::vector<uint8_t> &out);

namespace sym {

template <class T> T &at(void *p, int off) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off); }

constexpr uintptr_t kMirrorCoords = 0x00486570, kPlaceAt = 0x00488360, kDrawBrushAt = 0x00453170,
                    kPanelResize = 0x00501570, kPlaceTerrain = 0x00488700, kPlaceDiamond = 0x004868d0,
                    kPlaceRect = 0x00487e60, kSetTile = 0x00486e40, kGetTile = 0x0049f310, kSnapIso = 0x00486000, kRotate = 0x00486640;
constexpr int ED_MAP = 0xc, ED_ISOM = 0x10, ED_KIND = 0x68, ED_TYPE = 0x70, MAP_CHK = 0x6fc, CHK_W = 0x58, CHK_H = 0x5c,
              CHK_TILESET = 0x60;
constexpr int ENG_MAP = 0xb0, MAP_TYPE = 0xb0;  // the render engine sees the type through the map object
constexpr int PANEL_DLG = 0x20;
constexpr int ID_MIRROR_Y = 0x500, ID_MIRROR_XY = 0x501, ID_SKEW = 0x471;
constexpr int ID_DIAG1 = 0x7A01, ID_DIAG2 = 0x7A02, ID_ROTMIRROR = 0x7A03, ID_BALANCE = 0x7A04, ID_COPY_TERRAIN = 0x7A05;

bool g_diag1, g_diag2, g_rotMirror, g_balance = true;

void loadFlags() {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\ScmDraft HD", "Symmetry", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) {
        g_diag1 = v & 1; g_diag2 = v & 2; g_rotMirror = v & 4;
    }
    sz = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\ScmDraft HD", "SymmetryBalance", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        g_balance = v != 0;
}
void saveFlags() {
    DWORD v = (g_diag1 ? 1 : 0) | (g_diag2 ? 2 : 0) | (g_rotMirror ? 4 : 0), b = g_balance;
    RegSetKeyValueA(HKEY_CURRENT_USER, "Software\\ScmDraft HD", "Symmetry", REG_DWORD, &v, sizeof v);
    RegSetKeyValueA(HKEY_CURRENT_USER, "Software\\ScmDraft HD", "SymmetryBalance", REG_DWORD, &b, sizeof b);
}

// ---- symmetry group ---------------------------------------------------------------------------
// Elements are 2x2 signed permutation matrices acting on centered, normalized coordinates in [-1, 1].
struct M2 { int a, b, c, d; };  // (u, v) -> (a*u + b*v, c*u + d*v)
M2 mul(M2 p, M2 q) { return {p.a * q.a + p.b * q.c, p.a * q.b + p.b * q.d, p.c * q.a + p.d * q.c, p.c * q.b + p.d * q.d}; }
bool eq(M2 p, M2 q) { return p.a == q.a && p.b == q.b && p.c == q.c && p.d == q.d; }
constexpr M2 kMirrorX = {-1, 0, 0, 1};

std::vector<M2> group(bool mx, bool my, bool mxy, bool d1, bool d2) {
    std::vector<M2> gens;
    if (mx) gens.push_back(kMirrorX);
    if (my) gens.push_back({1, 0, 0, -1});
    if (mxy) gens.push_back({-1, 0, 0, -1});
    if (d1) gens.push_back({0, 1, 1, 0});
    if (d2) gens.push_back({0, -1, -1, 0});
    std::vector<M2> g = {{1, 0, 0, 1}};
    for (size_t i = 0; i < g.size(); i++)
        for (M2 s : gens) {
            M2 n = mul(s, g[i]);
            bool seen = false;
            for (M2 e : g) seen |= eq(e, n);
            if (!seen) g.push_back(n);
        }
    g.erase(g.begin());  // drop identity (the brush itself)
    return g;
}

// Pixel transform; matches ScmDraft's own mirror (x' = W-1-x) for the axis-aligned elements.
void apply(M2 m, int x, int y, int w, int h, int *ox, int *oy) {
    double u = (x + 0.5) / w * 2 - 1, v = (y + 0.5) / h * 2 - 1;
    double u2 = m.a * u + m.b * v, v2 = m.c * u + m.d * v;
    int nx = (int)((u2 + 1) / 2 * w), ny = (int)((v2 + 1) / 2 * h);
    *ox = nx < 0 ? 0 : nx >= w ? w - 1 : nx;
    *oy = ny < 0 ? 0 : ny >= h ? h - 1 : ny;
}

bool mapSizePx(void *map, int *w, int *h) {
    if (!map) return false;
    void *chk = at<void *>(map, MAP_CHK);
    if (!chk) return false;
    *w = at<int>(chk, CHK_W) * 32; *h = at<int>(chk, CHK_H) * 32;
    return *w > 0 && *h > 0;
}

// Copy choices are scoped to tileset and brush kind, with independent ISOM/rectangular choices.
// Keep them for this editor session; changing maps cannot apply another tileset's terrain ID.
struct CopyTerrain { int id = -1; std::wstring name; };
CopyTerrain g_copyTerrain[8][2];
CopyTerrain *copyTerrain(void *ed) {
    int kind = at<int>(ed, ED_KIND);
    void *map = at<void *>(ed, ED_MAP);
    void *chk = map ? at<void *>(map, MAP_CHK) : nullptr;
    int ts = chk ? at<uint16_t>(chk, 0x64) : -1;
    return ts >= 0 && ts < 8 && kind >= 0 && kind < 2 ? &g_copyTerrain[ts][kind] : nullptr;
}
bool alternateTerrain(void *ed) {
    CopyTerrain *choice = copyTerrain(ed);
    return choice && choice->id >= 0;
}

// ---- original functions -------------------------------------------------------------------------
using MirrorFn = HRESULT(__fastcall *)(void *, void *, int, int, int *, int *, unsigned *);
using PlaceFn = HRESULT(__fastcall *)(void *, void *, void *, int, int, unsigned);
using DrawFn = HRESULT(__fastcall *)(void *, void *, void *, void *, int, int, int);
using ResizeFn = void(__fastcall *)(void *, void *, HWND, char, int);
using PlaceTerrainFn = void(__fastcall *)(void *, void *, int, void *, int, int);
using PlaceDiamondFn = HRESULT(__fastcall *)(void *, void *, int, int);
using SetTileFn = HRESULT(__fastcall *)(void *, void *, int, int, int);
using GetTileFn = uint32_t(__fastcall *)(void *, void *, int, int);
using SnapFn = HRESULT(__fastcall *)(void *, void *, int, int, int *, int *);
using RotateFn = HRESULT(__fastcall *)(void *, void *, int, int, unsigned, unsigned, int *, int *);
MirrorFn g_origMirror;
PlaceFn g_origPlace;
DrawFn g_origDraw;
ResizeFn g_origResize;
PlaceTerrainFn g_origPlaceTerrain;
PlaceDiamondFn g_origPlaceDiamond;
using PlaceRectFn = HRESULT(__fastcall *)(void *, void *, int, int, int, int);
PlaceRectFn g_origPlaceRect;
SetTileFn g_origSetTile;
GetTileFn g_origGetTile;
const SnapFn kSnap = host_image::pointer<SnapFn>(kSnapIso);
const RotateFn kRot = host_image::pointer<RotateFn>(kRotate);

// ---- walkability (cv5 + vf4) --------------------------------------------------------------------
const char *kTilesets[8] = {"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"};
struct Walk { bool tried = false, ok = false; std::vector<uint8_t> cv5, vf4; } g_walk[8];

Walk *walkData(int ts) {
    if (ts < 0 || ts > 7) return nullptr;
    Walk &w = g_walk[ts];
    if (!w.tried) {
        w.tried = true;
        w.ok = scmhdCascRead(std::string("TileSet\\") + kTilesets[ts] + ".cv5", w.cv5) &&
               scmhdCascRead(std::string("TileSet\\") + kTilesets[ts] + ".vf4", w.vf4);
        logf("balance: walkability data for %s %s", kTilesets[ts], w.ok ? "loaded" : "MISSING");
    }
    return w.ok ? &w : nullptr;
}

// minitile flags (vf4: bit0 walkable, bit1 mid, bit2 high) for a MTXM value
inline uint16_t miniFlags(const Walk &w, uint16_t tile, int mx, int my) {
    size_t entry = (size_t)(tile >> 4) * 52 + 20 + (tile & 15) * 2;
    if (entry + 2 > w.cv5.size()) return 0;
    uint16_t mega = w.cv5[entry] | w.cv5[entry + 1] << 8;
    size_t f = ((size_t)mega * 16 + my * 4 + mx) * 2;
    if (f + 2 > w.vf4.size()) return 0;
    return w.vf4[f] | w.vf4[f + 1] << 8;
}

// Tileset of the map (SC ERA): low word of CHK+0x64 (0 badlands .. 7 twilight).
int tilesetId(void *chk) {
    int ts = at<uint16_t>(chk, 0x64);
    return ts >= 0 && ts < 8 ? ts : -1;
}

// ---- sandboxed placement -----------------------------------------------------------------------
void *g_terrainObj;   // MapTerrain (this of GetTile), captured from ScmDraft's own calls
bool g_trial;          // SetTile/GetTile go to the shadow overlay...
DWORD g_trialOwner;    // ...but only for the thread running the trial (ScmDraft renders on another one)
void *g_lastEditor;    // TerrainEditor seen by PlaceTerrain (editor+0xc = map)
inline bool inTrial() { return g_trial && GetCurrentThreadId() == g_trialOwner; }
std::unordered_map<uint32_t, uint16_t> g_shadow;
bool g_recordSource;   // record tiles the primary brush changes
std::unordered_set<uint32_t> g_source;
inline uint32_t tkey(int x, int y) { return (uint32_t)x << 16 | (uint32_t)(y & 0xffff); }

HRESULT __fastcall SetTileHook(void *ed, void *edx, int x, int y, int tile) {
    if (inTrial()) { g_shadow[tkey(x, y)] = (uint16_t)tile; return S_OK; }
    if (g_recordSource) g_source.insert(tkey(x, y));
    return g_origSetTile(ed, edx, x, y, tile);
}

uint32_t __fastcall GetTileHook(void *terrain, void *edx, int x, int y) {
    g_terrainObj = terrain;
    if (inTrial()) {
        auto it = g_shadow.find(tkey(x, y));
        if (it != g_shadow.end()) return it->second;
    }
    return g_origGetTile(terrain, edx, x, y);
}

uint16_t tileAt(int x, int y, bool withShadow) {
    if (withShadow) {
        auto it = g_shadow.find(tkey(x, y));
        if (it != g_shadow.end()) return it->second;
    }
    return g_terrainObj ? (uint16_t)g_origGetTile(g_terrainObj, nullptr, x, y) : 0;
}

struct Ctx {
    void *ed = nullptr; Walk *walk = nullptr;
    int wt = 0, ht = 0, wpx = 0, hpx = 0;  // map size in tiles / pixels
};

bool makeCtx(void *ed, Ctx &c) {
    void *map = at<void *>(ed, ED_MAP);
    if (!map || !g_terrainObj) return false;
    void *chk = at<void *>(map, MAP_CHK);
    if (!chk) return false;
    c.ed = ed; c.wt = at<int>(chk, CHK_W); c.ht = at<int>(chk, CHK_H); c.wpx = c.wt * 32; c.hpx = c.ht * 32;
    c.walk = walkData(tilesetId(chk));
    return c.walk && c.wt > 0 && c.ht > 0;
}

// A pixel transform from the source brush to one copy.
struct Xform {
    enum { MIRROR, ROT } kind = MIRROR;
    M2 m{1, 0, 0, 1};
    bool preMirror = false; unsigned step = 0, steps = 1;
    void operator()(const Ctx &c, int x, int y, int *ox, int *oy) const {
        if (kind == MIRROR) { apply(m, x, y, c.wpx, c.hpx, ox, oy); return; }
        if (preMirror) apply(kMirrorX, x, y, c.wpx, c.hpx, &x, &y);
        if (kRot(c.ed, nullptr, x, y, step, steps, ox, oy) < 0) { *ox = x; *oy = y; }
    }
};

inline int walkBit(const Ctx &c, int mx, int my, bool shadow) {  // minitile coords (8px)
    if (mx < 0 || my < 0 || mx >= c.wt * 4 || my >= c.ht * 4) return -1;
    return miniFlags(*c.walk, tileAt(mx >> 2, my >> 2, shadow), mx & 3, my & 3) & 1;
}

// Clearance: for every 8x8 cell, chamfer (3-4) distance to the nearest unwalkable cell, in thirds of
// a cell, capped at kDtCap cells. Equal clearance on both sides means equal corridor and choke
// widths, which is what matters for fairness even where the cliff shapes cannot match exactly
// (isometric staircases vs clean diamond edges).
constexpr int kDtCap = 16, kDtMax = kDtCap * 3;
struct Grid {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<uint8_t> d;
    int at(int x, int y) const {
        x -= x0; y -= y0;
        return x < 0 || y < 0 || x >= w || y >= h ? -1 : d[y * w + x];
    }
};

void clearance(const Ctx &c, int x0, int y0, int x1, int y1, bool shadow, Grid &g) {
    // pad so distances near the window edge see the obstacles just outside it
    x0 -= kDtCap; y0 -= kDtCap; x1 += kDtCap; y1 += kDtCap;
    g.x0 = x0; g.y0 = y0; g.w = x1 - x0; g.h = y1 - y0;
    g.d.assign((size_t)g.w * g.h, kDtMax);
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++)
            if (walkBit(c, x0 + x, y0 + y, shadow) != 1) g.d[y * g.w + x] = 0;  // off-map counts as blocked
    auto relax = [&](int i, int j, int cost) { int v = g.d[j] + cost; if (v < g.d[i]) g.d[i] = (uint8_t)v; };
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++) {
            int i = y * g.w + x;
            if (x) relax(i, i - 1, 3);
            if (y) { relax(i, i - g.w, 3); if (x) relax(i, i - g.w - 1, 4); if (x + 1 < g.w) relax(i, i - g.w + 1, 4); }
        }
    for (int y = g.h - 1; y >= 0; y--)
        for (int x = g.w - 1; x >= 0; x--) {
            int i = y * g.w + x;
            if (x + 1 < g.w) relax(i, i + 1, 3);
            if (y + 1 < g.h) { relax(i, i + g.w, 3); if (x + 1 < g.w) relax(i, i + g.w + 1, 4); if (x) relax(i, i + g.w - 1, 4); }
        }
}

struct Stats { int copies = 0, moved = 0, eroded = 0; } g_stats;
struct Tuning { bool chokeWeight = true, dilate = false; int rounds = 6, cands = 32, margin = 4; } g_tune;  // best of a 15-run comparison
// clearance difference; mismatches where paths are narrow (chokes) count more than in open ground
inline long mismatch(int ds, int dd) {
    long d = std::abs(ds - dd);
    return g_tune.chokeWeight ? d * (1 + 2 * (kDtMax - std::min(ds, dd)) / kDtMax) : d;
}
uint64_t fnvHash(const uint8_t *p, size_t n) { uint64_t h = 0xcbf29ce484222325ULL; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ULL; return h; }
struct Pair { int px, py, qx, qy; };  // source cell -> its image under the copy's transform (cells)

// ---- ISOM helpers -----------------------------------------------------------------------------
// The ISOM grid (editor+0x10 -> {w, h, cells}) has (mapW/2+1) x (mapH+1) cells of 4 u16 edge values.
// A pure terrain shows as (type << 4 | direction); transitions are >= 0x100. Diamond (a, b) is the
// corner of cells (a-1..a, b-1..b); its terrain is slot 2 of cell (a-1, b-1).
struct IsomSnap {
    int *grid = nullptr; uint8_t *cells = nullptr; size_t bytes = 0; int rect[4] = {};
    std::vector<uint8_t> data;
    void take(void *ed) {
        void *isom = static_cast<uint8_t *>(ed) + ED_ISOM;
        grid = at<int *>(isom, 0);
        if (!grid) return;
        bytes = (size_t)grid[0] * grid[1] * 8; cells = reinterpret_cast<uint8_t *>(grid[2]);
        data.assign(cells, cells + bytes);
        memcpy(rect, static_cast<int *>(isom) + 3, sizeof rect);
    }
    void restore(void *ed) const {
        if (!grid) return;
        memcpy(cells, data.data(), bytes);
        memcpy(static_cast<int *>(static_cast<void *>(static_cast<uint8_t *>(ed) + ED_ISOM)) + 3, rect, sizeof rect);
    }
    // pure terrain type of diamond (a, b) in this snapshot, or -1
    int pureType(int a, int b) const {
        if (!grid || a < 1 || b < 1 || a > grid[0] || b > grid[1]) return -1;
        uint16_t v = reinterpret_cast<const uint16_t *>(data.data() + ((size_t)(b - 1) * grid[0] + (a - 1)) * 8)[2];
        return v < 0x100 ? v >> 4 : -1;
    }
};

// Brush id (editor+0x54) for an ISOM terrain type, learned once per tileset by sandbox placements.
int g_brushForType[8][64];
bool g_brushTableReady[8];

void learnBrushes(const Ctx &c, int ts, int a, int b) {
    if (g_brushTableReady[ts]) return;
    g_brushTableReady[ts] = true;
    for (int &v : g_brushForType[ts]) v = -1;
    uint16_t saved = at<uint16_t>(c.ed, 0x54);
    IsomSnap s; s.take(c.ed);
    for (int id = 0; id < 64; id++) {
        at<uint16_t>(c.ed, 0x54) = (uint16_t)id;
        g_shadow.clear(); g_trial = true; g_trialOwner = GetCurrentThreadId();
        HRESULT hr = g_origPlaceDiamond(c.ed, nullptr, a, b);
        g_trial = false;
        if (hr >= 0) {
            IsomSnap after; after.take(c.ed);
            int t = after.pureType(a, b);
            if (t >= 0 && t < 64 && g_brushForType[ts][t] < 0) g_brushForType[ts][t] = id;
        }
        s.restore(c.ed);
    }
    g_shadow.clear();
    at<uint16_t>(c.ed, 0x54) = saved;
    char buf[256]; int o = snprintf(buf, sizeof buf, "balance: brush table for %s:", kTilesets[ts]);
    for (int t = 0; t < 64; t++) if (g_brushForType[ts][t] >= 0) o += snprintf(buf + o, sizeof buf - o, " type%d=brush%d", t, g_brushForType[ts][t]);
    logf("%s", buf);
}

long scoreCopy(const Ctx &c, const std::vector<Pair> &pairs, const Grid &src, int qx0, int qy0, int qx1, int qy1, bool shadow) {
    Grid dst; clearance(c, qx0, qy0, qx1, qy1, shadow, dst);
    long score = 0;
    for (const Pair &p : pairs) {
        int ds = src.at(p.px, p.py), dd = dst.at(p.qx, p.qy);
        // mismatches where paths are narrow (chokes) matter more than in open ground
        if (ds >= 0 && dd >= 0) score += mismatch(ds, dd);
    }
    return score;
}

// "Add space back" like map makers do: where the copy blocks cells that are walkable at the source
// (isometric staircases bloat diagonal/rotated copies), repaint the diamond with the pure terrain
// it had before this paint step, keeping the change only if the clearance match improves.
IsomSnap g_beforeStep;  // ISOM before the current paint step's copies
int g_brushType = -1;   // ISOM type of the active brush

void erode(const Ctx &c, const std::vector<Pair> &pairs, const Grid &src, int qx0, int qy0, int qx1, int qy1, long score) {
    int ts = tilesetId(at<void *>(at<void *>(c.ed, ED_MAP), MAP_CHK));
    if (ts < 0 || !g_beforeStep.grid) return;
    uint16_t saved = at<uint16_t>(c.ed, 0x54);
    for (int round = 0; round < g_tune.rounds && score > 0; round++) {
        Grid dst; clearance(c, qx0, qy0, qx1, qy1, false, dst);
        // candidate diamonds: under copy-blocked, source-walkable cells (erode: paint the old ground
        // back) and, with dilation, under copy-walkable, source-blocked cells (paint the brush terrain)
        std::unordered_map<uint32_t, bool> cand;  // diamond -> dilate?
        for (const Pair &p : pairs) {
            int ds = src.at(p.px, p.py), dd = dst.at(p.qx, p.qy);
            bool er = ds > 0 && dd == 0, di = g_tune.dilate && ds == 0 && dd > 0;
            if (er || di) {
                int a, b;
                if (kSnap(c.ed, nullptr, p.qx * 8 + 4, p.qy * 8 + 4, &a, &b) >= 0) cand.emplace(tkey(a, b), di);
            }
            if ((int)cand.size() >= g_tune.cands) break;
        }
        IsomSnap now; now.take(c.ed);
        long bestScore = score; int bestA = 0, bestB = 0, bestBrush = -1;
        for (auto &kv : cand) {
            int a = kv.first >> 16, b = kv.first & 0xffff, brush;
            if (kv.second) {
                brush = saved;  // dilate with the active brush
            } else {
                int t = g_beforeStep.pureType(a, b);
                if (t < 0 || t == g_brushType || t >= 64) continue;
                brush = g_brushForType[ts][t];
            }
            if (brush < 0) continue;
            at<uint16_t>(c.ed, 0x54) = (uint16_t)brush;
            g_shadow.clear(); g_trial = true; g_trialOwner = GetCurrentThreadId();
            HRESULT hr = g_origPlaceDiamond(c.ed, nullptr, a, b);
            g_trial = false;
            if (hr >= 0) {
                long sc = scoreCopy(c, pairs, src, qx0, qy0, qx1, qy1, true) + g_tune.margin;  // must clearly help
                if (sc < bestScore) { bestScore = sc; bestA = a; bestB = b; bestBrush = brush; }
            }
            now.restore(c.ed);
        }
        g_shadow.clear();
        at<uint16_t>(c.ed, 0x54) = saved;
        if (bestBrush < 0) break;
        at<uint16_t>(c.ed, 0x54) = (uint16_t)bestBrush;
        g_origPlaceDiamond(c.ed, nullptr, bestA, bestB);
        at<uint16_t>(c.ed, 0x54) = saved;
        g_stats.eroded++;
        if (getenv("SCMHD_DUMP")) logf("balance erode dia (%d,%d) brush %d: score %ld -> %ld", bestA, bestB, bestBrush, score, bestScore);
        score = bestScore;
    }
}

// Place one copy of the source brush: try the snapped diamond and its neighbours in the sandbox and
// keep the one whose clearance best matches the transformed source.
HRESULT smartPlace(const Ctx &c, int tx, int ty, const std::vector<Pair> &pairs, const std::unordered_set<uint32_t> &covered,
                   int *chosenDelta) {
    int a, b;
    if (kSnap(c.ed, nullptr, tx, ty, &a, &b) < 0) return E_FAIL;
    *chosenDelta = 0;
    if (pairs.empty()) return g_origPlaceDiamond(c.ed, nullptr, a, b);

    void *isom = static_cast<uint8_t *>(c.ed) + ED_ISOM;
    int *grid = at<int *>(isom, 0);
    if (!grid) return g_origPlaceDiamond(c.ed, nullptr, a, b);
    size_t gridBytes = (size_t)grid[0] * grid[1] * 8;
    uint8_t *cells = reinterpret_cast<uint8_t *>(grid[2]);
    static std::vector<uint8_t> snap;
    snap.assign(cells, cells + gridBytes);
    int rect[4]; memcpy(rect, static_cast<int *>(isom) + 3, sizeof rect);

    // clearance of the source footprint (real map) and the window the copy lands in
    int px0 = INT_MAX, py0 = INT_MAX, px1 = INT_MIN, py1 = INT_MIN, qx0 = INT_MAX, qy0 = INT_MAX, qx1 = INT_MIN, qy1 = INT_MIN;
    for (const Pair &p : pairs) {
        px0 = std::min(px0, p.px); py0 = std::min(py0, p.py); px1 = std::max(px1, p.px + 1); py1 = std::max(py1, p.py + 1);
        qx0 = std::min(qx0, p.qx); qy0 = std::min(qy0, p.qy); qx1 = std::max(qx1, p.qx + 1); qy1 = std::max(qy1, p.qy + 1);
    }
    Grid src, dst;
    clearance(c, px0, py0, px1, py1, false, src);

    // candidates: the snapped diamond first, then rings of neighbours
    static std::vector<std::pair<int, int>> offs;
    if (offs.empty()) {
        offs.push_back({0, 0});
        for (int r = 1; r <= 2; r++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++)
                    if (std::max(std::abs(dx), std::abs(dy)) == r) offs.push_back({dx, dy});
    }
    size_t best = 0; long bestScore = LONG_MAX, firstScore = -1;
    for (size_t i = 0; i < offs.size(); i++) {
        g_shadow.clear();
        g_trial = true; g_trialOwner = GetCurrentThreadId();
        HRESULT hr = g_origPlaceDiamond(c.ed, nullptr, a + offs[i].first, b + offs[i].second);
        g_trial = false;
        long score = LONG_MAX;
        if (hr >= 0) {
            clearance(c, qx0, qy0, qx1, qy1, true, dst);
            score = 0;
            for (const Pair &p : pairs) {
                int ds = src.at(p.px, p.py), dd = dst.at(p.qx, p.qy);
                if (ds >= 0 && dd >= 0) score += mismatch(ds, dd);
            }
            // walkability changed outside the mirrored footprint is asymmetry too
            for (auto &kv : g_shadow) {
                if (covered.count(kv.first)) continue;
                int x = kv.first >> 16, y = kv.first & 0xffff;
                uint16_t before = tileAt(x, y, false);
                for (int k = 0; k < 16; k++)
                    score += 3 * ((miniFlags(*c.walk, kv.second, k & 3, k >> 2) ^ miniFlags(*c.walk, before, k & 3, k >> 2)) & 1);
            }
            if (i) score += 1 + (long)(std::abs(offs[i].first) + std::abs(offs[i].second));  // prefer small moves on ties
        }
        memcpy(cells, snap.data(), gridBytes);
        memcpy(static_cast<int *>(isom) + 3, rect, sizeof rect);
        if (i == 0) firstScore = score;
        if (score < bestScore) { bestScore = score; best = i; }
        if (i == 0 && score == 0) break;  // already perfect (e.g. X/Y mirrors are exact)
    }
    // Also consider leaving this copy out: staircases make mirrored copies fatter than the source
    // (extra cliff cells along every step), and the neighbouring steps often already cover the
    // diamond. A gap that would open a walkable crossing scores badly through the clearance term.
    bool skip = false;
    if (bestScore > 0) {
        g_shadow.clear();
        clearance(c, qx0, qy0, qx1, qy1, true, dst);
        long score = 4;  // slight preference for placing
        for (const Pair &p : pairs) {
            int ds = src.at(p.px, p.py), dd = dst.at(p.qx, p.qy);
            if (ds >= 0 && dd >= 0) score += std::abs(ds - dd);
        }
        if (score < bestScore) { bestScore = score; skip = true; }
    }
    g_shadow.clear();
    *chosenDelta = skip ? -1 : (int)best;
    if (getenv("SCMHD_DUMP"))
        logf("balance copy at (%d,%d) dia (%d,%d): %zu cells, snap score %ld -> %s #%zu (%+d,%+d) score %ld", tx, ty, a, b,
             pairs.size(), firstScore, skip ? "SKIP" : "best", best, offs[best].first, offs[best].second, bestScore);
    HRESULT hr = skip ? S_OK : g_origPlaceDiamond(c.ed, nullptr, a + offs[best].first, b + offs[best].second);
    if (hr >= 0 && bestScore > 0) erode(c, pairs, src, qx0, qy0, qx1, qy1, bestScore);
    return hr;
}

// Source footprint (tiles the primary brush changed, plus a 1-tile margin) mapped through T.
void buildPairs(const Ctx &c, const Xform &T, std::vector<Pair> &pairs, std::unordered_set<uint32_t> &covered) {
    pairs.clear(); covered.clear();
    std::unordered_set<uint32_t> region;
    for (uint32_t k : g_source) {
        int x = k >> 16, y = k & 0xffff;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (x + dx >= 0 && y + dy >= 0 && x + dx < c.wt && y + dy < c.ht) region.insert(tkey(x + dx, y + dy));
    }
    for (uint32_t k : region) {
        int x = k >> 16, y = k & 0xffff;
        for (int m = 0; m < 16; m++) {
            int mx = x * 4 + (m & 3), my = y * 4 + (m >> 2);
            int qx, qy; T(c, mx * 8 + 4, my * 8 + 4, &qx, &qy);
            pairs.push_back({mx, my, qx >> 3, qy >> 3});
            covered.insert(tkey(qx >> 5, qy >> 5));
        }
    }
}


HRESULT placeCopy(const Ctx &c, int tx, int ty, const Xform &T) {
    std::vector<Pair> pairs; std::unordered_set<uint32_t> covered;
    buildPairs(c, T, pairs, covered);
    int delta = 0;
    HRESULT hr = smartPlace(c, tx, ty, pairs, covered, &delta);
    g_stats.copies++; if (delta) g_stats.moved++;
    return hr;
}

// ---- hooks ------------------------------------------------------------------------------------
// copies beyond the 3 that ScmDraft's arrays hold, emitted after the last regular one
struct Copy { int x, y; M2 m; };
struct Pending { bool active = false; int lastX = 0, lastY = 0; std::vector<Copy> extra; } g_pending;
std::vector<Copy> g_copies;  // every mirror copy of the current paint step, with its transform
int g_depth;                 // re-entrancy guard for our own calls into the originals
enum Phase { IDLE, PRIMARY, COPIES } g_phase = IDLE;
bool g_primaryPlaced;

// Hook the primitives inside PlaceAt's rotation loop too, so only the first placement uses the
// palette terrain. Copies retain the native shape, transitions, random variants and undo node.
struct CopyBrushScope {
    void *ed; uint16_t saved;
    CopyBrushScope(void *editor) : ed(editor), saved(at<uint16_t>(ed, 0x54)) {
        void *map = at<void *>(ed, ED_MAP);
        if (g_phase == IDLE || inTrial() || at<int16_t>(map, 0x704) != 1 || !alternateTerrain(ed)) return;
        if (g_primaryPlaced) at<uint16_t>(ed, 0x54) = (uint16_t)copyTerrain(ed)->id;
        g_primaryPlaced = true;
        if (getenv("SCMHD_DUMP")) logf("copy terrain: kind=%d brush=%u (palette=%u)", at<int>(ed, ED_KIND), at<uint16_t>(ed, 0x54), saved);
    }
    ~CopyBrushScope() { at<uint16_t>(ed, 0x54) = saved; }
};
HRESULT __fastcall PlaceDiamondHook(void *ed, void *edx, int a, int b) {
    CopyBrushScope choice(ed);
    return g_origPlaceDiamond(ed, edx, a, b);
}
HRESULT __fastcall PlaceRectHook(void *ed, void *edx, int x0, int y0, int x1, int y1) {
    CopyBrushScope choice(ed);
    return g_origPlaceRect(ed, edx, x0, y0, x1, y1);
}

// A terrain override is intentional asymmetry; erosion must not repaint it to match the source.
bool balancing(void *ed) { return g_balance && !alternateTerrain(ed) && at<int>(ed, ED_KIND) == 0 && g_phase != IDLE; }

HRESULT __fastcall MirrorHook(void *ed, void *edx, int x, int y, int *outX, int *outY, unsigned *count) {
    bool ours = g_diag1 || g_diag2 || g_balance;
    if (!outX || !outY || !count || *count < 3 || !ours) { g_pending.active = false; return g_origMirror(ed, edx, x, y, outX, outY, count); }
    int w, h;
    if (!mapSizePx(at<void *>(ed, ED_MAP), &w, &h)) return g_origMirror(ed, edx, x, y, outX, outY, count);
    bool mx = at<char>(ed, 0x88), my = at<char>(ed, 0x89), mxy = at<char>(ed, 0x8a);
    std::vector<M2> g = group(mx, my, mxy, g_diag1, g_diag2);
    unsigned cap = *count, n = 0;
    g_pending = Pending();
    g_copies.clear();
    for (M2 m : g) {
        int px, py; apply(m, x, y, w, h, &px, &py);
        g_copies.push_back({px, py, m});
        if (n < cap) { outX[n] = px; outY[n] = py; n++; }
        else g_pending.extra.push_back({px, py, m});
    }
    if (!g_pending.extra.empty()) { g_pending.active = true; g_pending.lastX = outX[n - 1]; g_pending.lastY = outY[n - 1]; }
    *count = n;
    return S_OK;
}

// Debug metrics over the whole map, for the current mirror group: 8x8 cells whose walkability differs
// from their image, and the summed clearance difference (in cells), i.e. how unequal the paths are.
void globalAsymmetry(void *ed, long *cellsOut, long *widthOut) {
    *cellsOut = *widthOut = -1;
    Ctx c;
    if (!makeCtx(ed, c)) return;
    bool mx = at<char>(ed, 0x88), my = at<char>(ed, 0x89), mxy = at<char>(ed, 0x8a);
    std::vector<M2> g = group(mx, my, mxy, g_diag1, g_diag2);
    Grid d; clearance(c, 0, 0, c.wt * 4, c.ht * 4, false, d);
    long cells = 0, width = 0;
    for (M2 m : g)
        for (int y = 0; y < c.ht * 4; y++)
            for (int x = 0; x < c.wt * 4; x++) {
                int qx, qy; apply(m, x * 8 + 4, y * 8 + 4, c.wpx, c.hpx, &qx, &qy);
                if (walkBit(c, x, y, false) != walkBit(c, qx >> 3, qy >> 3, false)) cells++;
                width += std::abs(d.at(x, y) - d.at(qx >> 3, qy >> 3));
            }
    *cellsOut = cells / 2; *widthOut = width / 6;  // both sides counted; thirds of a cell
    if (const char *path = getenv("SCMHD_WALKDUMP")) {  // walkability as PGM (255 = walkable)
        if (FILE *f = fopen(path, "wb")) {
            fprintf(f, "P5 %d %d 255\n", c.wt * 4, c.ht * 4);
            for (int y = 0; y < c.ht * 4; y++)
                for (int x = 0; x < c.wt * 4; x++) fputc(walkBit(c, x, y, false) == 1 ? 255 : 0, f);
            fclose(f);
        }
    }
}

void __fastcall PlaceTerrainHook(void *ed, void *edx, int p1, void *brush, int x, int y) {
    g_lastEditor = ed;
    g_phase = PRIMARY;
    g_primaryPlaced = false;
    g_stats = Stats();
    g_origPlaceTerrain(ed, edx, p1, brush, x, y);
    g_phase = IDLE;
    g_source.clear();
    if (getenv("SCMHD_DUMP")) {
        long cells = -1, width = -1;
        if (at<int>(ed, ED_TYPE) == 1) globalAsymmetry(ed, &cells, &width);
        logf("balance: %d copies, %d shifted, %d eroded; global asymmetry %ld cells, width %ld", g_stats.copies, g_stats.moved, g_stats.eroded, cells, width);
    }
}

// After the primary brush: remember the ISOM state (the "before" for erosion), the brush's ISOM type,
// and learn the tileset's brush table on first use.
void afterPrimary(const Ctx &c, int x, int y) {
    int a, b;
    if (kSnap(c.ed, nullptr, x, y, &a, &b) < 0) return;
    g_beforeStep.take(c.ed);
    g_brushType = g_beforeStep.pureType(a, b);
    int ts = tilesetId(at<void *>(at<void *>(c.ed, ED_MAP), MAP_CHK));
    if (ts >= 0) learnBrushes(c, ts, a, b);
}

// Rotation modes with balancing: do ScmDraft's rotation loop ourselves so each rotated copy (and
// its mirror with "+ Mirror") can be balanced against the source.
HRESULT smartRotations(const Ctx &c, int x, int y, unsigned steps) {
    int px, py, a, b;
    if (kRot(c.ed, nullptr, x, y, 0, steps, &px, &py) < 0 || kSnap(c.ed, nullptr, px, py, &a, &b) < 0) return E_FAIL;
    g_source.clear();
    g_recordSource = true;
    HRESULT hr = g_origPlaceDiamond(c.ed, nullptr, a, b);
    g_recordSource = false;
    afterPrimary(c, px, py);
    for (int mirrored = 0; mirrored <= (g_rotMirror ? 1 : 0); mirrored++)
        for (unsigned k = mirrored ? 0 : 1; k < steps && hr >= 0; k++) {
            Xform T; T.kind = Xform::ROT; T.step = k; T.steps = steps; T.preMirror = mirrored;
            T(c, x, y, &px, &py);
            hr = placeCopy(c, px, py, T);
        }
    return hr;
}

HRESULT __fastcall PlaceHook(void *ed, void *edx, void *brush, int x, int y, unsigned steps) {
    if (g_depth) return g_origPlace(ed, edx, brush, x, y, steps);
    g_depth++;
    HRESULT hr;
    int type = at<int>(ed, ED_TYPE), w, h;
    Ctx c;
    if (balancing(ed) && makeCtx(ed, c)) {
        if (g_phase == PRIMARY) {
            g_phase = COPIES;
            if ((type == 2 || type == 3) && steps >= 1) {
                hr = smartRotations(c, x, y, steps);
            } else {
                g_source.clear();
                g_recordSource = true;
                hr = g_origPlace(ed, edx, brush, x, y, steps);
                g_recordSource = false;
                afterPrimary(c, x, y);
            }
        } else {
            // a mirror copy: find its transform
            const Copy *cp = nullptr;
            for (const Copy &k : g_copies) if (k.x == x && k.y == y) { cp = &k; break; }
            if (cp) { Xform T; T.m = cp->m; hr = placeCopy(c, x, y, T); }
            else hr = g_origPlace(ed, edx, brush, x, y, steps);
            if (hr >= 0 && g_pending.active && x == g_pending.lastX && y == g_pending.lastY) {
                g_pending.active = false;
                for (const Copy &k : g_pending.extra) { Xform T; T.m = k.m; if ((hr = placeCopy(c, k.x, k.y, T)) < 0) break; }
            }
        }
        g_depth--;
        return hr;
    }

    hr = g_origPlace(ed, edx, brush, x, y, steps);
    if (hr >= 0) {
        if (g_phase == PRIMARY) g_phase = COPIES;
        if (g_pending.active && x == g_pending.lastX && y == g_pending.lastY) {
            g_pending.active = false;
            for (const Copy &k : g_pending.extra) if ((hr = g_origPlace(ed, edx, brush, k.x, k.y, steps)) < 0) break;
        }
        if (g_rotMirror && (type == 2 || type == 3) && mapSizePx(at<void *>(ed, ED_MAP), &w, &h)) {
            int mxp, myp; apply(kMirrorX, x, y, w, h, &mxp, &myp);
            hr = g_origPlace(ed, edx, brush, mxp, myp, steps);
        }
    }
    g_depth--;
    return hr;
}

HRESULT __fastcall DrawHook(void *eng, void *edx, void *ctx, void *brush, int x, int y, int p5) {
    HRESULT hr = g_origDraw(eng, edx, ctx, brush, x, y, p5);
    if (g_depth || hr < 0) return hr;
    g_depth++;
    if (g_pending.active && x == g_pending.lastX && y == g_pending.lastY) {
        g_pending.active = false;
        for (const Copy &k : g_pending.extra) if ((hr = g_origDraw(eng, edx, ctx, brush, k.x, k.y, p5)) < 0) break;
    }
    void *map = at<void *>(eng, ENG_MAP);
    int w, h;
    if (g_rotMirror && map && (at<int>(map, MAP_TYPE) == 2 || at<int>(map, MAP_TYPE) == 3) && mapSizePx(map, &w, &h)) {
        int mxp, myp; apply(kMirrorX, x, y, w, h, &mxp, &myp);
        hr = g_origDraw(eng, edx, ctx, brush, mxp, myp, p5);
    }
    g_depth--;
    return hr;
}

// ---- panel UI ---------------------------------------------------------------------------------
HWND g_dlg, g_cbDiag1, g_cbDiag2, g_cbRotMirror, g_cbBalance, g_copyButton;
WNDPROC g_origDlgProc;

void refreshMaps() {
    // repaint the brush preview with the new set of copies
    HWND top = GetAncestor(g_dlg, GA_ROOT);
    if (top) RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// Use the active palette selection rather than inventing terrain IDs or hardcoding tileset names.
void copyTerrainMenu(HWND dlg) {
    void *map = scmhdActiveMap();
    void *ed = map ? static_cast<uint8_t *>(map) + 0x40 : nullptr;
    CopyTerrain *choice = ed ? copyTerrain(ed) : nullptr;
    if (!choice || at<int16_t>(map, 0x700) != 1) {
        MessageBoxW(dlg, L"Select an isometric or rectangular terrain brush from the Terrain palette first.", L"Copy terrain", MB_OK | MB_ICONINFORMATION);
        return;
    }
    HWND tree = GetDlgItem(GetParent(dlg), 1145);
    if (!tree) tree = FindWindowExW(GetParent(dlg), nullptr, L"SysTreeView32", nullptr);
    wchar_t name[128] = {};
    TVITEMW item{}; item.mask = TVIF_TEXT; item.hItem = (HTREEITEM)SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0);
    item.pszText = name; item.cchTextMax = 128;
    if (item.hItem) SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&item);
    if (!name[0]) swprintf(name, 128, L"Terrain %u", at<uint16_t>(ed, 0x54));
    std::wstring selected = L"Use selected terrain: "; selected += name;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (choice->id < 0 ? MF_CHECKED : 0), 1, L"Same as original brush");
    AppendMenuW(menu, MF_STRING, 2, selected.c_str());
    if (choice->id >= 0) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        std::wstring current = L"Copies use: " + choice->name;
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_CHECKED, 3, current.c_str());
    }
    RECT r; GetWindowRect(g_copyButton, &r);
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, r.left, r.bottom, 0, dlg, nullptr);
    DestroyMenu(menu);
    if (command == 1) *choice = CopyTerrain();
    if (command == 2) { choice->id = at<uint16_t>(ed, 0x54); choice->name = name; }
    if (command) {
        logf("copy terrain: tileset=%d kind=%d selected=%d", tilesetId(at<void *>(map, MAP_CHK)), at<int>(ed, ED_KIND), choice->id);
        refreshMaps();
    }
}

LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
        int id = LOWORD(wp);
        if (id == ID_COPY_TERRAIN) { copyTerrainMenu(h); return 0; }
        bool *flag = id == ID_DIAG1 ? &g_diag1 : id == ID_DIAG2 ? &g_diag2 : id == ID_ROTMIRROR ? &g_rotMirror : id == ID_BALANCE ? &g_balance : nullptr;
        if (flag) {
            *flag = SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED;
            saveFlags();
            logf("symmetry flags: diag\\=%d diag/=%d rotMirror=%d balance=%d", g_diag1, g_diag2, g_rotMirror, g_balance);
            refreshMaps();
            return 0;
        }
    }
    return CallWindowProcW(g_origDlgProc, h, msg, wp, lp);
}

HWND makeCheckbox(HWND dlg, int id, const wchar_t *text, HWND like, bool checked) {
    RECT r; GetWindowRect(like, &r); MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT *>(&r), 2);
    HWND cb = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, r.left, r.top,
                              r.right - r.left, r.bottom - r.top, dlg, reinterpret_cast<HMENU>((INT_PTR)id),
                              reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE)), nullptr);
    SendMessageW(cb, WM_SETFONT, SendMessageW(like, WM_GETFONT, 0, 0), FALSE);
    SendMessageW(cb, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return cb;
}

void placeBelow(HWND cb, HWND anchor, HWND left, HWND dlg, int gap, int rowH) {
    RECT r, l; GetWindowRect(anchor, &r); MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT *>(&r), 2);
    GetWindowRect(left, &l); MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT *>(&l), 2);
    SetWindowPos(cb, nullptr, l.left, r.bottom + gap, std::max<int>(l.right - l.left, 120), rowH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void __fastcall ResizeHook(void *panel, void *edx, HWND hwnd, char expanded, int mode) {
    HWND dlg = at<HWND>(panel, PANEL_DLG);
    HWND xy = dlg ? GetDlgItem(dlg, ID_MIRROR_XY) : nullptr, y = dlg ? GetDlgItem(dlg, ID_MIRROR_Y) : nullptr;
    HWND skew = dlg ? GetDlgItem(dlg, ID_SKEW) : nullptr;
    if (xy && y && skew && !g_cbDiag1) {
        g_dlg = GetParent(xy);
        g_cbDiag1 = makeCheckbox(g_dlg, ID_DIAG1, L"Mirror-Diag (\\)", xy, g_diag1);
        g_cbDiag2 = makeCheckbox(g_dlg, ID_DIAG2, L"Mirror-Diag (/)", xy, g_diag2);
        g_cbRotMirror = makeCheckbox(g_dlg, ID_ROTMIRROR, L"+ Mirror (rotations)", xy, g_rotMirror);
        g_cbBalance = makeCheckbox(g_dlg, ID_BALANCE, L"Balance walkability", xy, g_balance);
        g_copyButton = makeCheckbox(g_dlg, ID_COPY_TERRAIN, L"Copy terrain...", xy, false);
        SetWindowLongW(g_copyButton, GWL_STYLE, WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON);
        g_origDlgProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_dlg, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DlgProc)));
        logf("symmetry panel: added controls to %p", g_dlg);
    }
    if (!g_cbDiag1) { g_origResize(panel, edx, hwnd, expanded, mode); return; }

    RECT ry, rxy; GetWindowRect(y, &ry); GetWindowRect(xy, &rxy);
    int pitch = rxy.top - ry.top, rowH = rxy.bottom - rxy.top, gap = pitch - rowH;
    bool mirror = expanded && mode == 1, rot = expanded && (mode == 2 || mode == 3);
    placeBelow(g_cbDiag1, xy, xy, g_dlg, gap, rowH);
    placeBelow(g_cbDiag2, g_cbDiag1, xy, g_dlg, gap, rowH);
    placeBelow(g_cbRotMirror, skew, xy, g_dlg, gap + 2, rowH);
    placeBelow(g_cbBalance, mirror ? g_cbDiag2 : g_cbRotMirror, xy, g_dlg, gap, rowH);
    placeBelow(g_copyButton, g_cbBalance, xy, g_dlg, gap, rowH + 4);
    ShowWindow(g_copyButton, mirror || rot ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_cbDiag1, mirror ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_cbDiag2, mirror ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_cbRotMirror, rot ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_cbBalance, mirror || rot ? SW_SHOWNA : SW_HIDE);

    // The original sizes the panel to the bottom of a per-mode anchor control (Mirror-XY / Skew) and
    // then notifies the sidebar. Move that anchor down to our last control while it measures, so the
    // panel gets the right height and the sidebar lays out around it.
    HWND anchor = mirror ? xy : rot ? skew : nullptr;
    if (!anchor) { g_origResize(panel, edx, hwnd, expanded, mode); return; }
    RECT ar, lr;
    GetWindowRect(anchor, &ar); MapWindowPoints(nullptr, GetParent(anchor), reinterpret_cast<POINT *>(&ar), 2);
    GetWindowRect(g_copyButton, &lr); MapWindowPoints(nullptr, GetParent(anchor), reinterpret_cast<POINT *>(&lr), 2);
    const UINT quiet = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOREDRAW;
    SetWindowPos(anchor, nullptr, ar.left, lr.bottom - (ar.bottom - ar.top), 0, 0, quiet);
    g_origResize(panel, edx, hwnd, expanded, mode);
    SetWindowPos(anchor, nullptr, ar.left, ar.top, 0, 0, quiet);
}


// ---- Copy Mirrored ------------------------------------------------------------------------------
// BW tiles mostly have no literal left-right mirror (terrain is lit from one side, so a cliff facing
// left is drawn differently from one facing right). The mirror that means something is isometric:
// every diamond keeps its terrain type at the mirrored position and ScmDraft's own ISOM code builds the
// tiles (cliffs facing the other way, correct lighting and transitions). We do that in the sandbox
// over the copied rectangle, read the resulting tiles into the copy brush and throw the sandbox away.
// Non-ISOM tiles (doodads, hand-placed tiles) use their pixel mirror when the tileset has one.
constexpr uintptr_t kCopySelection = 0x0043e1d0;   // CEngineMap::CopySelection(CBrush *)
constexpr int MAP_SELECTION = 0xe20, SEL_X0 = 0x24, SEL_Y0 = 0x28, SEL_X1 = 0x2c, SEL_Y1 = 0x30;
constexpr int BRUSH_W = 0x30, BRUSH_H = 0x32, BRUSH_TILES = 0x34;
using CopySelectionFn = HRESULT(__fastcall *)(void *, void *, void *);
CopySelectionFn g_origCopySelection;
bool g_mirrorNext;

// Tile-level left-right mirror per tileset. About half of a tileset's tiles have an exact pixel mirror;
// the others (cliffs lit from one side, many transitions) get the tile that looks closest to the flipped
// image (L1 over RGB, prefiltered by an 8x8 average-colour signature). Ground tiles only map to ground
// tiles; doodad pieces may map to anything. Results are cached per tile value.
struct TileMirror {
    bool tried = false, ok = false;
    std::vector<uint8_t> cv, vx, vr, wpe;
    size_t groups = 0, nMega = 0, nMini = 0;
    std::vector<uint8_t> isDoodad;                   // per cv5 group
    std::vector<uint16_t> candTile;                  // one tile value per distinct megatile image
    std::vector<uint8_t> candDoodad;
    std::vector<std::array<uint8_t, 3072>> candRgb;  // 32x32 RGB
    std::vector<std::array<int16_t, 192>> candSig;   // 8x8 average RGB
    std::unordered_map<uint64_t, uint16_t> exact;    // image hash -> tile value
    std::unordered_map<uint16_t, uint16_t> cache;    // source tile -> mirrored tile
};
TileMirror g_tm[8];

void tmRender(const TileMirror &t, uint16_t tile, uint8_t *idx) {
    size_t g = tile >> 4;
    uint16_t mega = g < t.groups ? (uint16_t)(t.cv[g * 52 + 20 + (tile & 15) * 2] | t.cv[g * 52 + 21 + (tile & 15) * 2] << 8) : 0;
    const uint32_t *e = reinterpret_cast<const uint32_t *>(t.vx.data()) + (mega < t.nMega ? mega : 0) * 16;
    for (int i = 0; i < 16; i++) {
        uint32_t mini = e[i] >> 1; if (mini >= t.nMini) mini = 0;
        const uint8_t *src = t.vr.data() + mini * 64;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
            idx[((i >> 2) * 8 + y) * 32 + (i & 3) * 8 + x] = src[y * 8 + ((e[i] & 1) ? 7 - x : x)];
    }
}
void tmRgb(const TileMirror &t, const uint8_t *idx, uint8_t *rgb, int16_t *sig) {
    int acc[192] = {};
    for (int p = 0; p < 1024; p++) {
        const uint8_t *c = &t.wpe[idx[p] * 4];
        int cell = ((p >> 5) >> 2) * 8 + ((p & 31) >> 2);
        for (int k = 0; k < 3; k++) { rgb[p * 3 + k] = c[k]; acc[cell * 3 + k] += c[k]; }
    }
    for (int i = 0; i < 192; i++) sig[i] = (int16_t)(acc[i] / 16);
}

TileMirror *tileMirror(int ts) {
    if (ts < 0 || ts > 7) return nullptr;
    TileMirror &t = g_tm[ts];
    if (t.tried) return t.ok ? &t : nullptr;
    t.tried = true;
    std::string base = std::string("TileSet\\") + kTilesets[ts];
    if (!scmhdCascRead(base + ".cv5", t.cv) || !scmhdCascRead(base + ".vx4ex", t.vx) || !scmhdCascRead(base + ".vr4", t.vr) ||
        !scmhdCascRead(base + ".wpe", t.wpe) || t.wpe.size() < 1024) return nullptr;
    t.groups = t.cv.size() / 52; t.nMega = t.vx.size() / 64; t.nMini = t.vr.size() / 64;
    t.isDoodad.resize(t.groups);
    std::unordered_set<uint16_t> seenMega;
    uint8_t idx[1024];
    for (size_t g = 0; g < t.groups; g++) {
        t.isDoodad[g] = (t.cv[g * 52] | t.cv[g * 52 + 1] << 8) == 1;  // cv5 index 1 = doodad group
        for (int sub = 0; sub < 16; sub++) {
            uint16_t mega = (uint16_t)(t.cv[g * 52 + 20 + sub * 2] | t.cv[g * 52 + 21 + sub * 2] << 8);
            if (!mega || mega >= t.nMega || !seenMega.insert(mega).second) continue;
            uint16_t tile = (uint16_t)(g << 4 | sub);
            tmRender(t, tile, idx);
            t.exact.emplace(fnvHash(idx, 1024), tile);
            t.candTile.push_back(tile);
            t.candDoodad.push_back(t.isDoodad[g]);
            t.candRgb.emplace_back(); t.candSig.emplace_back();
            tmRgb(t, idx, t.candRgb.back().data(), t.candSig.back().data());
        }
    }
    t.ok = !t.candTile.empty();
    logf("mirror: %s tile mirror ready, %zu distinct tiles", kTilesets[ts], t.candTile.size());
    return t.ok ? &t : nullptr;
}

// returns the mirrored tile and whether it is an exact pixel mirror
uint16_t mirrorTile(TileMirror &t, uint16_t tile, bool *exact) {
    size_t g = tile >> 4;
    if (g >= t.groups) { *exact = false; return tile; }
    uint8_t idx[1024], flipped[1024];
    tmRender(t, tile, idx);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) flipped[y * 32 + x] = idx[y * 32 + 31 - x];
    auto ex = t.exact.find(fnvHash(flipped, 1024));
    if (ex != t.exact.end()) { *exact = true; return ex->second; }
    *exact = false;
    auto c = t.cache.find(tile);
    if (c != t.cache.end()) return c->second;
    uint8_t rgb[3072]; int16_t sig[192];
    tmRgb(t, flipped, rgb, sig);
    bool doodad = t.isDoodad[g];
    std::vector<std::pair<int, size_t>> coarse;
    coarse.reserve(t.candTile.size());
    for (size_t i = 0; i < t.candTile.size(); i++) {
        if (!doodad && t.candDoodad[i]) continue;
        int d = 0; const int16_t *cs = t.candSig[i].data();
        for (int k = 0; k < 192; k++) d += std::abs(cs[k] - sig[k]);
        coarse.push_back({d, i});
    }
    size_t keep = std::min<size_t>(24, coarse.size());
    std::partial_sort(coarse.begin(), coarse.begin() + keep, coarse.end());
    long best = LONG_MAX; uint16_t bestTile = tile;
    for (size_t k = 0; k < keep; k++) {
        const uint8_t *cr = t.candRgb[coarse[k].second].data();
        long d = 0; for (int i = 0; i < 3072; i++) d += std::abs((int)cr[i] - (int)rgb[i]);
        if (d < best) { best = d; bestTile = t.candTile[coarse[k].second]; }
    }
    t.cache[tile] = bestTile;
    return bestTile;
}

// Mirrors w x h copied tiles in place (0xffff = not selected).
void mirrorTiles(void *map, int x0, int y0, int w, int h, uint16_t *tiles) {
    if (!tiles || w <= 0 || h <= 0) return;
    TileMirror *tm = tileMirror(tilesetId(at<void *>(map, MAP_CHK)));
    if (!tm) { logf("mirror: no tileset data"); return; }
    DWORD t0 = GetTickCount();
    std::vector<uint16_t> out((size_t)w * h);
    int exact = 0, closest = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t src = tiles[(size_t)y * w + x];
            uint16_t &dst = out[(size_t)y * w + (w - 1 - x)];
            if (src == 0xffff) { dst = 0xffff; continue; }  // not selected
            bool ex;
            dst = mirrorTile(*tm, src, &ex);
            (ex ? exact : closest)++;
        }
    if (const char *dump = getenv("SCMHD_MIRRORDUMP")) {  // w, h, tileset, source tiles, mirrored tiles
        if (FILE *f = fopen(dump, "wb")) {
            int hdr[3] = {w, h, tilesetId(at<void *>(map, MAP_CHK))}; fwrite(hdr, sizeof hdr, 1, f);
            fwrite(tiles, 2, out.size(), f); fwrite(out.data(), 2, out.size(), f); fclose(f);
        }
    }
    memcpy(tiles, out.data(), out.size() * 2);
    logf("mirror: %dx%d tiles at %d,%d: %d exact mirrors, %d closest matches, %lu ms", w, h, x0, y0, exact, closest, GetTickCount() - t0);
}

void mirrorBrush(void *map, void *brush) {
    int w = at<uint16_t>(brush, BRUSH_W), h = at<uint16_t>(brush, BRUSH_H);
    uint8_t *sel = static_cast<uint8_t *>(map) + MAP_SELECTION;
    if (at<int>(sel, SEL_X1) - at<int>(sel, SEL_X0) != w || at<int>(sel, SEL_Y1) - at<int>(sel, SEL_Y0) != h) return;
    mirrorTiles(map, at<int>(sel, SEL_X0), at<int>(sel, SEL_Y0), w, h, at<uint16_t *>(brush, BRUSH_TILES));
}

// Edit > Copy serializes the selection for the clipboard; 0x438f00 writes its terrain part:
// u16 w, u16 h, then w*h tile values (0xffff = not selected).
constexpr uintptr_t kExportTerrain = 0x00438f00;
using ExportTerrainFn = HRESULT(__fastcall *)(void *, void *, int *, uint8_t *, uint32_t *, uint16_t *);
ExportTerrainFn g_origExportTerrain;
HRESULT __fastcall ExportTerrainHook(void *map, void *edx, int *rect, uint8_t *mask, uint32_t *size, uint16_t *out) {
    HRESULT hr = g_origExportTerrain(map, edx, rect, mask, size, out);
    if (hr >= 0 && out && g_mirrorNext && rect) {
        g_mirrorNext = false;
        int w = out[0], h = out[1];
        logf("mirror: clipboard terrain %dx%d at %d,%d", w, h, rect[0], rect[1]);
        try { mirrorTiles(map, rect[0], rect[1], w, h, out + 2); } catch (...) { g_trial = false; logf("mirror: failed"); }
    }
    return hr;
}

HRESULT __fastcall CopySelectionHook(void *map, void *edx, void *brush) {
    HRESULT hr = g_origCopySelection(map, edx, brush);
    if (hr >= 0 && g_mirrorNext && brush) {
        g_mirrorNext = false;
        try { mirrorBrush(map, brush); } catch (...) { g_trial = false; logf("mirror: failed"); }
    }
    return hr;
}

void requestMirroredCopy(HWND mainWnd, void *map) {
    // test hook: SCMHD_TESTSELECT="x0,y0,x1,y1" selects that terrain rectangle first (as Select All does)
    int r[4];
    if (const char *t = getenv("SCMHD_TESTSELECT"))
        if (map && sscanf(t, "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4) {
            host_image::pointer<HRESULT(__fastcall *)(void *, void *, int, int, int, int, int)>(0x00459500)(
                static_cast<uint8_t *>(map) + MAP_SELECTION, nullptr, r[0], r[1], r[2], r[3], 1);
            at<int16_t>(map, 0x704) = 5;  // the terrain selection tool (map+0x704: 1 paint, 4 paste, 5-7 select)
        }
    if (map) {
        uint8_t *sel = static_cast<uint8_t *>(map) + MAP_SELECTION;
        logf("mirror: requested (map %p, main %p), selection %d,%d-%d,%d, layer %d", map, mainWnd, at<int>(sel, SEL_X0), at<int>(sel, SEL_Y0),
             at<int>(sel, SEL_X1), at<int>(sel, SEL_Y1), at<int>(map, 0x700));
    }
    g_mirrorNext = true;
    SendMessageW(mainWnd, WM_COMMAND, 32999, 0);  // Edit > Copy; the hook mirrors the brush it fills
    g_mirrorNext = false;
}

// ---- Replace by unit sprite ----
// Turns the selected units into unit sprites (THG2 entries drawn with the unit's graphics) at the same
// positions and owners, then deletes the units — one undo step. Goes through ScmDraft's own code paths:
//   map+0x1dc  CUnitEditor:  0x489670 export selection (uint *bytes, entry *out), 0x2c-byte entries
//                            {+0 instance, +4 u16 x, +6 u16 y, +8 u16 type, +0x10 u8 owner, ...};
//                            vtbl+0x10 delete selection
//   map+0x140  unit sprite editor (GenericSpriteEditor, vtbl+0x18 accepts flags&0x2000);
//                            vtbl+0x10 = 0x480b70 place(int, originX, originY, brush): brush+0x28 count,
//                            brush+0x2c 20-byte entries {THG2 record (id, x, y, owner, 0, flags), int dx, int dy}
//   0x436880   CEngineMap open undo node (map+0x67c), closed by zeroing it (as 0x43b910 does)
// Flags 0x2280 = what ScmDraft's sprite palette gives an enabled unit sprite (0x49c990).
struct UnitEntry { uint32_t inst; uint16_t x, y, type, rel, props, elems; uint8_t owner, hp, shield, energy; uint32_t res; uint16_t hangar, state; uint32_t pad[4]; };
static_assert(sizeof(UnitEntry) == 0x2c, "unit export entry");
struct SpriteEntry { uint16_t id, x, y; uint8_t owner, unused; uint16_t flags, pad; int dx, dy; };
static_assert(sizeof(SpriteEntry) == 0x14, "brush sprite entry");

void replaceUnitsBySprites(HWND mainWnd, void *map) {
    if (!map) return;
    uint8_t *m = static_cast<uint8_t *>(map);
    void *units = m + 0x1dc, *sprites = m + 0x140;
    if (at<void **>(sprites, 0)[6] != host_image::pointer<void *>(0x0048e200)) {
        logf("unitsprite: unexpected sprite editor vtable %p", at<void *>(sprites, 0));
        return;
    }
    auto exportSel = host_image::pointer<HRESULT(__fastcall *)(void *, void *, uint32_t *, void *)>(0x00489670);
    uint32_t bytes = 0;
    if (exportSel(units, nullptr, &bytes, nullptr) < 0 || !bytes) {
        MessageBoxW(mainWnd, L"Select some units first (Units layer).", L"Replace by Unit Sprite", MB_ICONINFORMATION);
        return;
    }
    std::vector<UnitEntry> sel(bytes / sizeof(UnitEntry));
    if (exportSel(units, nullptr, &bytes, sel.data()) < 0) return;

    std::vector<SpriteEntry> spr;
    for (auto &u : sel)
        if (u.type < 0xe4) spr.push_back({u.type, 0, 0, u.owner, 0, 0x2280, 0, u.x, u.y});
    if (spr.size() != sel.size()) {  // the deletion is selection-wide, so never drop units without a sprite
        MessageBoxW(mainWnd, L"The selection contains units with invalid type ids; nothing was changed.", L"Replace by Unit Sprite", MB_ICONWARNING);
        return;
    }
    struct { uint8_t pad[0x28]; uint32_t count; SpriteEntry *entries; uint8_t tail[0x40]; } brush{};
    brush.count = spr.size();
    brush.entries = spr.data();

    // the sprite editor stops (returning 1) at the map's sprite limit; check first so nothing is half done
    int limit = at<int16_t>(at<void *>(map, 0x38), 0x16a), have = at<int>(at<void *>(map, 0x6fc), 0x598c);
    logf("unitsprite: %zu selected, sprites %d / limit %d", sel.size(), have, limit);
    if (have + (int)spr.size() > limit) {
        wchar_t msg[200];
        swprintf(msg, 200, L"Not enough room: the map has %d of %d sprites, %d more needed.", have, limit, (int)spr.size());
        MessageBoxW(mainWnd, msg, L"Replace by Unit Sprite", MB_ICONWARNING);
        return;
    }
    host_image::pointer<int(__fastcall *)(void *, void *, char)>(0x00436880)(map, nullptr, 1);  // open undo node
    void **uvt = at<void **>(units, 0), **svt = at<void **>(sprites, 0);
    HRESULT hr = reinterpret_cast<HRESULT(__fastcall *)(void *, void *, int, int, int, void *)>(svt[4])(sprites, nullptr, 0, 0, 0, &brush);
    HRESULT hd = hr != 0 ? hr : reinterpret_cast<HRESULT(__fastcall *)(void *, void *)>(uvt[4])(units, nullptr);
    void *events = m + 8;  // "layer properties changed", as CEngineMap's delete does
    reinterpret_cast<HRESULT(__fastcall *)(void *, void *)>(at<void **>(events, 0)[5])(events, nullptr);
    at<void *>(map, 0x67c) = nullptr;  // close the undo node
    logf("unitsprite: %zu units selected -> %zu unit sprites (place hr=0x%08lx, delete hr=0x%08lx)", sel.size(), spr.size(), hr, hd);
    InvalidateRect(mainWnd, nullptr, FALSE);
}

template <class F> bool hook(uintptr_t addr, void *detour, F *orig) {
    return host_image::createHook(addr, detour, reinterpret_cast<void **>(orig), "symmetry");
}

bool install();
} // namespace sym
void *scmhdActiveMap();
namespace sym {
bool install() {
    loadFlags();
    if (const char *t = getenv("SCMHD_TUNE"))  // "chokeWeight,dilate,rounds,cands,margin" for experiments
        { int a = 1, b = 1; sscanf(t, "%d,%d,%d,%d,%d", &a, &b, &g_tune.rounds, &g_tune.cands, &g_tune.margin); g_tune.chokeWeight = a; g_tune.dilate = b; }
    bool ok = hook(kMirrorCoords, (void *)&MirrorHook, &g_origMirror) && hook(kPlaceAt, (void *)&PlaceHook, &g_origPlace) &&
              hook(kDrawBrushAt, (void *)&DrawHook, &g_origDraw) && hook(kPanelResize, (void *)&ResizeHook, &g_origResize) &&
              hook(kPlaceTerrain, (void *)&PlaceTerrainHook, &g_origPlaceTerrain) &&
              hook(kPlaceDiamond, (void *)&PlaceDiamondHook, &g_origPlaceDiamond) &&
              hook(kPlaceRect, (void *)&PlaceRectHook, &g_origPlaceRect) &&
              hook(kSetTile, (void *)&SetTileHook, &g_origSetTile) && hook(kGetTile, (void *)&GetTileHook, &g_origGetTile) &&
              hook(kCopySelection, (void *)&CopySelectionHook, &g_origCopySelection) &&
              hook(kExportTerrain, (void *)&ExportTerrainHook, &g_origExportTerrain);
    logf("symmetry hooks %s (diag\\=%d diag/=%d rotMirror=%d balance=%d)", ok ? "created" : "FAILED", g_diag1, g_diag2, g_rotMirror, g_balance);
    return ok;
}

} // namespace sym
