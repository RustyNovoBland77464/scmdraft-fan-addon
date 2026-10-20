// minesim: simulate StarCraft: Brood War worker mining at the bases of a map, with the real engine
// (OpenBW, an exact reimplementation of BW 1.16.1) and game data read from a StarCraft: Remastered
// CASC install.
//
// For every base (start locations, plus --base x,y) and race it places that race's resource depot
// and workers exactly like a melee game start (state_functions::create_starting_units), adds workers
// up to the requested count, splits them over the mineral fields like a player would (round-robin,
// closest fields first), lets the game run and measures delivered minerals. Worker movement,
// collision, pathfinding (BW regions) and mining (75 frame mining, field bouncing, return to the
// depot's bounding box) are all the engine's own, so CC / Nexus / Hatchery differences come out of
// their real unit dimensions.
//
// Bases: every start location, every resource depot placed in the map (expansions), and --base X,Y.
// Usage: minesim --casc <StarCraft dir> --map <map.scx|scenario.chk> [--base X,Y]... [--no-start] [--no-map-depots]
//                [--workers 9,18,27] [--warmup 720] [--frames 4286] [--gas] [--races tpz]
//   --workers counts mineral workers; --gas adds a completed refinery on the closest geyser with 3 more.
// Output: JSON on stdout.
#include "bwgame.h"
#include "../hd/src/simulation_map.h"
#include "CascLib.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace bwgame;

namespace {

[[noreturn]] void die(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fputc('\n', stderr);
    exit(1);
}

// OpenBW data loader backed by a CASC storage ("arr/units.dat" -> "arr\units.dat").
struct casc_loader {
    HANDLE storage = nullptr;
    explicit casc_loader(const std::string &dir) {
        if (!CascOpenStorage(dir.c_str(), 0, &storage)) die("cannot open CASC storage at %s (error %u)", dir.c_str(), GetCascError());
    }
    bool read(std::string name, std::vector<uint8_t> &out) const {
        std::replace(name.begin(), name.end(), '/', '\\');
        HANDLE f;
        if (!CascOpenFile(storage, name.c_str(), 0, CASC_OPEN_BY_NAME, &f)) return false;
        ULONGLONG size = 0; CascGetFileSize64(f, &size);
        out.resize((size_t)size);
        DWORD rd = 0; size_t got = 0;
        while (got < out.size() && CascReadFile(f, out.data() + got, (DWORD)(out.size() - got), &rd) && rd) got += rd;
        CascCloseFile(f);
        out.resize(got);
        return got == size;
    }
    void operator()(a_vector<uint8_t> &dst, a_string filename) const {
        std::vector<uint8_t> tmp;
        if (!read(std::string(filename.c_str()), tmp)) die("CASC: missing %s", filename.c_str());
        dst.assign(tmp.begin(), tmp.end());
    }
};

std::vector<uint8_t> load_chk_raw(const std::string &path);
std::vector<uint8_t> read_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) die("cannot open %s", path.c_str());
    std::vector<uint8_t> d;
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return d;
}

// OpenBW requires a classic "STR " chunk; Remastered-era maps may only have "STRx". Strings do not
// matter for mining, so add an empty table when it is missing.
std::vector<uint8_t> fix_chk(std::vector<uint8_t> d) {
    bool has_str = false;
    for (size_t o = 0; o + 8 <= d.size();) {
        uint32_t len; memcpy(&len, &d[o + 4], 4);
        if (!memcmp(&d[o], "STR ", 4)) has_str = true;
        // Remastered maps (VER 206) read like Brood War 1.04+ maps (205) for terrain, units and sprites
        if (!memcmp(&d[o], "VER ", 4) && len >= 2 && d[o + 8] == 206 && d[o + 9] == 0) d[o + 8] = 205;
        if (len > d.size() - o - 8) break;
        o += 8 + len;
    }
    if (!has_str) {
        const uint8_t str[] = {'S', 'T', 'R', ' ', 2, 0, 0, 0, 0, 0};
        d.insert(d.end(), str, str + sizeof str);
    }
    return d;
}

std::vector<uint8_t> load_chk(const std::string &path) { return fix_chk(load_chk_raw(path)); }

std::vector<uint8_t> load_chk_raw(const std::string &path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".chk") return read_file(path);
    a_vector<uint8_t> d;
    data_loading::mpq_file<> mpq(path.c_str());
    mpq(d, "staredit\\scenario.chk");
    return std::vector<uint8_t>(d.begin(), d.end());
}

struct base_t { xy pos; bool start; };

struct result_t {
    int race, workers;
    double minerals_per_min = 0, gas_per_min = 0;
    std::map<int, std::pair<int, int>> patch_trips;  // patch index -> (deliveries, frames spent)
};

const char *race_name(int r) { return r == 0 ? "zerg" : r == 1 ? "terran" : "protoss"; }
constexpr double kFramesPerMinute = 60.0 * 1000.0 / 42.0;  // Fastest game speed: 42 ms per frame

struct simulator {
    global_state &gst;
    const std::vector<uint8_t> &chk;
    int warmup, frames;
    bool gas;

    // Resource depots placed in the map (any owner) mark expansions.
    std::vector<base_t> depot_bases() {
        std::vector<base_t> r;
        for (size_t o = 0; o + 8 <= chk.size();) {
            uint32_t len; memcpy(&len, &chk[o + 4], 4);
            if (len > chk.size() - o - 8) break;
            if (!memcmp(&chk[o], "UNIT", 4))
                for (size_t u = o + 8; u + 36 <= o + 8 + len; u += 36) {
                    uint16_t x, y, type; memcpy(&x, &chk[u + 4], 2); memcpy(&y, &chk[u + 6], 2); memcpy(&type, &chk[u + 8], 2);
                    if (type == 106 || type == 131 || type == 132 || type == 133 || type == 154) r.push_back({xy(x, y), false});
                }
            o += 8 + len;
        }
        return r;
    }

    std::vector<base_t> start_bases() {
        game_state game; state st; st.global = &gst; st.game = &game;
        game_load_functions load(st);
        std::vector<uint8_t> data = chk;
        simulation_map::load(load, data, [&]() {
            for (size_t i = 0; i < 8; i++) st.players[i].controller = player_t::controller_closed;
            load.setup_info.victory_condition = 2; load.setup_info.starting_units = 1;
        });
        std::vector<base_t> r;
        for (size_t i = 0; i < 8; i++) {
            xy p = game.start_locations[i];
            if (p != xy()) r.push_back({p, true});
        }
        return r;
    }

    // One run: base, race, worker count.
    result_t run(base_t base, int race, int workers, std::vector<xy> *patch_positions) {
        game_state game; state st; st.global = &gst; st.game = &game;
        game_load_functions load(st);
        std::vector<uint8_t> data = chk;
        const int owner = 0;
        simulation_map::load(load, data, [&]() {
            for (size_t i = 0; i < 12; i++)
                if (i < 8) st.players[i].controller = player_t::controller_closed;
            // neutral during load so no melee start is created at its start location; we place the base
            st.players[owner].controller = player_t::controller_neutral;
            st.players[owner].race = (race_t)race;
            load.setup_info.victory_condition = 2;  // no melee victory triggers
            load.setup_info.starting_units = 1;     // non-UMS: only neutral map units (resources, critters)
        });
        st.players[owner].controller = player_t::controller_occupied;
        state_functions f(st);
        auto &L = static_cast<game_load_functions &>(load);
        L.create_starting_units(owner, base.pos, (race_t)race);

        unit_t *depot = nullptr;
        std::vector<unit_t *> workers_v;
        for (unit_t *u : ptr(st.player_units[owner])) {
            if (f.ut_resource_depot(u)) depot = u;
            else if (f.ut_worker(u)) workers_v.push_back(u);
        }
        if (!depot) die("no depot created at %d,%d", base.pos.x, base.pos.y);
        const unit_type_t *worker_type = workers_v.empty() ? nullptr : workers_v[0]->unit_type;
        int wanted = workers + (gas ? 3 : 0);  // --workers counts mineral workers; gas gets 3 extra
        while ((int)workers_v.size() < wanted && worker_type) {
            unit_t *u = L.create_starting_unit(worker_type, base.pos, owner);
            if (!u) break;
            workers_v.push_back(u);
        }

        if (getenv("MINESIM_DEBUG")) {
            int vis = 0, hid = 0, minerals = 0;
            for (unit_t *u : ptr(st.visible_units)) { vis++; if (f.unit_is_mineral_field(u)) minerals++; }
            for (unit_t *u : ptr(st.hidden_units)) { hid++; (void)u; }
            fprintf(stderr, "debug: visible %d hidden %d minerals %d depot at %d,%d workers %zu\n", vis, hid, minerals,
                    depot->sprite->position.x, depot->sprite->position.y, workers_v.size());
        }
        // resources near the base (BW keeps them within ~12 tiles of a depot)
        std::vector<unit_t *> fields, geysers;
        for (unit_t *u : ptr(st.visible_units)) {
            if (f.xy_length(u->sprite->position - depot->sprite->position) > 32 * 12) continue;
            if (f.unit_is_mineral_field(u)) fields.push_back(u);
            else if (f.unit_is(u, UnitTypes::Resource_Vespene_Geyser)) geysers.push_back(u);
        }
        auto dist = [&](unit_t *u) { return f.xy_length(u->sprite->position - depot->sprite->position); };
        std::sort(fields.begin(), fields.end(), [&](unit_t *a, unit_t *b) { return dist(a) < dist(b); });
        std::sort(geysers.begin(), geysers.end(), [&](unit_t *a, unit_t *b) { return dist(a) < dist(b); });
        if (patch_positions) { patch_positions->clear(); for (unit_t *m : fields) patch_positions->push_back(m->sprite->position); }

        // gas: a completed refinery on the closest geyser with 3 workers
        std::vector<unit_t *> gas_workers;
        if (gas && !geysers.empty() && (int)workers_v.size() >= workers + 3) {
            UnitTypes ref = race == 0 ? UnitTypes::Zerg_Extractor : race == 1 ? UnitTypes::Terran_Refinery : UnitTypes::Protoss_Assimilator;
            // what state_functions::build_refinery does: the geyser unit itself becomes the refinery
            unit_t *g = geysers[0];
            L.add_completed_unit(g, -1, true);
            L.u_unset_status_flag(g, unit_t::status_flag_completed);
            L.set_unit_owner(g, owner, false);
            L.set_sprite_owner(g, owner);
            L.morph_unit(g, L.get_unit_type(ref));
            L.create_image(L.get_image_type(ImageTypes::IMAGEID_Vespene_Geyser2), g->sprite, {}, L.image_order_below);
            L.finish_building_unit(g);
            L.complete_unit(g);
            for (int i = 0; i < 3; i++) {
                unit_t *w = workers_v.back(); workers_v.pop_back();
                f.issue_order(w, false, f.get_order_type(Orders::Harvest1), {g->sprite->position, g});
                gas_workers.push_back(w);
            }
        }

        // split: round-robin over fields, closest first
        std::map<unit_t *, int> field_index;
        for (size_t i = 0; i < fields.size(); i++) field_index[fields[i]] = (int)i;
        for (size_t i = 0; i < workers_v.size() && !fields.empty(); i++) {
            unit_t *m = fields[i % fields.size()];
            f.issue_order(workers_v[i], false, f.get_order_type(Orders::Harvest1), {m->sprite->position, m});
        }

        result_t r; r.race = race; r.workers = workers;
        std::map<unit_t *, int> last_delivery;
        int min0 = 0, gas0 = 0;
        for (int frame = 0; frame < warmup + frames; frame++) {
            if (frame == warmup) { min0 = st.total_minerals_gathered[owner]; gas0 = st.total_gas_gathered[owner]; }
            std::vector<std::pair<unit_t *, int>> carrying;
            for (unit_t *w : workers_v) carrying.push_back({w, w->carrying_flags});
            f.next_frame();
            for (auto &c : carrying) {
                unit_t *w = c.first;
                if ((c.second & 2) && !(w->carrying_flags & 2)) {  // delivered minerals this frame
                    unit_t *patch = w->worker.target_resource_unit;
                    auto it = last_delivery.find(w);
                    if (frame >= warmup && it != last_delivery.end() && patch && field_index.count(patch)) {
                        auto &pt = r.patch_trips[field_index[patch]];
                        pt.first++; pt.second += frame - it->second;
                    }
                    last_delivery[w] = frame;
                }
            }
        }
        r.minerals_per_min = (st.total_minerals_gathered[owner] - min0) * kFramesPerMinute / frames;
        r.gas_per_min = (st.total_gas_gathered[owner] - gas0) * kFramesPerMinute / frames;
        return r;
    }
};

} // namespace

int main(int argc, char **argv) {
    std::string casc, map;
    std::vector<base_t> bases;
    std::vector<int> worker_counts = {9, 18, 27};
    std::string races = "tpz";
    int warmup = 720, frames = 4286;  // 30 s warmup, 3 min measured (Fastest)
    bool starts = true, gas = false, map_depots = true, report = false;
    std::string json_path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) die("missing value for %s", a.c_str()); return argv[++i]; };
        if (a == "--casc") casc = next();
        else if (a == "--map") map = next();
        else if (a == "--base") { int x, y; if (sscanf(next().c_str(), "%d,%d", &x, &y) != 2) die("--base X,Y"); bases.push_back({xy(x, y), false}); }
        else if (a == "--no-start") starts = false;
        else if (a == "--no-map-depots") map_depots = false;
        else if (a == "--workers") { worker_counts.clear(); std::string s = next(); for (char *t = strtok(&s[0], ","); t; t = strtok(nullptr, ",")) worker_counts.push_back(atoi(t)); }
        else if (a == "--warmup") warmup = atoi(next().c_str());
        else if (a == "--frames") frames = atoi(next().c_str());
        else if (a == "--gas") gas = true;
        else if (a == "--report") report = true;
        else if (a == "--json") json_path = next();
        else if (a == "--races") races = next();
        else die("unknown argument %s", a.c_str());
    }
    if (casc.empty() || map.empty()) die("usage: minesim --casc <StarCraft dir> --map <map.scx|.chk> [--base X,Y]... [--workers 4,9,18] [--gas]");

    casc_loader loader(casc);
    global_state gst;
    global_init(gst, loader);
    std::vector<uint8_t> chk = load_chk(map);

    simulator sim{gst, chk, warmup, frames, gas};
    if (starts) { auto s = sim.start_bases(); bases.insert(bases.begin(), s.begin(), s.end()); }
    if (map_depots) {
        for (base_t d : sim.depot_bases()) {
            bool dup = false;  // a depot sitting on a start location is that base
            for (const base_t &b : bases) dup |= std::abs(b.pos.x - d.pos.x) < 96 && std::abs(b.pos.y - d.pos.y) < 96;
            if (!dup) bases.push_back(d);
        }
    }

    if (bases.empty()) die("no bases: the map has no start locations or resource depots");
    struct base_out { base_t base; std::vector<xy> patches; std::vector<result_t> results; };
    std::vector<base_out> out;
    for (const base_t &bs : bases) {
        base_out bo{bs, {}, {}};
        for (char rc : races) {
            int race = rc == 'z' ? 0 : rc == 't' ? 1 : 2;
            for (int w : worker_counts) bo.results.push_back(sim.run(bs, race, w, &bo.patches));
        }
        out.push_back(std::move(bo));
    }
    auto trip = [](const result_t &r, size_t p) {
        auto it = r.patch_trips.find((int)p);
        return it != r.patch_trips.end() && it->second.first ? (double)it->second.second / it->second.first : 0.0;
    };

    std::string json_path_copy = json_path;
    FILE *jf = nullptr;
    if (!json_path.empty()) { jf = fopen(json_path.c_str(), "w"); if (!jf) die("cannot write %s", json_path.c_str()); }
    if (!report || jf) {
        FILE *o = jf ? jf : stdout;
        auto printf = [o](const char *fmt, auto... a) { fprintf(o, fmt, a...); };
        printf("{\"frames_per_minute\": %.2f, \"measured_frames\": %d, \"bases\": [\n", kFramesPerMinute, frames);
        for (size_t b = 0; b < out.size(); b++) {
            printf("  {\"x\": %d, \"y\": %d, \"start\": %s, \"results\": [\n", out[b].base.pos.x, out[b].base.pos.y, out[b].base.start ? "true" : "false");
            for (size_t i = 0; i < out[b].results.size(); i++) {
                const result_t &r = out[b].results[i];
                printf("%s    {\"race\": \"%s\", \"workers\": %d, \"minerals_per_min\": %.1f, \"gas_per_min\": %.1f, \"patch_trip_frames\": [",
                       i ? ",\n" : "", race_name(r.race), r.workers, r.minerals_per_min, r.gas_per_min);
                for (size_t p = 0; p < out[b].patches.size(); p++) printf("%s%.1f", p ? ", " : "", trip(r, p));
                printf("]}");
            }
            printf("\n  ], \"patches\": [");
            for (size_t p = 0; p < out[b].patches.size(); p++) printf("%s[%d, %d]", p ? ", " : "", out[b].patches[p].x, out[b].patches[p].y);
            printf("]}%s\n", b + 1 < out.size() ? "," : "");
        }
        printf("]}\n");
        if (jf) fclose(jf);
        if (!report) return 0;
    }

    // human-readable report
    std::vector<int> race_ids;
    for (char rc : races) race_ids.push_back(rc == 'z' ? 0 : rc == 't' ? 1 : 2);
    auto find = [&](const base_out &bo, int race, int w) -> const result_t * {
        for (const result_t &r : bo.results) if (r.race == race && r.workers == w) return &r;
        return nullptr;
    };
    auto header = [&]() {
        printf("%-22s", "");
        for (int r : race_ids) printf("%10s", r == 0 ? "Zerg" : r == 1 ? "Terran" : "Protoss");
        printf("\r\n");
    };
    printf("Mining simulation - OpenBW engine (Brood War 1.16.1 rules), StarCraft: Remastered data\r\n");
    printf("Minerals per minute at Fastest, measured over %.1f game minutes after a %.0f s warm-up.\r\n",
           frames / kFramesPerMinute, warmup / (kFramesPerMinute / 60));
    printf("Workers are split over the fields like a player would (closest fields first)%s.\r\n",
           gas ? "; gas = 3 extra workers on a refinery at the closest geyser" : "");
    for (size_t b = 0; b < out.size(); b++) {
        const base_out &bo = out[b];
        printf("\r\nBase %zu - %s at (%d, %d), %zu mineral fields\r\n", b + 1, bo.base.start ? "start location" : "resource depot",
               bo.base.pos.x, bo.base.pos.y, bo.patches.size());
        header();
        for (int w : worker_counts) {
            char label[32]; snprintf(label, sizeof label, "  %d workers", w);
            printf("%-22s", label);
            for (int r : race_ids) { const result_t *x = find(bo, r, w); printf("%10.1f", x ? x->minerals_per_min : 0); }
            printf("\r\n");
        }
        if (gas) {
            printf("%-22s", "  gas (3 workers)");
            for (int r : race_ids) { const result_t *x = find(bo, r, worker_counts[0]); printf("%10.1f", x ? x->gas_per_min : 0); }
            printf("\r\n");
        }
        int w1 = worker_counts[0];
        printf("  Round trip per field, %d workers (frames; lower is better):\r\n", w1);
        for (size_t p = 0; p < bo.patches.size(); p++) {
            char label[48]; snprintf(label, sizeof label, "    field %zu (%d,%d)", p + 1, bo.patches[p].x, bo.patches[p].y);
            printf("%-22s", label);
            for (int r : race_ids) { const result_t *x = find(bo, r, w1); double t = x ? trip(*x, p) : 0; if (t > 0) printf("%10.1f", t); else printf("%10s", "-"); }
            printf("\r\n");
        }
    }
    if (out.size() > 1) {
        printf("\r\nFairness across bases (minerals/min, %d workers):\r\n", worker_counts[0]);
        header();
        for (const char *what : {"best", "worst", "spread %"}) {
            printf("  %-20s", what);
            for (int r : race_ids) {
                double lo = 1e9, hi = 0;
                for (const base_out &bo : out) { const result_t *x = find(bo, r, worker_counts[0]); if (x) { lo = std::min(lo, x->minerals_per_min); hi = std::max(hi, x->minerals_per_min); } }
                double v = !strcmp(what, "best") ? hi : !strcmp(what, "worst") ? lo : (hi > 0 ? 100.0 * (hi - lo) / hi : 0);
                printf("%10.1f", v);
            }
            printf("\r\n");
        }
    }
    return 0;
}
