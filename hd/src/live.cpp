// Live "Test Mining" for ScmDraft 2 (0.9.10), part of scmhd.sdp.
//
// New -> Test Mining loads the open map into an embedded OpenBW game (the exact BW 1.16.1
// engine, data from the SC:R CASC install). Every start location and placed CC / Nexus / Hatchery
// gets on-map buttons; "Test mining" spawns that base's depot and workers (by default one per nearby
// mineral field; - / + change it) for its own player, and the engine mines in real time at Fastest
// speed: movement, collision, pathing over BW regions, 75-frame mining, field bouncing, larva.
// The player's sprites (depot, workers, carried minerals, shadows, player colour) are drawn into a copy
// of ScmDraft's 8-bit backbuffer before it reaches the window, so they show at every zoom and under
// HD terrain. Each field shows its live minerals per minute, each base its income. "Stop" removes the
// base's units again; turning Test Mining off removes everything.
#include "bwgame.h"
#include "simulation_map.h"
#include "ui/ui.h"
#include "mining_boost.h"
#include "return_boost.h"
#include "cycle_boost.h"
#include "mining_playback.h"
#include "overlay_layout.h"
#include "hd_sprites.h"

#include <windows.h>
#include <algorithm>
#include <deque>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

int scmhdGraphicsMode();
void logf(const char *fmt, ...);
bool scmhdCascRead(const std::string &name, std::vector<uint8_t> &out);
bool scmhdExportActiveMap(std::vector<uint8_t> &chk, std::string &err);
void scmhdLock();
void scmhdUnlock();
void *scmhdActiveMap();
struct Locked { Locked() { scmhdLock(); } ~Locked() { scmhdUnlock(); } };

namespace live {

using namespace bwgame;

constexpr int kWindow = 1429;            // frames in one game minute at Fastest (42 ms per frame)

struct Loader {
    void operator()(a_vector<uint8_t> &dst, a_string filename) const {
        std::string n(filename.c_str());
        std::replace(n.begin(), n.end(), '/', '\\');
        std::vector<uint8_t> v;
        if (!scmhdCascRead(n, v)) error("CASC: missing %s", filename.c_str());
        dst.assign(v.begin(), v.end());
    }
};

// ---- static data --------------------------------------------------------------------------------
std::unique_ptr<global_state> g_gst;
std::unique_ptr<bwgame::image_data> g_img;
bwgame::pcx_image g_dark[8];
bool g_darkLoaded[8];
bool g_dataFailed;

bool loadData() {
    if (g_gst) return true;
    if (g_dataFailed) return false;
    try {
        DWORD t0 = GetTickCount();
        auto gst = std::make_unique<global_state>();
        global_init(*gst, Loader());
        auto img = std::make_unique<bwgame::image_data>();
        bwgame::load_image_data(*img, Loader());
        g_gst = std::move(gst); g_img = std::move(img);
        logf("live: game data loaded in %lu ms", GetTickCount() - t0);
        return true;
    } catch (std::exception &e) {
        logf("live: loading game data failed: %s", e.what());
        g_dataFailed = true;
        return false;
    }
}

const char *kTilesetNames[8] = {"badlands", "platform", "install", "AshWorld", "Jungle", "Desert", "Ice", "Twilight"};
const bwgame::pcx_image *dark(int ts) {
    if (ts < 0 || ts > 7) return nullptr;
    if (!g_darkLoaded[ts]) {
        g_darkLoaded[ts] = true;
        std::vector<uint8_t> v;
        if (scmhdCascRead(std::string("TileSet\\") + kTilesetNames[ts] + "\\dark.pcx", v)) {
            try { g_dark[ts] = bwgame::load_pcx_data(v); } catch (...) {}
        }
    }
    return g_dark[ts].width == 256 ? &g_dark[ts] : nullptr;
}

const uint8_t *shadowRemap(int tileset) {
    const auto *p = dark(tileset);
    return p && p->data.size() >= 256 * 19 ? p->data.data() + 256 * 18 : nullptr;
}

// ---- map / bases ----------------------------------------------------------------------------------
struct Ghost {
    xy origin;
    int width, height;
    std::vector<uint32_t> pixels;  // fixed, premultiplied BGRA, with transparent GRP background
    std::shared_ptr<void> bitmap;
    int mode = 0, scale = 1;
    xy offset4{};
    int kind=0; // position, cargo, cycle
    xy commandAt{}, target{};
    int stage=0;
    size_t worker=0;
    COLORREF guideColor=RGB(110,200,255);
    bool annotate=false; // one body image per graphics mode owns the command guide
};
struct Base {
    int x, y, owner;
    bool start;           // start location (race selectable) vs placed depot (race fixed)
    int race;             // 0 zerg, 1 terran, 2 protoss
    bool active = false;
    int slot = -1;        // OpenBW player running this base
    int workers = 0, gasWorkers = 0;
    std::vector<unit_t *> fields, refineries;
    std::vector<std::pair<unit_t *, int>> assigned;  // worker -> field index, or -1-refinery index for gas
    std::vector<std::deque<int>> fieldDeliveries, refineryDeliveries;  // delivery frames per resource
    std::vector<std::deque<int>> fieldCycles;        // recent worker round trips (frames) per field
    std::deque<int> deliveries, gasDeliveries;       // all mineral / gas deliveries (8 each)
    std::unordered_map<unit_t *, std::pair<int, int>> lastDelivery;  // worker -> (frame, field)
    int startFrame = 0;
    std::array<int,3> searchSeconds{{15,15,15}};
    std::shared_ptr<boost_compute::Job<mining_boost::Plan>> positionSession;
    std::shared_ptr<boost_compute::Job<return_boost::Plan>> cargoSession;
    std::shared_ptr<boost_compute::Job<cycle_boost::Plan>> cycleSession;
    std::map<size_t,xy> pendingPositions, appliedPositions;
    bool boostEnabled = false, boostPending = false;
    int boostedFields = 0, fasterFields = 0;
    std::vector<Ghost> ghosts;
    bool cargoEnabled = false, cargoPending = false;
    std::vector<return_boost::Rule> cargoRules;
    std::set<size_t> cargoMarked;
    bool cycleEnabled = false, cyclePending = false;
    std::vector<cycle_boost::Rule> cycleRules;
    std::set<std::pair<size_t, int>> cycleMarked;
};

struct Game {
    unsigned guideMask=7; // Position / cargo / cycle; display only, shared by all bases.
    bool guideVisible(int kind) const {return (guideMask&(1u<<kind))!=0;}
    void *mapIdentity = nullptr;
    std::vector<uint8_t> chk;
    std::unique_ptr<game_state> game;
    std::unique_ptr<state> st;
    std::vector<Base> bases;
    int tileset = 0;
    DWORD lastTick = 0;
    mining_playback::Clock playback;
    std::shared_ptr<boost_compute::Job<mining_boost::Plan>> boost;
    int boostBase = -1;
    std::shared_ptr<boost_compute::Job<return_boost::Plan>> cargo;
    int cargoBase = -1;
    std::shared_ptr<boost_compute::Job<cycle_boost::Plan>> cycle;
    int cycleBase = -1;
};
std::unique_ptr<Game> g;

std::vector<uint8_t> fixChk(std::vector<uint8_t> d) {  // see minesim: STR + VER 206
    bool hasStr = false;
    for (size_t o = 0; o + 8 <= d.size();) {
        uint32_t len; memcpy(&len, &d[o + 4], 4);
        if (!memcmp(&d[o], "STR ", 4)) hasStr = true;
        if (!memcmp(&d[o], "VER ", 4) && len >= 2 && d[o + 8] == 206 && d[o + 9] == 0) d[o + 8] = 205;
        if (len > d.size() - o - 8) break;
        o += 8 + len;
    }
    if (!hasStr) { const uint8_t s[] = {'S', 'T', 'R', ' ', 2, 0, 0, 0, 0, 0}; d.insert(d.end(), s, s + sizeof s); }
    return d;
}

template <class F> void forChunks(const std::vector<uint8_t> &d, F f) {
    for (size_t o = 0; o + 8 <= d.size();) {
        uint32_t len; memcpy(&len, &d[o + 4], 4);
        if (len > d.size() - o - 8) break;
        f(std::string((const char *)&d[o], 4), &d[o + 8], len);
        o += 8 + len;
    }
}

std::vector<Base> findBases(const std::vector<uint8_t> &chk) {
    uint8_t side[12] = {};
    forChunks(chk, [&](const std::string &tag, const uint8_t *p, uint32_t len) { if (tag == "SIDE" && len >= 12) memcpy(side, p, 12); });
    std::vector<Base> r;
    forChunks(chk, [&](const std::string &tag, const uint8_t *p, uint32_t len) {
        if (tag != "UNIT") return;
        for (uint32_t u = 0; u + 36 <= len; u += 36) {
            uint16_t x, y, type; memcpy(&x, p + u + 4, 2); memcpy(&y, p + u + 6, 2); memcpy(&type, p + u + 8, 2);
            int owner = p[u + 16];
            int race = -1;
            if (type == 106) race = 1;
            else if (type == 131 || type == 132 || type == 133) race = 0;
            else if (type == 154) race = 2;
            if (type == 214) {
                Base b{x, y, owner, true, owner < 12 && side[owner] <= 2 ? side[owner] : 2};
                r.push_back(b);
            } else if (race >= 0) {
                bool dup = false;
                for (Base &b : r) if (std::abs(b.x - x) < 96 && std::abs(b.y - y) < 96) { dup = true; b.race = race; b.start = false; }
                if (!dup) r.push_back(Base{x, y, owner, false, race});
            }
        }
    });
    return r;
}

// ---- engine --------------------------------------------------------------------------------------
struct Funcs : game_load_functions {
    explicit Funcs(state &st) : game_load_functions(st) {}
};

bool loadGame(Game &G) {
    G.game = std::make_unique<game_state>();
    G.st = std::make_unique<state>();
    state &st = *G.st;
    st.global = g_gst.get(); st.game = G.game.get();
    Funcs load(st);
    std::vector<uint8_t> data = G.chk;
    simulation_map::load(load, data, [&]() {
        // every player slot neutral during load: no melee starts; bases are spawned on demand
        for (size_t i = 0; i < 8; i++) st.players[i].controller = player_t::controller_neutral;
        load.setup_info.victory_condition = 2;
        load.setup_info.starting_units = 1;
    });
    for (size_t i = 0; i < 8; i++) st.players[i].controller = player_t::controller_occupied;
    G.tileset = G.game->tileset_index;
    return true;
}

unit_t *depotOf(state &st, int slot) {
    Funcs f(st);
    for (unit_t *u : ptr(st.player_units[slot])) if (f.ut_resource_depot(u)) return u;
    return nullptr;
}

// Nearby depots can share resources and worker paths. Keep their searches conservative,
// but preserve a distant base's private snapshot when another expansion changes.
bool basesCanInteract(const Base &a, const Base &b) {
    if (&a == &b) return true;
    auto bounds = [](const Base &base) {
        // Resources are assigned within 12 tiles; leave another three tiles for detours,
        // depot/worker extents and freshly spawned units before assignments exist.
        RECT r{base.x - 480, base.y - 480, base.x + 481, base.y + 481};
        auto include = [&](xy p) {
            r.left = std::min<LONG>(r.left, p.x - 96); r.right = std::max<LONG>(r.right, p.x + 97);
            r.top = std::min<LONG>(r.top, p.y - 96); r.bottom = std::max<LONG>(r.bottom, p.y + 97);
        };
        for (auto *u : base.fields) if (u->sprite) include(u->sprite->position);
        for (auto *u : base.refineries) if (u->sprite) include(u->sprite->position);
        for (auto a : base.assigned) if (a.first->sprite) include(a.first->sprite->position);
        return r;
    };
    RECT x = bounds(a), y = bounds(b), overlap;
    return IntersectRect(&overlap, &x, &y) != FALSE;
}
void invalidateBoostSearches(const Base &changed) {
    for (auto &b:g->bases) if(basesCanInteract(changed,b)) {
        b.positionSession.reset(); b.cargoSession.reset(); b.cycleSession.reset();
        b.pendingPositions.clear();
    }
    if (g->boost && basesCanInteract(changed, g->bases[g->boostBase])) {
        Base &b = g->bases[g->boostBase]; b.boostPending = false;
        logf("live boost: discarding outdated nearby search at %d,%d after change at %d,%d", b.x, b.y, changed.x, changed.y);
        g->boost.reset(); g->boostBase = -1;
    }
    if (g->cargo && basesCanInteract(changed, g->bases[g->cargoBase])) {
        Base &b = g->bases[g->cargoBase]; b.cargoPending = false;
        logf("live cargo: discarding outdated nearby search at %d,%d after change at %d,%d", b.x, b.y, changed.x, changed.y);
        g->cargo.reset(); g->cargoBase = -1;
    }
    if (g->cycle && basesCanInteract(changed, g->bases[g->cycleBase])) {
        Base &b = g->bases[g->cycleBase]; b.cyclePending = false;
        logf("live cycle: discarding outdated nearby search at %d,%d after change at %d,%d", b.x, b.y, changed.x, changed.y);
        g->cycle.reset(); g->cycleBase = -1;
    }
}

// order a worker to the resource with the fewest workers we assigned (closest first on ties) - the way a
// player splits; the engine then bounces mineral workers off busy fields on its own
void assignWorker(state &st, Base &b, unit_t *w, bool gas) {
    Funcs f(st);
    size_t n = gas ? b.refineries.size() : b.fields.size();
    if (!n) return;
    std::vector<int> load(n);
    for (auto &a : b.assigned) {
        if (gas && a.second < 0) load[-1 - a.second]++;
        if (!gas && a.second >= 0) load[a.second]++;
    }
    size_t best = 0;
    for (size_t i = 1; i < n; i++) if (load[i] < load[best]) best = i;
    unit_t *target = gas ? b.refineries[best] : b.fields[best];
    f.issue_order(w, false, f.get_order_type(Orders::Harvest1), {target->sprite->position, target});
    b.assigned.push_back({w, gas ? -1 - (int)best : (int)best});
    (gas ? b.gasWorkers : b.workers)++;
}

void addWorker(Base &b, bool gas) {
    invalidateBoostSearches(b);
    state &st = *g->st;
    Funcs f(st);
    unit_t *depot = depotOf(st, b.slot);
    if (!depot || (gas && b.refineries.empty())) return;
    UnitTypes wt = b.race == 0 ? UnitTypes::Zerg_Drone : b.race == 1 ? UnitTypes::Terran_SCV : UnitTypes::Protoss_Probe;
    unit_t *w = f.create_starting_unit(f.get_unit_type(wt), depot->sprite->position, b.slot);
    if (w) assignWorker(st, b, w, gas);
}

void removeWorker(Base &b, bool gas) {
    invalidateBoostSearches(b);
    state &st = *g->st;
    Funcs f(st);
    for (size_t i = b.assigned.size(); i-- > 0;) {  // the newest worker of that kind
        if ((b.assigned[i].second < 0) != gas) continue;
        unit_t *victim = b.assigned[i].first;
        b.assigned.erase(b.assigned.begin() + i);
        b.lastDelivery.erase(victim);
        f.destroy_unit(victim);
        (gas ? b.gasWorkers : b.workers)--;
        return;
    }
}

// A geyser becomes this player's completed refinery - what state_functions::build_refinery does.
unit_t *makeRefinery(Base &b, unit_t *geyser) {
    Funcs f(*g->st);
    UnitTypes ref = b.race == 0 ? UnitTypes::Zerg_Extractor : b.race == 1 ? UnitTypes::Terran_Refinery : UnitTypes::Protoss_Assimilator;
    f.add_completed_unit(geyser, -1, true);
    f.u_unset_status_flag(geyser, unit_t::status_flag_completed);
    f.set_unit_owner(geyser, b.slot, false);
    f.set_sprite_owner(geyser, b.slot);
    f.morph_unit(geyser, f.get_unit_type(ref));
    f.create_image(f.get_image_type(ImageTypes::IMAGEID_Vespene_Geyser2), geyser->sprite, {}, f.image_order_below);
    f.finish_building_unit(geyser);
    f.complete_unit(geyser);
    return geyser;
}

// ...and back to a neutral geyser - the refinery branch of destroy_unit_impl, without removing the sprite.
void revertRefinery(unit_t *u) {
    Funcs f(*g->st);
    if (!f.unit_is_refinery(u) || !u->sprite) return;
    if (f.unit_is(u, UnitTypes::Zerg_Extractor)) f.remove_extractor_creep(u, u->sprite->position, true);
    if (f.u_completed(u)) f.add_completed_unit(u, -1, false);
    f.u_unset_status_flag(u, unit_t::status_flag_completed);
    f.morph_unit(u, f.get_unit_type(UnitTypes::Resource_Vespene_Geyser));
    u->order_type = u->unit_type->human_ai_idle;
    u->order_target = {};
    u->order_state = 0;
    u->order_unit_type = nullptr;
    u->hp = u->unit_type->hitpoints;
    f.set_unit_owner(u, 11, false);
    f.set_sprite_owner(u, 11);
    f.u_set_status_flag(u, unit_t::status_flag_completed);
    f.add_completed_unit(u, 1, true);
}

void stopBase(Base &b) {
    if (!b.active) return;
    invalidateBoostSearches(b);
    state &st = *g->st;
    Funcs f(st);
    b.boostPending = false; b.appliedPositions.clear(); b.ghosts.clear(); b.boostedFields = b.fasterFields = 0;
    b.cargoPending = false; b.cargoRules.clear(); b.cargoMarked.clear();
    b.cyclePending = false; b.cycleRules.clear(); b.cycleMarked.clear();
    std::vector<unit_t *> workers, others;
    for (unit_t *u : ptr(st.player_units[b.slot])) {
        if (f.unit_is_refinery(u)) continue;
        (f.ut_worker(u) ? workers : others).push_back(u);
    }
    for (unit_t *u : workers) if (u->sprite) f.destroy_unit(u);  // workers first: some are inside refineries
    for (unit_t *r : b.refineries) revertRefinery(r);
    for (unit_t *u : others) if (u->sprite) f.destroy_unit(u);
    b.active = false; b.workers = b.gasWorkers = 0;
    b.assigned.clear(); b.lastDelivery.clear();
    b.deliveries.clear(); b.gasDeliveries.clear();
    b.fieldDeliveries.clear(); b.refineryDeliveries.clear(); b.fieldCycles.clear();
    b.fields.clear(); b.refineries.clear();
}

void startBase(Base &b, int workers, int gasWorkers) {
    state &st = *g->st;
    Funcs f(st);
    // a free player slot (prefer the base's own owner, for its colour)
    std::vector<bool> used(8, false);
    for (Base &o : g->bases) if (o.active && o.slot >= 0) used[o.slot] = true;
    b.slot = b.owner < 8 && !used[b.owner] ? b.owner : -1;
    for (int i = 0; i < 8 && b.slot < 0; i++) if (!used[i]) b.slot = i;
    if (b.slot < 0) return;
    invalidateBoostSearches(b);
    st.players[b.slot].race = (race_t)b.race;
    f.create_starting_units(b.slot, xy(b.x, b.y), (race_t)b.race);
    unit_t *depot = depotOf(st, b.slot);
    if (!depot) { logf("live: no depot at %d,%d", b.x, b.y); return; }
    auto dist = [&](unit_t *u) { return f.xy_length(u->sprite->position - depot->sprite->position); };
    b.fields.clear(); b.refineries.clear();
    std::vector<unit_t *> geysers;
    for (unit_t *u : ptr(st.visible_units)) {
        if (dist(u) > 32 * 12) continue;
        if (f.unit_is_mineral_field(u)) {
            // Give long animated tests a full reserve in the private simulation only.
            f.set_unit_resources(u,50000);
            b.fields.push_back(u);
        }
        else if (f.unit_is(u, UnitTypes::Resource_Vespene_Geyser)) geysers.push_back(u);
    }
    std::sort(b.fields.begin(), b.fields.end(), [&](unit_t *a, unit_t *c) { return dist(a)!=dist(c) ? dist(a)<dist(c) : a->index<c->index; });
    std::sort(geysers.begin(), geysers.end(), [&](unit_t *a, unit_t *c) { return dist(a)!=dist(c) ? dist(a)<dist(c) : a->index<c->index; });
    for (unit_t *gy : geysers) b.refineries.push_back(makeRefinery(b, gy));
    b.fieldDeliveries.assign(b.fields.size(), {});
    b.fieldCycles.assign(b.fields.size(), {});
    b.refineryDeliveries.assign(b.refineries.size(), {});
    b.assigned.clear(); b.lastDelivery.clear();
    b.deliveries.clear(); b.gasDeliveries.clear();
    b.active = true;
    b.startFrame = st.current_frame;
    // workers from create_starting_units (4) go to minerals, then up to the wanted counts
    b.workers = b.gasWorkers = 0;
    std::vector<unit_t *> ws;
    for (unit_t *u : ptr(st.player_units[b.slot])) if (f.ut_worker(u)) ws.push_back(u);
    for (unit_t *w : ws) assignWorker(st, b, w, false);
    if (workers <= 0) workers = (int)b.fields.size();
    if (gasWorkers < 0) gasWorkers = 3 * (int)b.refineries.size();
    while (b.workers < workers) { int before = b.workers; addWorker(b, false); if (b.workers == before) break; }
    while (b.workers > workers) removeWorker(b, false);
    while (b.gasWorkers < gasWorkers) { int before = b.gasWorkers; addWorker(b, true); if (b.gasWorkers == before) break; }
    b.boostPending = false;
    b.cargoPending = false;
    b.cyclePending = false;
    logf("live: base at %d,%d started as %s, player %d, %d fields, %d geysers, %d + %d workers", b.x, b.y,
         b.race == 0 ? "zerg" : b.race == 1 ? "terran" : "protoss", b.slot, (int)b.fields.size(), (int)b.refineries.size(), b.workers, b.gasWorkers);
}

// Snapshot the worker's current body frame. These are drawing data only, never simulated units;
// their frame and map position remain fixed while the real worker continues mining.
void captureGhost(Base &b, unit_t *w, const xy *at = nullptr, int kind=0, const xy *target=nullptr, int stage=0) {
    if (!w || !w->sprite) return;
    Funcs f(*g->st);
    std::vector<uint8_t> palette;
    if (!scmhdCascRead(std::string("TileSet\\") + kTilesetNames[g->tileset] + ".wpe", palette) || palette.size() < 1024) return;
    const uint8_t *pc = g_img->player_unit_colors.at(g->st->players[w->owner].color).data();
    // Field assignment keeps a worker's guides the same hue across both commands,
    // cargo markers, graphics modes and subsequent search slices.
    static constexpr COLORREF colors[]={RGB(255,205,80),RGB(80,220,255),RGB(255,125,205),
        RGB(125,235,125),RGB(185,150,255),RGB(255,155,80),RGB(100,160,255),
        RGB(210,235,90),RGB(255,110,115)};
    size_t colorIndex=w->index;
    for(const auto &a:b.assigned) if(a.first==w && a.second>=0) {colorIndex=size_t(a.second); break;}
    COLORREF guideColor=colors[colorIndex%(sizeof(colors)/sizeof(colors[0]))];
    bool annotate=true;
    for (const image_t *img : ptr(reverse(w->sprite->images))) {
        if (f.i_flag(img, image_t::flag_hidden) || !img->grp || (img->modifier != 0 && img->modifier != 1)) continue;
        auto &frame = img->grp->frames.at(img->frame_index);
        Ghost ghost{f.get_image_map_position(img), frame.size.x, frame.size.y, {}}; ghost.kind=kind;
        ghost.commandAt=at?*at:w->position; ghost.target=target?*target:xy{};
        ghost.stage=stage; ghost.annotate=annotate; ghost.worker=w->index; ghost.guideColor=guideColor;
        if (at) ghost.origin += *at - w->sprite->position;
        ghost.pixels.assign((size_t)ghost.width * ghost.height, 0);
        std::vector<uint8_t> indices(ghost.pixels.size(), 0);
        bwgame::draw_frame(frame, f.i_flag(img, image_t::flag_horizontally_flipped), indices.data(), ghost.width,
                          0, 0, ghost.width, ghost.height, [pc](uint8_t v, uint8_t) { return v >= 8 && v < 16 ? pc[v - 8] : v; });
        constexpr uint32_t alpha = 112;
        for (size_t p = 0; p < indices.size(); ++p) if (indices[p]) {
            const uint8_t *c = &palette[indices[p] * 4];
            ghost.pixels[p] = alpha << 24 | (c[0] * alpha / 255) << 16 | (c[1] * alpha / 255) << 8 | (c[2] * alpha / 255);
        }
        Ghost normal = std::move(ghost);
        for (int mode = 0; mode < 3; ++mode) {
            Ghost ghost = normal; ghost.mode = mode;
            hd_anim::Rect bounds;
            const uint8_t *tint = &palette[pc[0] * 4];
            if (mode && hd_sprites::snapshot(mode, (unsigned)img->image_type->id, img->frame_index,
                    f.i_flag(img, image_t::flag_horizontally_flipped), tint[0] << 16 | tint[1] << 8 | tint[2],
                    alpha, bounds, ghost.pixels)) {
                ghost.origin = f.get_image_center_map_position(img);
                if (at) ghost.origin += *at - w->sprite->position;
                ghost.offset4 = {bounds.x, bounds.y}; ghost.scale = 4;
                ghost.width = bounds.w; ghost.height = bounds.h;
            }
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = ghost.width; bi.bmiHeader.biHeight = -ghost.height;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            void *bits = nullptr;
            HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bm && bits) {
                memcpy(bits, ghost.pixels.data(), ghost.pixels.size() * sizeof(uint32_t));
                ghost.bitmap = std::shared_ptr<void>(bm, [](void *p) { DeleteObject(static_cast<HBITMAP>(p)); });
                ghost.pixels.clear();
                b.ghosts.push_back(std::move(ghost));
            } else if (bm) DeleteObject(bm);
        }
        annotate=false;
    }
}

// Enough context to distinguish a different engine start from an identical search rerun.
// Unit order matters: OpenBW redistributes processing timers in visible-list order.
void logSearchContext(const char *kind, const Base &b, const std::vector<mining_boost::Assignment> &a, const state *snapshot=nullptr) {
    const state &st = snapshot ? *snapshot : *g->st;
    uint64_t order = 1469598103934665603ULL;
    for (const unit_t *u : ptr(st.visible_units)) {
        order ^= u->index; order *= 1099511628211ULL;
    }
    logf("live %s: input frame=%d order-phase=%d rng=%u unit-order=%llx base=%d,%d player=%d",
         kind, st.current_frame, st.order_timer_counter, st.lcg_rand_state,
         (unsigned long long)order, b.x, b.y, b.slot);
    Funcs f(const_cast<state &>(st));
    for (const auto &v : a) {
        const auto *w = f.get_unit(v.worker), *m = f.get_unit(v.field);
        if (w && m) logf("live %s: input worker=%zu at %d,%d timer=%d field=%zu at %d,%d minerals=%d",
            kind, v.worker, w->position.x, w->position.y, w->order_process_timer,
            v.field, m->position.x, m->position.y, m->building.resource.resource_count);
    }
}

// Run a bounded amount of private simulation between UI messages. Live game time pauses while
// positions are searched/validated; none of the trial resources, income or frames enter the test.
bool advanceBoost() {
    if (!g->boost) {
        for (size_t i = 0; i < g->bases.size(); ++i) {
            Base &b = g->bases[i];
            if (!b.active || !b.boostPending) continue;
            std::vector<mining_boost::Assignment> assignments;
            std::vector<bool> seen(b.fields.size(), false);
            for (auto &a : b.assigned) if (a.second >= 0 && !seen[a.second]) {
                seen[a.second] = true; assignments.push_back({a.first->index, b.fields[a.second]->index});
            }
            logSearchContext("boost", b, assignments, b.positionSession?b.positionSession->input:nullptr);
            g->boostBase = (int)i;
            size_t count = assignments.size();
            if (b.positionSession) {
                g->boost=b.positionSession; g->boost->extend(b.searchSeconds[0]);
            } else g->boost = b.positionSession = std::make_shared<boost_compute::Job<mining_boost::Plan>>(*g->st,
                [a = std::move(assignments)](const state &st, boost_compute::Executor *pool) {
                    return std::make_unique<mining_boost::Plan>(st, a, pool);
                }, b.searchSeconds[0]);
            logf("live boost: searching %zu patches at %d,%d", count, b.x, b.y);
            break;
        }
    }
    if (!g->boost) return false;
    if (g->boost->done()) {
        auto &plan = *g->boost->result;
        Base &b = g->bases[g->boostBase];
        if(!g->boost->changed.load()) {
            logf("live boost: slice complete, retained previous best (%.3f s)",g->boost->seconds());
            b.boostPending=false; g->boost.reset(); g->boostBase=-1; g->lastTick=GetTickCount(); return true;
        }
        Funcs f(*g->st);
        bool positionsChanged=b.boostedFields>0 || plan.selections()>0;
        if(positionsChanged) {
            b.cargoSession.reset(); b.cycleSession.reset(); b.cargoRules.clear(); b.cycleRules.clear();
            b.ghosts.clear(); b.cargoMarked.clear(); b.cycleMarked.clear();
        }
        auto previous=std::move(b.appliedPositions), pending=std::move(b.pendingPositions);
        b.appliedPositions.clear(); b.pendingPositions.clear();
        b.boostedFields=b.fasterFields=0;
        Funcs seed(*g->boost->input);
        for (size_t i = 0; i < plan.positions.size(); ++i) {
            auto &p = plan.positions[i];
            auto a = plan.assignments[i];
            logf("live boost: field %zu at %d,%d, normal %.2f -> %.2f frames, verified=%d", a.field,
                 p.position.x, p.position.y, plan.normalFrames[i], p.frames, p.stable);
            if (!p.stable) {
                // A better combined lineup may drop a formerly selected start. Restore that
                // worker's normal start too; do not leave an invisible old boost in live play.
                if(previous.count(a.worker)) {
                    auto *normal=seed.get_unit(a.worker);
                    if(normal && normal->sprite) b.pendingPositions[a.worker]=normal->position;
                } else if(pending.count(a.worker)) b.pendingPositions[a.worker]=pending.at(a.worker);
                continue;
            }
            b.appliedPositions[a.worker]=p.position;
            auto *w = f.get_unit(a.worker);
            // Search used this same initial worker and placement operation; do not create a new
            // unit here (its order-processing phase would change the route we measured).
            bool unchanged=previous.count(a.worker) && previous.at(a.worker)==p.position;
            if(unchanged) {
                if(pending.count(a.worker)) b.pendingPositions[a.worker]=p.position;
            } else if (w && !(w->carrying_flags&3) && !f.is_blocked(w,p.position).first) {
                f.move_unit(w,p.position);
                auto *m=f.get_unit(a.field);
                f.issue_order(w,false,f.get_order_type(Orders::Harvest1),{m->position,m});
            } else b.pendingPositions[a.worker]=p.position;
            ++b.boostedFields;
            const auto &m=plan.selected[i], &control=plan.control[i];
            if (control.mean()-m.mean()>0.5 && control.mean(1)-m.mean(1)>0.1 &&
                control.mean(2)-m.mean(2)>0.1) {
                ++b.fasterFields; captureGhost(b,w,&p.position);
            }
        }
        b.boostPending = false;
        logf("live boost: ready, %d/%zu verified positions (%d faster), %d rejected in joint validation (%.3f s, %zu threads)", b.boostedFields,
             plan.positions.size(), b.fasterFields, plan.rejected, g->boost->seconds(), g->boost->executor.threads.size());
        g->boost.reset(); g->boostBase = -1;
    }
    g->lastTick = GetTickCount();
    return true;
}

bool advanceCargo() {
    if (!g->cargo) {
        for (size_t i = 0; i < g->bases.size(); ++i) {
            Base &b = g->bases[i];
            if (!b.active || !b.cargoPending || b.boostPending || !b.pendingPositions.empty()) continue;
            std::vector<mining_boost::Assignment> assignments;
            for (auto &a : b.assigned) if (a.second >= 0)
                assignments.push_back({a.first->index, b.fields[a.second]->index});
            std::vector<return_boost::Rule> background;
            for (const Base &o : g->bases) if (o.active && &o != &b) {
                for (auto r : o.cargoRules) if (r.enabled) background.push_back(r);
                for (auto r : o.cycleRules) if (r.enabled) background.push_back(r);
            }
            logSearchContext("cargo", b, assignments, b.cargoSession?b.cargoSession->input:nullptr);
            g->cargoBase = (int)i;
            size_t count = assignments.size();
            if (b.cargoSession) {
                g->cargo=b.cargoSession; g->cargo->extend(b.searchSeconds[1]);
            } else g->cargo = b.cargoSession = std::make_shared<boost_compute::Job<return_boost::Plan>>(*g->st,
                [a = std::move(assignments), other = std::move(background)](const state &st, boost_compute::Executor *pool) {
                    return std::make_unique<return_boost::Plan>(st, a, other, pool);
                }, b.searchSeconds[1]);
            logf("live cargo: searching %zu workers at %d,%d (position boost %d)",
                 count, b.x, b.y, b.boostEnabled);
            break;
        }
    }
    if (!g->cargo) return false;
    if (g->cargo->done()) {
        auto &plan = *g->cargo->result;
        Base &b = g->bases[g->cargoBase];
        if(!g->cargo->changed.load()) {
            logf("live cargo: slice complete, retained previous best (%.3f s)",g->cargo->seconds());
            b.cargoPending=false; g->cargo.reset(); g->cargoBase=-1; g->lastTick=GetTickCount(); return true;
        }
        auto activeKey=[](std::vector<return_boost::Rule> rules) {
            rules.erase(std::remove_if(rules.begin(),rules.end(),[](const auto &r){return !r.enabled;}),rules.end());
            return return_boost::Plan::ruleKey(rules);
        };
        if(activeKey(b.cargoRules)!=activeKey(plan.chosen)) {
            b.cycleSession.reset(); b.cycleRules.clear(); b.cycleMarked.clear(); b.cargoMarked.clear();
            b.ghosts.erase(std::remove_if(b.ghosts.begin(),b.ghosts.end(),[](const auto &v){return v.kind>=1;}),b.ghosts.end());
            b.cargoRules=plan.chosen;
        }
        int accepted = 0;
        for (size_t i = 0; i < b.cargoRules.size(); ++i) {
            const auto &r = b.cargoRules[i]; const auto &m = plan.selected[i];
            if (!r.enabled) continue;
            ++accepted;
            logf("live cargo: worker %zu field %zu, spot %d,%d pickup delay %d, %.3f -> %.3f frames, %.3f -> %.3f px",
                 r.worker, r.field, r.spot.x, r.spot.y, r.cargoDelay, plan.baseline[i].mean(), m.mean(),
                 plan.baseline[i].path(), m.path());
        }
        logf("live cargo: ready, %d/%zu verified boosts (%.3f s, %zu threads)", accepted, b.cargoRules.size(),
             g->cargo->seconds(), g->cargo->executor.threads.size());
        b.cargoPending = false;
        g->cargo.reset(); g->cargoBase = -1;
    }
    g->lastTick = GetTickCount();
    return true;
}

bool advanceCycle() {
    if (!g->cycle) {
        for (size_t i = 0; i < g->bases.size(); ++i) {
            Base &b = g->bases[i];
            if (!b.active || !b.cyclePending || b.boostPending || b.cargoPending || !b.pendingPositions.empty()) continue;
            std::vector<mining_boost::Assignment> assignments;
            for (auto &a : b.assigned) if (a.second >= 0)
                assignments.push_back({a.first->index, b.fields[a.second]->index});
            std::vector<cycle_boost::Rule> background;
            for (const Base &o : g->bases) if (o.active) {
                for (auto r : o.cargoRules) if (r.enabled) background.push_back(r);
                if (&o != &b) for (auto r : o.cycleRules) if (r.enabled) background.push_back(r);
            }
            logSearchContext("cycle", b, assignments, b.cycleSession?b.cycleSession->input:nullptr);
            g->cycleBase = (int)i;
            size_t count = assignments.size();
            if (b.cycleSession) {
                g->cycle=b.cycleSession; g->cycle->extend(b.searchSeconds[2]);
            } else g->cycle = b.cycleSession = std::make_shared<boost_compute::Job<cycle_boost::Plan>>(*g->st,
                [a = std::move(assignments), other = std::move(background)](const state &st, boost_compute::Executor *pool) {
                    return std::make_unique<cycle_boost::Plan>(st, a, other, pool);
                }, b.searchSeconds[2]);
            logf("live cycle: searching %zu workers at %d,%d (position boost %d)",
                 count, b.x, b.y, b.boostEnabled);
            break;
        }
    }
    if (!g->cycle) return false;
    if (g->cycle->done()) {
        auto &plan = *g->cycle->result;
        Base &b = g->bases[g->cycleBase];
        if(!g->cycle->changed.load()) {
            logf("live cycle: slice complete, retained previous best (%.3f s)",g->cycle->seconds());
            b.cyclePending=false; g->cycle.reset(); g->cycleBase=-1; g->lastTick=GetTickCount(); return true;
        }
        b.cycleRules = plan.chosen; b.cycleMarked.clear();
        b.ghosts.erase(std::remove_if(b.ghosts.begin(),b.ghosts.end(),[](const auto &v){return v.kind==2;}),b.ghosts.end());
        int accepted = 0;
        for (size_t i = 0; i < b.cycleRules.size(); ++i) {
            const auto &r = b.cycleRules[i]; const auto &m = plan.selected[i];
            if (!r.enabled) continue;
            ++accepted;
            logf("live cycle: worker %zu field %zu via %zu, delay %d detour %d, %.3f -> %.3f frames, %.3f -> %.3f px",
                 r.worker, r.field, r.neighbor, r.delay, r.detour, plan.baseline[i].mean(), m.mean(),
                 plan.baseline[i].path(), m.path());
        }
        logf("live cycle: ready, %d/%zu verified boosts (%.3f s, %zu threads)", accepted, b.cycleRules.size(),
             g->cycle->seconds(), g->cycle->executor.threads.size());
        b.cyclePending = false;
        g->cycle.reset(); g->cycleBase = -1;
    }
    g->lastTick = GetTickCount();
    return true;
}

void step(int frames) {
    state &st = *g->st;
    Funcs f(st);
    for (int i = 0; i < frames; i++) {
        for (Base &b:g->bases) if(b.active) for(auto it=b.pendingPositions.begin();it!=b.pendingPositions.end();) {
            auto *w=f.get_unit(it->first);
            if(!w || !w->sprite) {it=b.pendingPositions.erase(it); continue;}
            if((w->carrying_flags&3) || f.is_blocked(w,it->second).first) {++it; continue;}
            auto a=std::find_if(b.assigned.begin(),b.assigned.end(),[&](const auto &v){return v.first==w && v.second>=0;});
            if(a!=b.assigned.end()) {
                auto *m=b.fields[a->second]; f.move_unit(w,it->second);
                f.issue_order(w,false,f.get_order_type(Orders::Harvest1),{m->position,m});
            }
            it=b.pendingPositions.erase(it);
        }
        for (Base &b : g->bases) if (b.active && b.cargoEnabled) for (auto &r : b.cargoRules) {
            if (!r.apply(f)) continue;
            unit_t *w = f.get_unit(r.worker);
            if (b.cargoMarked.insert(r.worker).second) {
                captureGhost(b, w, &r.issuedAt, 1);
                logf("live cargo: marker for worker %zu at %d,%d", r.worker, r.issuedAt.x, r.issuedAt.y);
            }
        }
        for (Base &b : g->bases) if (b.active && b.cycleEnabled) for (auto &r : b.cycleRules) {
            if (!r.apply(f)) continue;
            if (b.cycleMarked.insert({r.worker, r.stage}).second) {
                auto *target=f.get_unit(r.stage==1?r.neighbor:r.field);
                if(target && target->sprite) captureGhost(b,f.get_unit(r.worker),nullptr,2,&target->sprite->position,r.stage);
                logf("live cycle: marker for worker %zu command %d at %d,%d", r.worker, r.stage,
                     f.get_unit(r.worker)->position.x, f.get_unit(r.worker)->position.y);
            }
        }
        // carrying state before the frame, to detect deliveries
        std::vector<std::pair<unit_t *, int>> carrying;
        for (Base &b : g->bases)
            if (b.active)
                for (unit_t *u : ptr(st.player_units[b.slot])) if (f.ut_worker(u)) carrying.push_back({u, u->carrying_flags});
        f.next_frame();
        int now = st.current_frame;
        for (auto &c : carrying) {
            unit_t *w = c.first;
            bool minerals = (c.second & 2) && !(w->carrying_flags & 2), gas = (c.second & 1) && !(w->carrying_flags & 1);
            if ((!minerals && !gas) || !w->sprite) continue;
            for (Base &b : g->bases) {
                if (!b.active || w->owner != b.slot) continue;
                unit_t *res = w->worker.target_resource_unit;
                if (gas) {
                    b.gasDeliveries.push_back(now);
                    for (size_t k = 0; k < b.refineries.size(); k++) if (res == b.refineries[k]) b.refineryDeliveries[k].push_back(now);
                    continue;
                }
                b.deliveries.push_back(now);
                int field = -1;
                for (size_t k = 0; k < b.fields.size(); k++) if (res == b.fields[k]) field = (int)k;
                if (field < 0) continue;
                b.fieldDeliveries[field].push_back(now);
                // one mining cycle = this worker's previous delivery from the same field to this one
                auto it = b.lastDelivery.find(w);
                if (it != b.lastDelivery.end() && it->second.second == field) {
                    auto &cy = b.fieldCycles[field];
                    cy.push_back(now - it->second.first);
                    if (cy.size() > 10) cy.pop_front();
                }
                b.lastDelivery[w] = {now, field};
            }
        }
    }
    int now = st.current_frame;
    auto trim = [now](std::deque<int> &d) { while (!d.empty() && d.front() <= now - kWindow) d.pop_front(); };
    for (Base &b : g->bases) {
        trim(b.deliveries); trim(b.gasDeliveries);
        for (auto &d : b.fieldDeliveries) trim(d);
        for (auto &d : b.refineryDeliveries) trim(d);
    }
}

// resource per minute from deliveries in the last game minute (scaled while younger than that)
double rate(const std::deque<int> &d, const Base &b) {
    int now = g->st->current_frame, span = std::min(kWindow, now - b.startFrame);
    if (span < 24 * 8) return -1;  // not enough time yet
    return d.size() * 8.0 * kWindow / span;
}

double latestCycle(const std::deque<int> &c) {
    return c.empty() ? -1 : c.back();
}

// ---- public API (called from scmhd.cpp) ----------------------------------------------------------
bool g_enabled;
HWND g_main;
void g_buttons_clear();
constexpr UINT_PTR kTimerId = 0x5C4D;

bool enabled() { return g_enabled; }
bool running() {
    if (!g) return false;
    for (Base &b : g->bases) if (b.active) return true;
    return false;
}

void setEnabled(bool on, HWND mainWnd) {
    g_main = mainWnd;
    if (!on) {
        { Locked l; g_enabled = false; g.reset(); g_buttons_clear(); }
        KillTimer(mainWnd, kTimerId);
        RedrawWindow(mainWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        logf("live: test mining off");
        return;
    }
    std::string err;
    auto G = std::make_unique<Game>();
    if (!scmhdExportActiveMap(G->chk, err)) {
        MessageBoxA(mainWnd, err.c_str(), "Test Mining", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!loadData()) {
        MessageBoxA(mainWnd, "Could not load the StarCraft game data from the SC:R installation.", "Test Mining", MB_OK | MB_ICONERROR);
        return;
    }
    G->mapIdentity = scmhdActiveMap();
    G->bases = findBases(G->chk);
    G->chk = fixChk(G->chk);
    try {
        loadGame(*G);
    } catch (std::exception &e) {
        std::string m = std::string("The map could not be loaded into the game engine:\n") + e.what();
        MessageBoxA(mainWnd, m.c_str(), "Test Mining", MB_OK | MB_ICONERROR);
        return;
    }
    if (G->bases.empty()) {
        MessageBoxA(mainWnd, "This map has no start locations or resource depots to test.", "Test Mining", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        Locked l;  // the export above ran unlocked on purpose (it calls into ScmDraft)
        g = std::move(G);
        g_enabled = true;
    }
    g->lastTick = GetTickCount();
    SetTimer(mainWnd, kTimerId, 16, nullptr); // poll searches and animate at the selected playback speed
    logf("live: test mining on, %zu bases", g->bases.size());
    RedrawWindow(mainWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool searchPending() {
    for (const auto &b : g->bases)
        if (b.active && (b.boostPending || b.cargoPending || b.cyclePending)) return true;
    return false;
}

// Scale only elapsed wall time. Every engine frame and command still runs in order.
bool onTimer() {
    Locked l;
    if (!g) return false;
    try {
        if (advanceBoost() || advanceCargo() || advanceCycle()) {
            g->playback.reset();
            return true;
        }
    } catch (std::exception &e) {
        logf("live boost: search failed: %s", e.what());
        if (g->boostBase >= 0) {g->bases[g->boostBase].boostPending=false; g->bases[g->boostBase].positionSession.reset();}
        g->boost.reset(); g->boostBase = -1;
        if (g->cargoBase >= 0) {g->bases[g->cargoBase].cargoPending=false; g->bases[g->cargoBase].cargoSession.reset();}
        g->cargo.reset(); g->cargoBase = -1;
        if (g->cycleBase >= 0) {g->bases[g->cycleBase].cyclePending=false; g->bases[g->cycleBase].cycleSession.reset();}
        g->cycle.reset(); g->cycleBase = -1;
        g->lastTick = GetTickCount();
        g->playback.reset();
    }
    DWORD now = GetTickCount();
    int frames = g->playback.advance(now - g->lastTick);
    g->lastTick = now;
    if (frames <= 0) return false;
    if (!running()) return false;
    try {
        step(frames);
    } catch (std::exception &e) {
        logf("live: engine error: %s", e.what());
        for (Base &b : g->bases) b.active = false;
    }
    return true;
}

// Draw the active bases' sprites into an 8-bit buffer whose pixel (0, 0) is map pixel (originX, originY).
void drawSprites(void *back, uint8_t *buf, int pitch, int w, int h, int originX, int originY) {
    if (!g || !running()) return;
    state &st = *g->st;
    Funcs f(st);
    const bwgame::pcx_image *dk = dark(g->tileset);
    std::vector<std::pair<uint32_t, const sprite_t *>> sprites;
    for (Base &b : g->bases) {
        if (!b.active) continue;
        for (size_t y = 0; y < st.sprites_on_tile_line.size(); y++)
            for (const sprite_t *s : ptr(st.sprites_on_tile_line[y])) {
                if (s->owner != b.slot || f.s_hidden(s)) continue;
                uint32_t order = (uint32_t)s->elevation_level << 14 | (s->elevation_level <= 4 ? s->position.y : 0) << 1 | (f.s_flag(s, sprite_t::flag_turret) ? 1 : 0);
                sprites.push_back({order, s});
            }
    }
    std::sort(sprites.begin(), sprites.end());
    for (auto &sp : sprites) {
        const sprite_t *s = sp.second;
        size_t color = st.players[s->owner].color;
        for (const image_t *img : ptr(reverse(s->images))) {
            if (f.i_flag(img, image_t::flag_hidden) || !img->grp) continue;
            // plain, player-coloured and shadow images; glow / cloak / warp effects are skipped
            if (img->modifier != 0 && img->modifier != 1 && img->modifier != 10) continue;
            xy pos = f.get_image_map_position(img);
            int sx = pos.x - originX, sy = pos.y - originY;
            auto &frame = img->grp->frames.at(img->frame_index);
            int fw = frame.size.x, fh = frame.size.y;
            if (sx >= w || sy >= h || sx + fw <= 0 || sy + fh <= 0) continue;
            size_t ox = sx < 0 ? -sx : 0, oy = sy < 0 ? -sy : 0;
            size_t cw = std::min(fw, w - sx), ch = std::min(fh, h - sy);
            uint8_t *dst = buf + (ptrdiff_t)sy * pitch + sx;
            bool flip = f.i_flag(img, image_t::flag_horizontally_flipped);
            const uint8_t *pc = g_img->player_unit_colors.at(color).data();
            xy center = f.get_image_center_map_position(img);
            if (f.i_flag(img, image_t::flag_y_frozen)) center.y = sy + originY - int(frame.offset.y) + img->grp->height / 2;
            hd_sprites::drawLive(back, buf, pitch, (unsigned)img->image_type->id, img->frame_index,
                center.x - originX, center.y - originY, {sx, sy, fw, fh}, flip, pc, img->modifier == 10, [&] {
            if (img->modifier == 10) {
                if (!dk) return;
                const uint8_t *tab = &dk->data[256 * 18];
                bwgame::draw_frame(frame, flip, dst, pitch, ox, oy, cw, ch, [tab](uint8_t, uint8_t old) { return tab[old]; });
            } else {
                const uint8_t *pc = g_img->player_unit_colors.at(color).data();
                bwgame::draw_frame(frame, flip, dst, pitch, ox, oy, cw, ch, [pc](uint8_t v, uint8_t) {
                    return v >= 8 && v < 16 ? pc[v - 8] : v;
                });
            }
            });
        }
    }
}

// ---- on-map controls ----------------------------------------------------------------------------
// action: 0 start, 1 stop, 2/3 minerals -/+, 4/5 gas -/+, 6 display, 7 positions, 8 cargo, 10+race
struct Button { RECT r; int base; int action; };
bool g_showCycles = true;  // field labels: frames per mining cycle instead of minerals per minute
std::vector<Button> g_buttons;
HWND g_buttonsWnd;
void g_buttons_clear() { g_buttons.clear(); g_buttonsWnd = nullptr; }

// GDI primitives do not write alpha. Draw into a small clipped DIB, then premultiply
// its RGB coverage before compositing; guides never change the terrain/backbuffer.
template<class Draw>
void alphaGuide(HDC dc, RECT bounds, RECT viewport, BYTE alpha, Draw draw) {
    RECT r; if(!IntersectRect(&r,&bounds,&viewport)) return;
    int w=r.right-r.left,h=r.bottom-r.top;
    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void *bits=nullptr;
    HBITMAP bm=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    HDC tmp=CreateCompatibleDC(dc);
    if(bm && bits && tmp) {
        auto old=SelectObject(tmp,bm);
        memset(bits,0,size_t(w)*h*4);
        SetViewportOrgEx(tmp,-r.left,-r.top,nullptr);
        draw(tmp);
        GdiFlush();
        auto *pixels=static_cast<uint32_t *>(bits);
        for(size_t i=0;i<size_t(w)*h;++i) {
            uint32_t c=pixels[i];
            if(c&0xffffff) pixels[i]=(uint32_t(alpha)<<24) |
                (((c>>16)&255)*alpha/255<<16) | (((c>>8)&255)*alpha/255<<8) | ((c&255)*alpha/255);
        }
        SetViewportOrgEx(tmp,0,0,nullptr);
        AlphaBlend(dc,r.left,r.top,w,h,tmp,0,0,w,h,{AC_SRC_OVER,0,255,AC_SRC_ALPHA});
        SelectObject(tmp,old);
    }
    if(tmp) DeleteDC(tmp);
    if(bm) DeleteObject(bm);
}

void drawUI(HDC dc, HWND wnd, int xd, int yd, int wd, int hd, double xs, double ys, double ws, double hs, int originX, int originY) {
    if (!g_enabled || !g) return;
    g_buttons.clear(); g_buttonsWnd = wnd;
    static HFONT font = CreateFontW(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Tahoma");
    int saved = SaveDC(dc);
    IntersectClipRect(dc, xd, yd, xd + wd, yd + hd);
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    auto toScreen = [&](double mx, double my, int *sx, int *sy) {
        *sx = xd + (int)((mx - originX - xs) * wd / ws);
        *sy = yd + (int)((my - originY - ys) * hd / hs);
    };
    RECT client; GetClientRect(wnd,&client);
    // Solid guides link the two actual command positions, then the original patch.
    // The canceled remainder toward the neighbor is only a faint dotted intention.
    auto arrow=[&](xy from,xy to,COLORREF color,bool canceled) {
        int x,y,tx,ty; toScreen(from.x,from.y,&x,&y); toScreen(to.x,to.y,&tx,&ty);
        double length=std::hypot(double(tx-x),double(ty-y));
        if(length<1) return;
        double ux=(tx-x)/length,uy=(ty-y)/length;
        RECT bounds{std::min(x,tx)-10,std::min(y,ty)-10,std::max(x,tx)+11,std::max(y,ty)+11};
        alphaGuide(dc,bounds,client,canceled?85:180,[&](HDC tmp) {
            SetBkMode(tmp,TRANSPARENT);
            HPEN pen=CreatePen(canceled?PS_DOT:PS_SOLID,canceled?1:2,color); auto old=SelectObject(tmp,pen);
            MoveToEx(tmp,x,y,nullptr); LineTo(tmp,tx,ty);
            // Scale the head down for command positions only a few pixels apart.
            double head=std::min(9.0,length*0.45), wing=head*0.55;
            MoveToEx(tmp,tx-int(ux*head-uy*wing),ty-int(uy*head+ux*wing),nullptr); LineTo(tmp,tx,ty);
            LineTo(tmp,tx-int(ux*head+uy*wing),ty-int(uy*head-ux*wing));
            SelectObject(tmp,old); DeleteObject(pen);
        });
    };
    for(const Base &b:g->bases) if(b.active) for(const Ghost &ghost:b.ghosts) {
        if(ghost.mode!=scmhdGraphicsMode() || !ghost.annotate || ghost.kind!=2 || !g->guideVisible(ghost.kind)) continue;
        if(ghost.stage==1) {
            const Ghost *turn=nullptr;
            for(const Ghost &other:b.ghosts) if(other.kind==2 && other.stage==2 && other.annotate &&
                other.mode==ghost.mode && other.worker==ghost.worker) {turn=&other; break;}
            if(turn) {
                arrow(turn->commandAt,ghost.target,ghost.guideColor,true);
                arrow(ghost.commandAt,turn->commandAt,ghost.guideColor,false);
            } else arrow(ghost.commandAt,ghost.target,ghost.guideColor,true);
        } else arrow(ghost.commandAt,ghost.target,ghost.guideColor,false);
    }
    HDC ghostDC = CreateCompatibleDC(dc);
    if (ghostDC) {
        for (const Base &b : g->bases) if (b.active) for (const Ghost &ghost : b.ghosts) {
            if (ghost.mode != scmhdGraphicsMode() || !g->guideVisible(ghost.kind)) continue;
            int sx, sy, ex, ey;
            double gx = ghost.origin.x + ghost.offset4.x / 4.0, gy = ghost.origin.y + ghost.offset4.y / 4.0;
            toScreen(gx, gy, &sx, &sy);
            toScreen(gx + double(ghost.width) / ghost.scale, gy + double(ghost.height) / ghost.scale, &ex, &ey);
            if (ex <= xd || ey <= yd || sx >= xd + wd || sy >= yd + hd || ex <= sx || ey <= sy) continue;
            HGDIOBJ old = SelectObject(ghostDC, static_cast<HBITMAP>(ghost.bitmap.get()));
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, sx, sy, ex - sx, ey - sy, ghostDC, 0, 0, ghost.width, ghost.height, blend);
            SelectObject(ghostDC, old);
        }
        DeleteDC(ghostDC);
    }
    std::vector<overlay_layout::Rect> placed;
    for(const Base &b:g->bases) if(b.active) for(const Ghost &ghost:b.ghosts) {
        if(ghost.mode!=scmhdGraphicsMode() || !ghost.annotate || !ghost.kind || !g->guideVisible(ghost.kind)) continue;
        int x,y; toScreen(ghost.commandAt.x,ghost.commandAt.y,&x,&y);
        // Small screen-sized badges remain readable at every terrain zoom.
        if(ghost.kind==1) {x-=22; y-=18;}
        else {x+=6; y+=ghost.stage==1?-18:6;}
        RECT bounds{x-2,y-2,x+18,y+17};
        placed.push_back({bounds.left,bounds.top,bounds.right,bounds.bottom});
        alphaGuide(dc,bounds,client,210,[&](HDC tmp) {
            COLORREF color=ghost.guideColor;
            HPEN pen=CreatePen(PS_SOLID,2,color); auto old=SelectObject(tmp,pen);
            HBRUSH brush=CreateSolidBrush(RGB(20,25,35)); auto oldBrush=SelectObject(tmp,brush);
            RoundRect(tmp,x-1,y-1,x+16,y+15,4,4);
            if(ghost.kind==1) {
                // Bent return arrow, plus a mineral diamond: Return Cargo.
                MoveToEx(tmp,x+12,y+10,nullptr); LineTo(tmp,x+12,y+4); LineTo(tmp,x+3,y+4);
                MoveToEx(tmp,x+6,y+1,nullptr); LineTo(tmp,x+3,y+4); LineTo(tmp,x+6,y+7);
                POINT diamond[]={{x+5,y+8},{x+8,y+11},{x+5,y+13},{x+2,y+11}};
                Polygon(tmp,diamond,4);
            } else {
                auto oldFont=SelectObject(tmp,GetStockObject(DEFAULT_GUI_FONT));
                SetBkMode(tmp,TRANSPARENT); SetTextColor(tmp,color);
                const char *number=ghost.stage==1?"1":"2";
                TextOutA(tmp,x+4,y,number,1);
                SelectObject(tmp,oldFont);
            }
            SelectObject(tmp,oldBrush); DeleteObject(brush);
            SelectObject(tmp,old); DeleteObject(pen);
        });
    }
    struct Label { int x, y; std::string text; COLORREF color; overlay_layout::Rect rect{}; };
    std::vector<Label> labels;
    struct PanelBox { RECT r; std::string text; COLORREF bg,fg; int base,action; };
    struct PanelFill { RECT r; COLORREF color; };
    std::vector<PanelBox> panelBoxes;
    std::vector<PanelFill> panelFills;
    bool collecting=true;
    auto drawBox=[&](const PanelBox &item) {
        RECT r=item.r;
        placed.push_back({r.left,r.top,r.right,r.bottom});
        HBRUSH br=CreateSolidBrush(item.bg); FillRect(dc,&r,br); DeleteObject(br);
        if(item.action>=0) {HBRUSH fr=CreateSolidBrush(RGB(200,200,200)); FrameRect(dc,&r,fr); DeleteObject(fr);}
        SetTextColor(dc,item.fg);
        TextOutA(dc,r.left+4,r.top+2,item.text.c_str(),(int)item.text.size());
        if(item.action>=0) g_buttons.push_back({r,item.base,item.action});
    };
    auto box = [&](int x, int y, const char *text, COLORREF bg, COLORREF fg, int baseIdx, int action, int minWidth=0) {
        SIZE sz; GetTextExtentPoint32A(dc,text,(int)strlen(text),&sz);
        PanelBox item{{x,y,x+std::max(int(sz.cx)+8,minWidth),y+sz.cy+4},text,bg,fg,baseIdx,action};
        if(collecting) panelBoxes.push_back(item); else drawBox(item);
        return item.r;
    };
    // Use the whole client, not the current dirty blit rectangle, so partial repaints
    // cannot shift the group to a different position.
    overlay_layout::Rect controlsViewport{(int)client.left,(int)client.top,(int)client.right,(int)client.bottom};
    auto flushPanel=[&](const Base &base) {
        if(panelBoxes.empty()) return;
        int bx,by; toScreen(base.x,base.y,&bx,&by);
        // Perpendicular fitting is only for a visible base. Otherwise an offscreen
        // depot's reflected panel would remain pinned to the viewport while panning.
        if(bx<client.left || bx>=client.right || by<client.top || by>=client.bottom) {
            panelBoxes.clear(); panelFills.clear();
            return; // also omit its button hitboxes and label-layout obstacles
        }
        RECT bounds=panelBoxes.front().r;
        auto include=[&](RECT r) {bounds.left=std::min(bounds.left,r.left); bounds.top=std::min(bounds.top,r.top);
            bounds.right=std::max(bounds.right,r.right); bounds.bottom=std::max(bounds.bottom,r.bottom);};
        for(auto &item:panelBoxes) include(item.r);
        for(auto &item:panelFills) include(item.r);
        if(base.active) bounds.right=std::max(bounds.right,bounds.left+336);
        InflateRect(&bounds,7,7);
        double mx=0,my=0; int count=0;
        Funcs f(*g->st);
        auto mineral=[&](const unit_t *m) {if(m->sprite) {mx+=m->sprite->position.x; my+=m->sprite->position.y; ++count;}};
        if(base.active) for(auto *m:base.fields) mineral(m);
        else for(auto *m:ptr(g->st->visible_units)) if(f.unit_is_mineral_field(m) &&
            f.xy_length(m->position-xy(base.x,base.y))<=384) mineral(m);
        int offsetX=0,offsetY=0;
        if(count) {
            int cx,cy; toScreen(mx/count,my/count,&cx,&cy);
            auto r=overlay_layout::oppositePanel({bounds.left,bounds.top,bounds.right,bounds.bottom},bx,by,cx,cy,controlsViewport);
            offsetX=r.left-bounds.left; offsetY=r.top-bounds.top;
        }
        // Keep patch labels out of the aligned control group, but leave terrain
        // visible through its row gaps instead of painting a solid backdrop.
        placed.push_back({bounds.left+offsetX,bounds.top+offsetY,bounds.right+offsetX,bounds.bottom+offsetY});
        for(auto item:panelFills) {
            OffsetRect(&item.r,offsetX,offsetY);
            placed.push_back({item.r.left,item.r.top,item.r.right,item.r.bottom});
            HBRUSH brush=CreateSolidBrush(item.color);
            FillRect(dc,&item.r,brush);
            DeleteObject(brush);
        }
        for(auto item:panelBoxes) {OffsetRect(&item.r,offsetX,offsetY); drawBox(item);}
        panelBoxes.clear(); panelFills.clear();
    };
    static const char *raceName[3] = {"Zerg", "Terran", "Protoss"};
    static const char raceLetter[3] = {'Z', 'T', 'P'};
    for (size_t i = 0; i < g->bases.size(); i++) {
        Base &b = g->bases[i];
        int sx, sy; toScreen(b.x, b.y, &sx, &sy);
        int y = sy + 34;
        char t[96];
        if (!b.active) {
            snprintf(t, sizeof t, "Test mining (%s)", raceName[b.race]);
            RECT r = box(sx - 60, y, t, RGB(20, 70, 20), RGB(255, 255, 255), (int)i, 0);
            if (b.start) {
                int x = r.right + 3;
                for (int race : {1, 2, 0}) {
                    char l[2] = {raceLetter[race], 0};
                    RECT rr = box(x, y, l, race == b.race ? RGB(90, 90, 20) : RGB(40, 40, 40), RGB(255, 255, 255), (int)i, 10 + race);
                    x = rr.right + 2;
                }
            }
            RECT boost = box(sx - 60, r.bottom + 3, b.boostEnabled ? "[x] Enable position boosting" : "[ ] Enable position boosting",
                b.boostEnabled ? RGB(35, 70, 65) : RGB(35, 35, 35), RGB(255, 255, 255), (int)i, 7);
            RECT cargo = box(sx - 60, boost.bottom + 3, b.cargoEnabled ? "[x] Enable return cargo boost" : "[ ] Enable return cargo boost",
                b.cargoEnabled ? RGB(35, 50, 85) : RGB(35, 35, 35), RGB(255, 255, 255), (int)i, 8);
            box(sx - 60, cargo.bottom + 3, b.cycleEnabled ? "[x] Enable cycle boost" : "[ ] Enable cycle boost",
                b.cycleEnabled ? RGB(65, 40, 85) : RGB(35, 35, 35), RGB(255, 255, 255), (int)i, 9);
            flushPanel(b);
            continue;
        }
        double inc = rate(b.deliveries, b), gas = rate(b.gasDeliveries, b);
        if (inc >= 0) {
            if (b.refineries.empty()) snprintf(t, sizeof t, "%s: %.0f minerals/min", raceName[b.race], inc);
            else snprintf(t, sizeof t, "%s: %.0f minerals/min, %.0f gas/min", raceName[b.race], inc, gas);
        } else snprintf(t, sizeof t, "%s: measuring...", raceName[b.race]);
        if (b.boostPending) {
            if (g->boost && (int)i == g->boostBase) {
                snprintf(t, sizeof t, "Positions: %.1f/%.0fs, %zu checks%s", g->boost->sliceElapsed.load(), g->boost->sliceLimit.load(),
                         g->boost->executor.completed.load(), g->boost->validating.load()?" - validating":"");
            } else snprintf(t, sizeof t, "Position search queued...");
        } else if (b.cargoPending) {
            if (g->cargo && (int)i == g->cargoBase) {
                snprintf(t, sizeof t, "Cargo: %.1f/%.0fs, %zu checks%s", g->cargo->sliceElapsed.load(), g->cargo->sliceLimit.load(),
                         g->cargo->executor.completed.load(), g->cargo->validating.load()?" - validating":"");
            } else snprintf(t, sizeof t, "Return cargo search queued...");
        }
        if (!b.boostPending && !b.cargoPending && b.cyclePending) {
            if (g->cycle && (int)i == g->cycleBase)
                snprintf(t, sizeof t, "Cycle: %.1f/%.0fs, %zu checks%s", g->cycle->sliceElapsed.load(), g->cycle->sliceLimit.load(),
                    g->cycle->executor.completed.load(), g->cycle->validating.load()?" - validating":"");
            else snprintf(t, sizeof t, "Cycle search queued...");
        }
        bool pending=b.boostPending || b.cargoPending || b.cyclePending;
        RECT r = box(sx - 70+(pending?40:0), y, t, RGB(16, 16, 16), RGB(255, 220, 60), -1, -1);
        if (b.boostPending || b.cargoPending || b.cyclePending) {
            // Use wall time: simulation frames pause during searches. Draw from the first
            // pending repaint, including preparation/validation before counters advance.
            RECT track{r.left - 40, r.top, r.left - 4, r.bottom};
            panelFills.push_back({track,RGB(16,16,16)});
            int phase = (GetTickCount() / 40) % 48;
            int offset = phase <= 24 ? phase : 48 - phase;
            int cy = (r.top + r.bottom) / 2;
            RECT segment{track.left + 2 + offset, cy - 3, track.left + 10 + offset, cy + 3};
            panelFills.push_back({segment,b.boostPending ? RGB(90,220,200) : b.cargoPending ? RGB(100,180,255) : RGB(210,150,255)});
        }
        int x = sx - 70, y2 = r.bottom + 6;
        const COLORREF surface=RGB(18,22,28), muted=RGB(175,188,202), white=RGB(240,244,248);
        RECT m = box(x,y2," - ",RGB(60,30,30),white,(int)i,2,24);
        snprintf(t,sizeof t,"%d minerals",b.workers);
        RECT wr=box(m.right+2,y2,t,surface,white,-1,-1,78);
        RECT plus=box(wr.right+2,y2," + ",RGB(30,60,40),white,(int)i,3,24);
        if(!b.refineries.empty()) {
            RECT gm=box(plus.right+12,y2," - ",RGB(60,30,30),white,(int)i,4,24);
            snprintf(t,sizeof t,"%d gas",b.gasWorkers);
            RECT gw=box(gm.right+2,y2,t,surface,RGB(140,255,140),-1,-1,60);
            box(gw.right+2,y2," + ",RGB(30,60,40),white,(int)i,5,24);
        }
        int y3=wr.bottom+4;
        RECT stop=box(x,y3,"Stop",RGB(90,30,30),white,(int)i,1,48);
        RECT display=box(stop.right+6,y3,g_showCycles?"Frames: latest cycle":"Minerals / minute",RGB(35,45,60),white,(int)i,6);
        auto searchRow=[&](int kind,int top,const auto &session) {
            box(x+8,top,"Budget",surface,muted,-1,-1);
            RECT minus=box(x+62,top," - ",RGB(40,43,48),white,(int)i,31+kind*3,24);
            snprintf(t,sizeof t,"%ds",b.searchSeconds[kind]);
            RECT time=box(minus.right+2,top,t,surface,white,-1,-1,44);
            RECT more=box(time.right+2,top," + ",RGB(40,43,48),white,(int)i,32+kind*3,24);
            const char *label=session?(session->exhausted.load()?"Search exhausted":"Continue search"):"Search";
            int action=searchPending() || (session && session->exhausted.load())?-1:30+kind*3;
            RECT run=box(more.right+10,top,label,RGB(35,45,65),action<0?RGB(140,140,140):white,action<0?-1:(int)i,action,120);
            return run.bottom+8;
        };
        int cargoCount=0,cycleCount=0;
        for(const auto &rule:b.cargoRules) if(rule.enabled) ++cargoCount;
        for(const auto &rule:b.cycleRules) if(rule.enabled) ++cycleCount;
        int nextY=display.bottom+9;
        auto technique=[&](int kind,const char *name,bool enabled,bool pending,int count,const auto &session,COLORREF color,COLORREF bg) {
            snprintf(t,sizeof t,"[%c] %s",enabled?'x':' ',name);
            RECT toggle=box(x,nextY,t,enabled?bg:RGB(38,40,44),white,(int)i,7+kind,148);
            if(pending) snprintf(t,sizeof t,"Searching...");
            else if(!enabled) snprintf(t,sizeof t,"Off");
            else if(!session) snprintf(t,sizeof t,"Not searched");
            else if(kind==0) snprintf(t,sizeof t,"%d/%zu faster starts",count,b.fields.size());
            else snprintf(t,sizeof t,"%d verified boosts",count);
            box(x+158,nextY,t,surface,color,-1,-1);
            nextY=searchRow(kind,toggle.bottom+3,session);
        };
        technique(0,"Position boost",b.boostEnabled,b.boostPending,b.fasterFields,b.positionSession,RGB(140,230,220),RGB(35,70,65));
        technique(1,"Return cargo",b.cargoEnabled,b.cargoPending,cargoCount,b.cargoSession,RGB(140,200,255),RGB(35,50,85));
        technique(2,"Cycle boost",b.cycleEnabled,b.cyclePending,cycleCount,b.cycleSession,RGB(210,160,255),RGB(65,40,85));
        RECT guides=box(x,nextY,"Guides (all bases):",surface,muted,-1,-1);
        for(int kind=0;kind<3;++kind) {
            static const char *names[]={"Position","Cargo","Cycle"};
            bool visible=g->guideVisible(kind);
            snprintf(t,sizeof t,"[%c] %s",visible?'x':' ',names[kind]);
            guides=box(guides.right+4,nextY,t,visible?RGB(40,58,64):RGB(38,40,44),visible?white:muted,(int)i,41+kind,kind==0?76:kind==1?60:56);
        }
        guides=box(guides.right+4,nextY,"All",RGB(35,45,60),white,(int)i,40,28);
        nextY=guides.bottom+6;
        if(!searchPending()) {
            RECT speed=box(x,nextY,"Speed (all bases):",surface,muted,-1,-1);
            for(int n=0;n<3;++n) {
                int value=1<<n; snprintf(t,sizeof t,"%dx",value);
                speed=box(speed.right+4,nextY,t,g->playback.speed==value?RGB(75,75,25):RGB(35,40,48),white,(int)i,20+n,28);
            }
        }
        flushPanel(b);
        for (size_t k = 0; k < b.fields.size(); k++) {
            unit_t *m2 = b.fields[k];
            if (!m2->sprite) continue;
            int fx, fy; toScreen(m2->sprite->position.x, m2->sprite->position.y, &fx, &fy);
            double v = g_showCycles ? latestCycle(b.fieldCycles[k]) : rate(b.fieldDeliveries[k], b);
            if (v < 0) snprintf(t, sizeof t, "...");
            else if (g_showCycles) snprintf(t, sizeof t, "%.0f f", v);
            else snprintf(t, sizeof t, "%.0f/min", v);
            labels.push_back({fx, fy, t, v > 0 ? RGB(120, 220, 255) : RGB(150, 150, 150)});
        }
        for (size_t k = 0; k < b.refineries.size(); k++) {  // gas is always per minute
            unit_t *rf = b.refineries[k];
            if (!rf->sprite) continue;
            int fx, fy; toScreen(rf->sprite->position.x, rf->sprite->position.y, &fx, &fy);
            double v = rate(b.refineryDeliveries[k], b);
            if (v < 0) snprintf(t, sizeof t, "..."); else snprintf(t, sizeof t, "%.0f gas/min", v);
            labels.push_back({fx, fy, t, v > 0 ? RGB(140, 255, 140) : RGB(150, 150, 150)});
        }
    }
    collecting=false;
    // Layout all field/gas labels against every base's controls, not just earlier bases.
    // Reserve the activity bar too; labels must not hide search feedback.
    auto controlRects = placed;
    std::vector<Label> visible;
    int mapLeft, mapTop, mapRight, mapBottom;
    toScreen(0, 0, &mapLeft, &mapTop);
    toScreen(g->game->map_width, g->game->map_height, &mapRight, &mapBottom);
    overlay_layout::Rect viewport{std::max({xd, (int)client.left, mapLeft}), std::max({yd, (int)client.top, mapTop}),
        std::min({xd + wd, (int)client.right, mapRight}), std::min({yd + hd, (int)client.bottom, mapBottom})};
    for (auto &label : labels) {
        // Offscreen patches should not push their labels onto visible patches at the edge.
        if (label.x < viewport.left || label.x >= viewport.right || label.y < viewport.top || label.y >= viewport.bottom) continue;
        SIZE sz; GetTextExtentPoint32A(dc, label.text.c_str(), (int)label.text.size(), &sz);
        int x = label.x - sz.cx / 2 - 4, y = label.y - sz.cy / 2 - 2;
        auto r = overlay_layout::place({x, y, x + sz.cx + 8, y + sz.cy + 4}, viewport, placed);
        if (!r) continue;
        label.rect = *r; placed.push_back(*r); visible.push_back(std::move(label));
    }
    // Guide lines preserve the patch association when a label has to move. Clip them out of
    // controls, then draw all text last, so neither lines nor later bases can overwrite labels.
    int guides = SaveDC(dc);
    for (const auto &r : controlRects) ExcludeClipRect(dc, r.left, r.top, r.right, r.bottom);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(65, 120, 135));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    for (const auto &label : visible) {
        const auto &r = label.rect;
        int x = std::clamp(label.x, r.left, r.right - 1), y = std::clamp(label.y, r.top, r.bottom - 1);
        if (std::abs(x - label.x) + std::abs(y - label.y) <= 3) continue;
        MoveToEx(dc, label.x, label.y, nullptr); LineTo(dc, x, y);
    }
    SelectObject(dc, oldPen); DeleteObject(pen); RestoreDC(dc, guides);
    for (const auto &label : visible)
        box(label.rect.left, label.rect.top, label.text.c_str(), RGB(16, 16, 16), label.color, -1, -1);
    RestoreDC(dc, saved);
}

// Changes upstream invalidate dependent searches: their evaluated lineups are no longer the
// same phase space. Distant bases keep their independent input snapshots and cursors.
void requestSearch(Base &b,int kind) {
    if(!b.active || b.fields.empty() || searchPending()) return;
    if(kind==0) {
        if(!b.positionSession) {
            int workers=b.workers,gas=b.gasWorkers;
            stopBase(b); startBase(b,workers,gas);
        }
        b.boostEnabled=true; b.boostPending=true;
    } else if(kind==1) {
        b.cargoEnabled=true; b.cargoPending=true;
    } else {b.cycleEnabled=true; b.cyclePending=true;}
    g->playback.reset(); g->lastTick=GetTickCount();
}

// Mouse on the map window: returns true when the click was one of our buttons.
bool onClick(HWND wnd, int x, int y) {
    Locked l;
    if (getenv("SCMHD_DUMP")) {
        for (const Button &bt : g_buttons) logf("live button base %d action %d at %ld,%ld-%ld,%ld", bt.base, bt.action, bt.r.left, bt.r.top, bt.r.right, bt.r.bottom);
        logf("live click at %d,%d", x, y);
    }
    if (!g_enabled || !g || wnd != g_buttonsWnd) return false;
    POINT pt{x, y};
    for (const Button &bt : g_buttons) {
        if (!PtInRect(&bt.r, pt)) continue;
        Base &b = g->bases[bt.base];
        try {
            if (bt.action == 0) startBase(b, 0, -1);
            else if (bt.action == 1) stopBase(b);
            else if (bt.action == 7 || bt.action == 8 || bt.action == 9) {
                bool &option = bt.action == 7 ? b.boostEnabled : bt.action == 8 ? b.cargoEnabled : b.cycleEnabled;
                bool on = !option;
                if (b.active) {
                    int workers = b.workers, gas = b.gasWorkers;
                    stopBase(b);
                    option = on;
                    startBase(b, workers, gas);
                } else option = on;
            }
            else if(bt.action>=40 && bt.action<=43) {
                if(bt.action==40) g->guideMask=7;
                else g->guideMask^=1u<<(bt.action-41);
                logf("live: guides position=%d cargo=%d cycle=%d, frame %d",g->guideVisible(0),g->guideVisible(1),g->guideVisible(2),g->st->current_frame);
            }
            else if (bt.action >= 30 && bt.action < 39) {
                int kind=(bt.action-30)/3, action=(bt.action-30)%3;
                if(action==0) requestSearch(b,kind);
                else {int step=(GetKeyState(VK_SHIFT)&0x8000)?1:5;
                    b.searchSeconds[kind]=std::clamp(b.searchSeconds[kind]+(action==1?-step:step),1,3600);}
            }
            else if (bt.action >= 2 && bt.action <= 5 && (b.boostEnabled || b.cargoEnabled || b.cycleEnabled)) {
                int workers = b.workers, gas = b.gasWorkers;
                if (bt.action == 2 && workers > 1) --workers;
                if (bt.action == 3) ++workers;
                if (bt.action == 4 && gas > 0) --gas;
                if (bt.action == 5) ++gas;
                stopBase(b); startBase(b, workers, gas);
            }
            else if (bt.action == 2 && b.workers > 1) removeWorker(b, false);
            else if (bt.action == 3) addWorker(b, false);
            else if (bt.action == 4 && b.gasWorkers > 0) removeWorker(b, true);
            else if (bt.action == 5) addWorker(b, true);
            else if (bt.action == 6) g_showCycles = !g_showCycles;
            else if (bt.action >= 20 && bt.action <= 22) {
                if (!searchPending()) {
                    g->playback.speed = 1 << (bt.action - 20);
                    g->playback.reset(); g->lastTick = GetTickCount();
                    logf("live: playback %dx (all bases), frame %d", g->playback.speed, g->st->current_frame);
                }
            }
            else if (bt.action >= 10 && bt.action <= 12) b.race = bt.action - 10;
        } catch (std::exception &e) {
            logf("live: engine error: %s", e.what());
        }
        return true;
    }
    return false;
}

bool overButton(HWND wnd, int x, int y) {
    Locked l;
    if (!g_enabled || wnd != g_buttonsWnd) return false;
    POINT pt{x, y};
    for (const Button &bt : g_buttons) if (PtInRect(&bt.r, pt)) return true;
    return false;
}

UINT_PTR timerId() { return kTimerId; }

} // namespace live

// Collision overlays use immutable UI-thread snapshots of map UNIT records. ScmDraft's
// serializer must run outside the draw lock; render threads never call editor map APIs.
namespace hitboxes {
using namespace bwgame;
constexpr UINT_PTR kTimer = 0x48424f58;
bool buildings = false, units = false;
struct Entry { int x, y, type; };
std::vector<Entry> entries;
void *map = nullptr;
std::unique_ptr<unit_types_t> types;

bool enabled(bool building) { Locked lock; return building ? buildings : units; }
UINT_PTR timerId() { return kTimer; }
void update() {  // UI thread, no draw lock across SaveToMemory
    { Locked lock; if (!buildings && !units) return; }
    void *current = scmhdActiveMap();
    std::vector<uint8_t> chk; std::string error;
    std::vector<Entry> next;
    if (current && scmhdExportActiveMap(chk, error)) {
        live::forChunks(chk, [&](const std::string &tag, const uint8_t *p, uint32_t size) {
            if (tag != "UNIT") return;
            for (uint32_t i = 0; i + 36 <= size; i += 36) {
                uint16_t x, y, type;
                memcpy(&x, p + i + 4, 2); memcpy(&y, p + i + 6, 2); memcpy(&type, p + i + 8, 2);
                if (type < 228 && type != 214) next.push_back({x, y, type}); // start locations are editor markers
            }
        });
    }
    Locked lock;
    entries = std::move(next); map = current;
    if (!types) {
        std::vector<uint8_t> data;
        if (scmhdCascRead("arr\\units.dat", data)) {
            try { types = std::make_unique<unit_types_t>(data_loading::load_units_dat(data)); }
            catch (const std::exception &e) { logf("hitboxes: units.dat failed: %s", e.what()); }
        }
    }
}
void toggle(HWND main, bool building) {
    DWORD flags;
    {
        Locked lock;
        (building ? buildings : units) = !(building ? buildings : units);
        flags = (buildings ? 1 : 0) | (units ? 2 : 0);
        logf("hitboxes: buildings=%d units=%d", buildings, units);
    }
    if (flags) { SetTimer(main, kTimer, 250, nullptr); update(); }
    else { KillTimer(main, kTimer); Locked lock; entries.clear(); map = nullptr; }
    RedrawWindow(main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void init(HWND main) {
    KillTimer(main, kTimer);
    Locked lock;
    buildings = units = false;
    entries.clear(); map = nullptr;
}
void draw(HDC dc, void *viewMap, int xd, int yd, int wd, int hd, double xs, double ys,
          double ws, double hs, int ox, int oy) {
    if ((!buildings && !units) || !types || ws <= 0 || hs <= 0) return;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, xd, yd, xd + wd, yd + hd);
    HPEN buildingPen = CreatePen(PS_SOLID, 1, RGB(255, 190, 60));
    HPEN unitPen = CreatePen(PS_SOLID, 1, RGB(70, 240, 240));
    SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    auto outline = [&](int x, int y, const unit_type_t &type) {
        bool building = (type.flags & unit_type_t::flag_building) != 0;
        if (building ? !buildings : !units) return;
        // BW's right/bottom extents are inclusive. Convert to half-open pixel edges.
        auto d = type.dimensions;
        int l = xd + (int)std::floor((x - d.from.x - ox - xs) * wd / ws);
        int t = yd + (int)std::floor((y - d.from.y - oy - ys) * hd / hs);
        int r = xd + (int)std::ceil((x + d.to.x + 1 - ox - xs) * wd / ws);
        int b = yd + (int)std::ceil((y + d.to.y + 1 - oy - ys) * hd / hs);
        if (r <= xd || b <= yd || l >= xd + wd || t >= yd + hd) return;
        SelectObject(dc, building ? buildingPen : unitPen);
        Rectangle(dc, l, t, std::max(l + 1, r), std::max(t + 1, b));
    };
    bool liveMap = live::g && live::g_enabled && live::g->mapIdentity == viewMap;
    if (viewMap == map) for (const auto &e : entries) {
        bool replaced = false;
        // A refinery replaces its neutral geyser in the simulation and has different extents.
        if (liveMap && e.type == (int)UnitTypes::Resource_Vespene_Geyser)
            for (const auto &base : live::g->bases) if (base.active)
                for (const auto *r : base.refineries)
                    if (r->sprite && r->sprite->position == xy(e.x, e.y)) replaced = true;
        if (!replaced) outline(e.x, e.y, types->vec[e.type]);
    }
    // These are actual OpenBW units, including moving workers, depots and refineries.
    if (liveMap) {
        for (auto *u : ptr(live::g->st->visible_units)) {
            if (!u->sprite || u->owner >= 8) continue;
            bool active = false;
            for (const auto &base : live::g->bases) if (base.active && base.slot == u->owner) active = true;
            if (active) outline(u->sprite->position.x, u->sprite->position.y, *u->unit_type);
        }
    }
    RestoreDC(dc, saved);
    DeleteObject(buildingPen); DeleteObject(unitPen);
}
} // namespace hitboxes
