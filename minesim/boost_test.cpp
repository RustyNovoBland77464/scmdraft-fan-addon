// Headless integration check for the live mining position search, using installed BW data.
// Build like minesim.cpp. Usage: boost_test CASC MAP [t|p|z] [base index] [field limit]
#define main minesim_main
#include "minesim.cpp"
#undef main
#include "../hd/src/mining_boost.h"
#include "../hd/src/return_boost.h"
#include "../hd/src/cycle_boost.h"
#include <chrono>
#include <cassert>
#include "boost_study.h"

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    // Facing is relative to each depot, including diagonal mineral lines and boundaries.
    mining_boost::DepotApproach west{{100,100},{0,100},true};
    assert(west.allows({90,110}) && !west.allows({110,90}) && west.allows({100,150}));
    mining_boost::DepotApproach diagonal{{100,100},{0,0},true};
    assert(diagonal.allows({90,105}) && !diagonal.allows({110,95}));
    mining_boost::DepotApproach missing{}, coincident{{100,100},{100,100},true};
    assert(!missing.allows({0,0}) && !coincident.allows({100,100}));
    // A reproducible 4-trip route is stable even without three identical consecutive trips.
    mining_boost::Cycles periodic; periodic.delivery(0); int frame=0;
    for(int i=0;i<12;++i) {
        periodic.hash=std::array<uint64_t,4>{{11,11,22,33}}[i%4];
        frame+=std::array<int,4>{{147,147,150,156}}[i%4]; periodic.delivery(frame);
    }
    assert(periodic.stable && periodic.period==4 && periodic.frames==150);
    periodic.hash=99; periodic.delivery(frame+150); assert(!periodic.stable);
    mining_boost::Cycles drifting; drifting.delivery(0);
    for(int i=1;i<=30;++i) {drifting.hash=uint64_t(i); drifting.delivery(i*150); assert(!drifting.stable);}
    casc_loader loader(argv[1]);
    global_state gst; global_init(gst, loader);
    auto chk = load_chk(argv[2]);
    simulator sim{gst, chk, 0, 0, false};
    auto bases = sim.start_bases();
    int race = argc > 3 && argv[3][0] == 'z' ? 0 : argc > 3 && argv[3][0] == 't' ? 1 : 2;
    int baseIndex = argc > 4 ? atoi(argv[4]) : 0;
    int limit = argc > 5 ? atoi(argv[5]) : 100;
    bool cycleBoost = getenv("BOOST_CYCLE") != nullptr;
    bool cargoBoost = getenv("BOOST_RETURN_CARGO") != nullptr;
    bool positionBoost = (!cargoBoost && !cycleBoost) || getenv("BOOST_WITH_POSITION") != nullptr;
    if (baseIndex < 0 || baseIndex >= (int)bases.size()) return 2;
    game_state game; state st; st.global = &gst; st.game = &game;
    game_load_functions f(st);
    f.load_map_data(chk.data(), chk.size(), [&]() {
        for (int i = 0; i < 8; ++i) st.players[i].controller = player_t::controller_neutral;
        f.setup_info.victory_condition = 2; f.setup_info.starting_units = 1;
    });
    st.players[0].controller = player_t::controller_occupied;
    st.players[0].race = (race_t)race;
    for (int phase = boost_study::setting("BOOST_PHASE", 0); phase > 0; --phase) f.next_frame();
    f.create_starting_units(0, bases[baseIndex].pos, (race_t)race);
    auto *depot = [&]() -> unit_t * { for (auto *u : ptr(st.player_units[0])) if (f.ut_resource_depot(u)) return u; return nullptr; }();
    if (!depot) return 2;
    std::vector<size_t> workers, fields;
    for (auto *u : ptr(st.player_units[0])) if (f.ut_worker(u)) workers.push_back(u->index);
    const unit_type_t *wt = f.get_unit(workers[0])->unit_type;
    for (auto *u : ptr(st.visible_units))
        if (f.unit_is_mineral_field(u) && f.xy_length(u->sprite->position - depot->sprite->position) <= 384) fields.push_back(u->index);
    std::sort(fields.begin(), fields.end(), [&](size_t a, size_t b) {
        return f.xy_length(f.get_unit(a)->position - depot->position) < f.xy_length(f.get_unit(b)->position - depot->position);
    });
    std::vector<unit_t *> refineries;
    if(getenv("BOOST_GAS")) {
        for(auto *u:ptr(st.visible_units)) if(f.unit_is(u,UnitTypes::Resource_Vespene_Geyser) &&
            f.xy_length(u->position-depot->position)<=384) refineries.push_back(u);
        std::sort(refineries.begin(),refineries.end(),[](auto *a,auto *b){return a->index<b->index;});
        for(auto *u:refineries) {
            f.add_completed_unit(u,-1,true); f.u_unset_status_flag(u,unit_t::status_flag_completed);
            f.set_unit_owner(u,0,false); f.set_sprite_owner(u,0);
            f.morph_unit(u,f.get_unit_type(race==0?UnitTypes::Zerg_Extractor:race==1?UnitTypes::Terran_Refinery:UnitTypes::Protoss_Assimilator));
            f.create_image(f.get_image_type(ImageTypes::IMAGEID_Vespene_Geyser2),u->sprite,{},f.image_order_below);
            f.finish_building_unit(u); f.complete_unit(u);
        }
    }
    while (workers.size() < fields.size()) {
        auto *u = f.create_starting_unit(wt, depot->position, 0);
        if (!u) return 2;
        workers.push_back(u->index);
    }
    while (workers.size() > fields.size()) { f.destroy_unit(f.get_unit(workers.back())); workers.pop_back(); }
    for (size_t i = 0; i < workers.size(); ++i) {
        auto *m = f.get_unit(fields[i]);
        f.set_unit_resources(m,50000);
        f.issue_order(f.get_unit(workers[i]), false, f.get_order_type(Orders::Harvest1), {m->position, m});
    }
    for(auto *r:refineries) for(int i=0;i<3;++i) {
        auto *w=f.create_starting_unit(wt,depot->position,0); assert(w);
        f.issue_order(w,false,f.get_order_type(Orders::Harvest1),{r->position,r});
    }
    for(int n=boost_study::setting("BOOST_SETTLE_FRAMES",0);n>0;--n) f.next_frame();
    if (getenv("BOOST_LOW_MINERALS")) for (size_t m : fields) f.set_unit_resources(f.get_unit(m), 8);
    // Diagnostic counterfactual: vary only the global scheduler counter, not RNG/spawns.
    if (getenv("BOOST_ORDER_PHASE")) st.order_timer_counter = std::clamp(boost_study::setting("BOOST_ORDER_PHASE", 8), 1, 150);
    if(positionBoost && !fields.empty()) {
        auto *w=f.get_unit(workers[0]), *m=f.get_unit(fields[0]);
        auto approach=mining_boost::depotApproach(f,w,m);
        xy rear=m->position+(m->position-depot->position);
        assert(approach.valid && !approach.allows(rear));
        mining_boost::Search search(st,workers[0],fields[0]);
        size_t count=search.candidates.size(); search.addCandidate(rear);
        assert(search.candidates.size()==count); // observed/refined starts share the same gate
        std::vector<mining_boost::Result> bad{{true,rear,100,0,false}};
        mining_boost::PositionTrial rejected(st,{{workers[0],fields[0]}},bad,6144);
        state_functions trialFuncs(*rejected.st);
        assert(rejected.metrics[0].switched && trialFuncs.get_unit(workers[0])->position==w->position);
    }
    if(cargoBoost && !workers.empty() && !getenv("BOOST_LOW_MINERALS")) {
        auto copy=copy_state(st); state_functions cf(copy); auto *w=cf.get_unit(workers[0]);
        for(int n=0;n<1000 && !(w->carrying_flags&2);++n) cf.next_frame();
        if(w->carrying_flags&2) {
            return_boost::Rule timed{workers[0],fields[0],{},true}; timed.cargoDelay=0;
            assert(!timed.apply(cf)); // cannot infer the pickup time of cargo already carried
            bool issued=false;
            for(int n=0;n<1000 && !issued;++n) {cf.next_frame(); issued=timed.apply(cf);}
            assert(issued && timed.cargoSawEmpty && timed.commands==1);
        }
        return_boost::Rule spatial{workers[0],fields[0],{},true},timed=spatial; timed.cargoDelay=8;
        assert(!return_boost::Plan::same(spatial,timed));
        assert(return_boost::Plan::ruleKey({spatial})!=return_boost::Plan::ruleKey({timed}));
        auto later=timed; later.cargoDelay=9;
        assert(return_boost::Plan::ruleKey({timed})!=return_boost::Plan::ruleKey({later}));
    }
    if (getenv("BOOST_STUDY")) return boost_study::run(st, workers, fields);
    auto before = copy_state(st);
    boost_compute::Executor pool;
    auto *executor = getenv("BOOST_SERIAL") ? nullptr : &pool;
    auto t0 = std::chrono::steady_clock::now();
    if (getenv("BOOST_TIMED")) {
        std::vector<mining_boost::Assignment> a;
        for(size_t i=0;i<fields.size();++i) a.push_back({workers[i],fields[i]});
        {
            boost_compute::Executor e(2); boost_compute::Memo<int> memo;
            std::atomic<int> calls{0}; auto run=[&]{++calls; return 7;};
            auto one=memo.submit(e,{1,2,3},run), again=memo.submit(e,{1,2,3},run);
            assert(one.get()==7 && again.get()==7 && calls==1 && memo.hits==1);
            assert(memo.submit(e,{1,2,4},run).get()==7 && calls==2);
        }
        auto test=[&](auto factory) {
            using P=typename decltype(factory(st,(boost_compute::Executor*)nullptr))::element_type;
            double slice=getenv("BOOST_TIMED_SECONDS")?atof(getenv("BOOST_TIMED_SECONDS")):0.15;
            double more=getenv("BOOST_TIMED_MORE")?atof(getenv("BOOST_TIMED_MORE")):0.25;
            boost_compute::Job<P> job(st,factory,slice);
            auto wait=[&]{while(!job.done()) std::this_thread::sleep_for(std::chrono::milliseconds(2));};
            wait();
            auto *frontier=job.frontier.get(); auto *seed=job.input;
            double prior=job.result->score(); size_t completed=job.executor.completed;
            double used=job.seconds();
            assert(used>=slice || job.exhausted);
            assert(used<slice+1.0);
            for(int n=0;n<3 && !job.exhausted;++n) {
                job.extend(more); wait();
                assert(job.frontier.get()==frontier && job.input==seed);
                assert(job.result->score()+0.000001>=prior);
                assert(job.executor.completed>=completed && job.seconds()>=used);
                prior=job.result->score(); completed=job.executor.completed; used=job.seconds();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            size_t parkedChecks=job.executor.completed;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            assert(job.executor.completed==parkedChecks);
            if constexpr(std::is_same_v<P,mining_boost::Plan>) {
                size_t options=0;
                bwgame::state_functions sf(*job.input);
                for(size_t i=0;i<a.size();++i) {
                    auto approach=mining_boost::depotApproach(sf,sf.get_unit(a[i].worker),sf.get_unit(a[i].field));
                    for(const auto &r:job.frontier->options[i]) assert(approach.allows(r.position));
                    if(job.result->positions[i].stable) assert(approach.allows(job.result->positions[i].position));
                    options+=job.frontier->options[i].size();
                }
                printf("position frontier: phase %d, dense %d, %zu lineup options\n",job.frontier->phase,job.frontier->denseExpanded,options);
            }
            assert(st.current_frame==before.current_frame && st.lcg_rand_state==before.lcg_rand_state);
            printf("timed %s: %.3fs %zu checks score %.6f, memo %zu unique / %zu hits, fixed seed and monotone checkpoints OK\n",typeid(P).name(),used,completed,prior,job.frontier->memo.entries.size(),job.frontier->memo.hits); fflush(stdout);
        };
        std::string kind=getenv("BOOST_TIMED_KIND")?getenv("BOOST_TIMED_KIND"):"";
        if(kind.empty() || kind=="position") test([&](const state &s,boost_compute::Executor *e){return std::make_unique<mining_boost::Plan>(s,a,e);});
        if(kind.empty() || kind=="cargo") test([&](const state &s,boost_compute::Executor *e){return std::make_unique<return_boost::Plan>(s,a,std::vector<return_boost::Rule>{},e);});
        if(kind.empty() || kind=="cycle") test([&](const state &s,boost_compute::Executor *e){return std::make_unique<cycle_boost::Plan>(s,a,std::vector<return_boost::Rule>{},e);});
        return 0;
    }
    if (getenv("BOOST_CANCEL")) {
        std::vector<mining_boost::Assignment> a;
        for (size_t i = 0; i < fields.size(); ++i) a.push_back({workers[i], fields[i]});
        {
            boost_compute::Job<mining_boost::Plan> job(st, [a](const state &s, boost_compute::Executor *e) {
                return std::make_unique<mining_boost::Plan>(s, a, e);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        {
            boost_compute::Job<return_boost::Plan> job(st, [a](const state &s, boost_compute::Executor *e) {
                return std::make_unique<return_boost::Plan>(s, a, std::vector<return_boost::Rule>{}, e);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        {
            boost_compute::Job<cycle_boost::Plan> job(st, [a](const state &s, boost_compute::Executor *e) {
                return std::make_unique<cycle_boost::Plan>(s, a, std::vector<return_boost::Rule>{}, e);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        assert(st.current_frame == before.current_frame);
        printf("all three background jobs cancelled safely\n");
        return 0;
    }
    if (positionBoost && limit < 100) for (size_t i = 0; i < fields.size() && (int)i < limit; ++i) {
        mining_boost::Search search(before, workers[i], fields[i]);
        while (!search.done()) search.advance(4096);
        for(size_t n=1;n<search.candidates.size();++n) assert(search.approach.allows(search.candidates[n]));
        for(const auto &r:search.options) assert(search.approach.allows(r.position));
        for(const auto &r:search.samples) assert(search.approach.allows(r.position));
        if(search.best.stable) assert(search.approach.allows(search.best.position));
        printf("field %zu: baseline %.2f (%d), best %.2f (%d) at %d,%d; %d/%d stable\n", i,
            search.baseline.frames, search.baseline.stable, search.best.frames, search.best.stable,
            search.best.position.x, search.best.position.y, search.accepted, search.tested);
        fflush(stdout);
        if (search.best.stable) f.move_unit(f.get_unit(workers[i]), search.best.position);
    }
    if (positionBoost && limit >= 100) {
        std::vector<mining_boost::Assignment> a;
        for (size_t i = 0; i < fields.size(); ++i) a.push_back({workers[i], fields[i]});
        mining_boost::Plan plan(before, a, executor);
        int initialFrame = st.current_frame, initialMinerals = st.total_minerals_gathered[0];
        std::vector<xy> initialPositions;
        for (size_t w : workers) initialPositions.push_back(f.get_unit(w)->position);
        size_t last = 0; int phase = 0, pass = 0;
        while (!plan.done()) {
            plan.advance(4096);
            if (last != plan.field || phase != plan.phase || pass != plan.pass) {
                printf("progress: field %zu/%zu, phase %d, pass %d\n", plan.field, fields.size(), plan.phase, plan.pass);
                last = plan.field; phase = plan.phase; pass = plan.pass; fflush(stdout);
            }
        }
        assert(st.current_frame == initialFrame && st.total_minerals_gathered[0] == initialMinerals);
        for (size_t i = 0; i < workers.size(); ++i) assert(f.get_unit(workers[i])->position == initialPositions[i]);
        for (size_t i = 0; i < fields.size(); ++i) {
            auto r = plan.positions[i];
            auto approach=mining_boost::depotApproach(f,f.get_unit(workers[i]),f.get_unit(fields[i]));
            for(const auto &option:plan.options[i]) assert(approach.allows(option.position));
            if(r.stable) assert(approach.allows(r.position));
            printf("field %zu: normal %.2f -> %.2f at %d,%d verified=%d option %zu/%zu\n", i,
                plan.normalFrames[i], r.frames, r.position.x, r.position.y, r.stable, plan.choice[i], plan.options[i].size());
            if (r.stable) {
                assert(r.frames > 0 && (!plan.normalFrames[i] || r.frames <= plan.normalFrames[i] + 0.1));
                assert(!f.is_blocked(f.get_unit(workers[i]), r.position).first);
                const auto &m=plan.selected[i], &control=plan.control[i];
                assert(!m.switched && m.cycles.stable);
                for (int b=1; b<=2; ++b) assert(m.stable[b-1] && m.trips[b]>=8 && m.mean(b)<=control.mean(b)+0.1);
                f.move_unit(f.get_unit(workers[i]), r.position);
            }
        }
    }
    std::vector<return_boost::Rule> cargoRules;
    std::vector<bool> commandModes;
    if (cargoBoost) commandModes.push_back(false);
    if (cycleBoost) commandModes.push_back(true);
    for (bool cycleMode : commandModes) {
        before = copy_state(st); // control includes the same selected spawn positions when combining
        std::vector<mining_boost::Assignment> a;
        for (size_t i = 0; i < fields.size(); ++i) a.push_back({workers[i], fields[i]});
        return_boost::SearchConfig commandConfig;
        commandConfig.pickupTiming=!getenv("BOOST_SPATIAL_ONLY");
        return_boost::Plan plan(st, a, cargoRules, executor, cycleMode, commandConfig);
        size_t last = 0; int phase = 0;
        while (!plan.done()) {
            plan.advance(4096);
            if (last != plan.worker || phase != plan.phase) {
                printf("%s progress: worker %zu/%zu, phase %d\n", cycleMode ? "cycle" : "cargo", plan.worker, fields.size(), plan.phase);
                last = plan.worker; phase = plan.phase; fflush(stdout);
            }
        }
        assert(st.current_frame == before.current_frame && st.total_minerals_gathered[0] == before.total_minerals_gathered[0]);
        for (size_t i = 0; i < workers.size(); ++i) {
            assert(f.get_unit(workers[i])->position == state_functions(before).get_unit(workers[i])->position);
            const auto &r = plan.chosen[i]; const auto &m = plan.selected[i];
            printf("command field %zu: %.3f -> %.3f frames, %.3f -> %.3f px, at %d,%d enabled=%d commands=%d/%d pickup_delay=%d\n", i,
                plan.baseline[i].mean(), m.mean(), plan.baseline[i].path(), m.path(), r.spot.x, r.spot.y, r.enabled, m.commandedTrips, m.trips, r.cargoDelay);
            if (r.enabled) assert(plan.improves(m, plan.baseline[i], true));
        }
        if(!cycleMode && getenv("BOOST_CARGO_DIAG")) {
            size_t top=0;
            for(size_t i=1;i<fields.size();++i) if(f.get_unit(fields[i])->position.y<f.get_unit(fields[top])->position.y) top=i;
            for(int delay=0;delay<=30;++delay) {
                auto rules=plan.chosen; rules[top]={a[top].worker,a[top].field,{},true}; rules[top].cargoDelay=delay;
                return_boost::Trial diagnostic(st,a,rules,cargoRules,16384,false,false,4096);
                while(!diagnostic.done()) diagnostic.advance(4096);
                const auto &m=diagnostic.metrics[top];
                printf("diagnostic top delay=%d mean=%.6f path=%.6f stable=%d blocks=%d/%d commands=%d/%d rate=%.6f\n",
                    delay,m.mean(),m.path(),m.cycles.stable,m.blocks[0].stable,m.blocks[1].stable,
                    m.commandedTrips,m.trips,diagnostic.rate());
            }
        }
        if(!cycleMode && getenv("BOOST_REQUIRE_TOP_FRAMES")) {
            size_t top=0;
            for(size_t i=1;i<fields.size();++i) if(f.get_unit(fields[i])->position.y<f.get_unit(fields[top])->position.y) top=i;
            double ceiling=atof(getenv("BOOST_REQUIRE_TOP_FRAMES"));
            assert(plan.selected[top].mean()<=ceiling);
            if(plan.baseline[top].mean()>ceiling) assert(plan.chosen[top].enabled);
            if(plan.chosen[top].enabled) assert(plan.improves(plan.selected[top],plan.baseline[top],true));
            return_boost::Trial holdout(st,a,plan.chosen,cargoRules,24576,false,false,8192);
            while(!holdout.done()) holdout.advance(4096);
            const auto &m=holdout.metrics[top];
            assert(!m.switched && m.cycles.stable && (!plan.chosen[top].enabled || m.commandedTrips==m.trips) && m.mean()<=ceiling);
            printf("top regression: base %d field %zu at %d,%d pickup_delay=%d validated=%.6f holdout=%.6f commands=%d/%d\n",
                baseIndex,top,f.get_unit(fields[top])->position.x,f.get_unit(fields[top])->position.y,
                plan.chosen[top].cargoDelay,plan.selected[top].mean(),m.mean(),m.commandedTrips,m.trips);
        }
        cargoRules.insert(cargoRules.end(), plan.chosen.begin(), plan.chosen.end());
        for (const auto &r : plan.chosen) if (r.enabled && r.cycle)
            printf("cycle worker %zu via %zu delay %d detour %d\n", r.worker, r.neighbor, r.delay, r.detour);
    }
    // Each lineup is allowed to settle and then runs unchanged for a full game minute.
    auto measure = [&](state &s, const char *name) {
        state_functions fn(s);
        std::vector<mining_boost::Cycles> cycles(workers.size());
        int income0 = 0;
        auto rules = cargoRules;
        int commands = 0;
        for (int frame = 0; frame < 720 + 1429; ++frame) {
            if (frame == 720) income0 = s.total_minerals_gathered[0];
            std::vector<int> cargo;
            for (size_t index : workers) {
                auto *w = fn.get_unit(index); cargo.push_back(w ? w->carrying_flags : 0);
            }
            if (name[0] == 'b') for (auto &r : rules) if (r.apply(fn)) {
                auto *w = fn.get_unit(r.worker);
                assert(w && (r.cycle ? (w->carrying_flags & 3) == 0 : (w->carrying_flags & 3) == 2));
                if (r.cycle) {
                    assert(!r.failed && (r.stage == 1 || r.stage == 2));
                    auto *target = fn.get_unit(r.stage == 1 ? r.neighbor : r.field);
                    bool targeted = w->order_type->id == Orders::Harvest1 && w->order_target.unit == target;
                    // OpenBW queues an order behind a non-interruptible animation.
                    for (const auto &order : w->order_queue)
                        if (order.order_type->id == Orders::Harvest1 && order.target.unit == target) targeted = true;
                    assert(targeted);
                }
                else assert(r.issued); // iscript may temporarily change the active order after issue_order
                ++commands;
            }
            fn.next_frame();
            for (size_t i = 0; i < workers.size(); ++i) {
                auto *w = fn.get_unit(workers[i]);
                if (!w) continue;
                cycles[i].sample(w);
                if ((cargo[i] & 2) && !(w->carrying_flags & 2)) {
                    if (i == 0 && getenv("BOOST_TRACE")) printf("trace %s frame %d dt=%d hash=%llx\n", name,
                        s.current_frame, s.current_frame - cycles[i].lastFrame, (unsigned long long)cycles[i].hash);
                    cycles[i].delivery(s.current_frame);
                }
            }
        }
        int stable = 0; double inverse = 0;
        for (size_t i = 0; i < cycles.size(); ++i) {
            if (cycles[i].stable) ++stable;
            if (cycles[i].frames > 0) inverse += 8.0 * 1429 / cycles[i].frames;
            printf("%s field %zu: %.2f frames stable=%d repeats=%d\n", name, i, cycles[i].frames, cycles[i].stable, cycles[i].repeats);
        }
        printf("%s: %d minerals/min; %.2f cycle-based minerals/min; %d/%zu stable\n", name,
            s.total_minerals_gathered[0] - income0, inverse, stable, workers.size());
        if (cargoBoost || cycleBoost) printf("%s: %d boost commands\n", name, commands);
    };
    measure(before, "normal"); measure(st, "boosted");
    printf("search + validation: %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}
