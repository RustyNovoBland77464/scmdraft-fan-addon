// Reproducible experiments against the production search implementations.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cassert>

namespace boost_study {
using namespace bwgame;
inline int setting(const char *key, int fallback) {
    auto *value = std::getenv(key);
    return value ? std::atoi(value) : fallback;
}
inline double seconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
inline int run(const state &source, const std::vector<size_t> &workers, const std::vector<size_t> &fields) {
    const char *mode = std::getenv("BOOST_STUDY");
    int repeats = std::max(1, setting("BOOST_REPEATS", 1));
    int stride = std::max(1, setting("BOOST_STRIDE", 4));
    mining_boost::SearchConfig positionConfig;
    positionConfig.stride = stride;
    positionConfig.adaptive = !std::getenv("BOOST_FIXED_GRID");
    if (std::getenv("BOOST_ALL_RINGS")) positionConfig.gaps = {1, 2, 3, 4, 5};
    return_boost::SearchConfig commandConfig;
    commandConfig.cargoStride = stride;
    commandConfig.adaptive = !std::getenv("BOOST_FIXED_GRID");
    commandConfig.earlyReject = !std::getenv("BOOST_FULL_TRIALS");
    commandConfig.pickupTiming = !std::getenv("BOOST_SPATIAL_ONLY");
    int step = setting("BOOST_TIMING_STEP", 0);
    if (step > 0) {
        commandConfig.delays.clear(); commandConfig.detours.clear();
        for (int t = 0; t <= 28; t += step) commandConfig.delays.push_back(t);
        for (int t = step; t <= 28; t += step) commandConfig.detours.push_back(t);
    }
    std::vector<mining_boost::Assignment> assignments;
    for (size_t i = 0; i < fields.size(); ++i) assignments.push_back({workers[i], fields[i]});
    // Use the same private graph boundary as the live background Job, also for serial trials.
    boost_compute::Executor pool;
    auto *executor = std::getenv("BOOST_SERIAL") ? nullptr : &pool;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        boost_compute::World input(source);
        state_functions f(input.st);
        printf("{\"type\":\"input\",\"repeat\":%d,\"mode\":\"%s\",\"frame\":%d,\"order_phase\":%d,\"rng\":%u,\"stride\":%d,\"memo\":%d,\"rings\":%zu,\"timing_step\":%d,\"serial\":%d}\n",
            repeat, mode, source.current_frame, source.order_timer_counter, source.lcg_rand_state,
            stride, 0, positionConfig.gaps.size(), step, executor == nullptr);
        for (size_t i = 0; i < fields.size(); ++i) {
            auto *w = f.get_unit(workers[i]), *m = f.get_unit(fields[i]);
            printf("{\"type\":\"worker\",\"repeat\":%d,\"field\":%zu,\"worker_id\":%zu,\"mineral_id\":%zu,\"x\":%d,\"y\":%d,\"mx\":%d,\"my\":%d,\"order_timer\":%d}\n",
                repeat, i, workers[i], fields[i], w->position.x, w->position.y, m->position.x, m->position.y, w->order_process_timer);
        }
        fflush(stdout);
        auto start = std::chrono::steady_clock::now();
        if (!std::strcmp(mode, "landscape")) {
            int field = setting("BOOST_FIELD", 0);
            if (field < 0 || field >= int(fields.size())) return 2;
            positionConfig.record = true;
            mining_boost::Search search(input.st, workers[field], fields[field], positionConfig);
            while (!search.done()) search.advance(4096);
            std::set<uint64_t> routes;
            long long frames = 0;
            for (auto &o : search.observations) {
                auto &r = o.result; frames += o.frames;
                if (r.stable) routes.insert(r.route);
                printf("{\"type\":\"candidate\",\"repeat\":%d,\"field\":%d,\"x\":%d,\"y\":%d,\"stable\":%d,\"mean\":%.9f,\"route\":\"%016llx\",\"frames\":%d,\"cached\":%d,\"contact\":%d}\n",
                    repeat, field, r.position.x, r.position.y, r.stable, r.frames,
                    (unsigned long long)r.route, o.frames, o.cached, r.miningStart);
            }
            printf("{\"type\":\"landscape\",\"repeat\":%d,\"tested\":%d,\"stable\":%d,\"routes\":%zu,\"best\":%.9f,\"simulation_frames\":%lld,\"seconds\":%.6f}\n",
                repeat, search.tested, search.accepted, routes.size(), search.best.frames, frames, seconds(start));
            continue;
        }
        std::vector<return_boost::Rule> rules;
        size_t accepted = 0, candidates = 0;
        int rejected = 0;
        if (!std::strcmp(mode, "position")) {
            mining_boost::Plan plan(input.st, assignments, executor, positionConfig);
            while (!plan.done()) plan.advance(4096);
            for (size_t i = 0; i < assignments.size(); ++i) {
                auto &r = plan.positions[i];
                candidates += plan.options[i].size(); accepted += r.stable;
                printf("{\"type\":\"selection\",\"repeat\":%d,\"field\":%zu,\"accepted\":%d,\"normal\":%.9f,\"mean\":%.9f,\"x\":%d,\"y\":%d,\"options\":%zu,\"route\":\"%016llx\"}\n",
                    repeat, i, r.stable, plan.normalFrames[i], r.frames, r.position.x, r.position.y,
                    plan.options[i].size(), (unsigned long long)r.route);
                if (r.stable) {
                    const auto &m=plan.selected[i], &control=plan.control[i];
                    assert(!m.switched && m.cycles.stable && m.mean()<=control.mean()+0.1);
                    for (int b=1; b<=2; ++b) assert(m.stable[b-1] && m.trips[b]>=8 && m.mean(b)<=control.mean(b)+0.1);
                    f.move_unit(f.get_unit(workers[i]),r.position);
                }
            }
            rejected = plan.rejected;
        } else if (!std::strcmp(mode, "cargo") || !std::strcmp(mode, "cycle")) {
            bool cycle = !std::strcmp(mode, "cycle");
            return_boost::Plan plan(input.st, assignments, {}, executor, cycle, commandConfig);
            while (!plan.done()) plan.advance(4096);
            rules = plan.chosen;
            for (size_t i = 0; i < assignments.size(); ++i) {
                auto &r = rules[i]; accepted += r.enabled; candidates += plan.tested[i];
                if (r.enabled) assert(plan.improves(plan.selected[i],plan.baseline[i],true));
                printf("{\"type\":\"selection\",\"repeat\":%d,\"field\":%zu,\"accepted\":%d,\"normal\":%.9f,\"mean\":%.9f,\"x\":%d,\"y\":%d,\"neighbor\":%zu,\"delay\":%d,\"detour\":%d,\"pickup_delay\":%d,\"candidates\":%zu}\n",
                    repeat, i, r.enabled, plan.baseline[i].mean(), plan.selected[i].mean(), r.spot.x, r.spot.y,
                    r.neighbor, r.delay, r.detour, r.cargoDelay, plan.tested[i]);
            }
            rejected = plan.rejected;
        } else return 2;
        double elapsed = seconds(start);
        // Independent, longer holdout, including all unboosted workers in total throughput.
        // Always compare from the same untouched snapshot and measure full trips after warmup.
        double rates[2]{};
        for (int boosted = 0; boosted != 2; ++boosted) {
            boost_compute::World world(boosted ? input.st : source);
            std::vector<return_boost::Rule> own = rules;
            if (!boosted) for (auto &r : own) r.enabled = false;
            return_boost::Trial holdout(world.st, assignments, own, {}, 16384, false, !std::strcmp(mode, "cycle"));
            while (!holdout.done()) holdout.advance(4096);
            rates[boosted] = holdout.rate();
            for (size_t i = 0; i < assignments.size(); ++i) {
                auto &m = holdout.metrics[i];
                if (boosted && i<own.size() && own[i].enabled)
                    assert(!m.switched && m.cycles.stable && m.commandedTrips==m.trips);
                printf("{\"type\":\"holdout\",\"repeat\":%d,\"boosted\":%d,\"field\":%zu,\"mean\":%.9f,\"trips\":%d,\"stable\":%d,\"switched\":%d,\"commanded_trips\":%d}\n",
                    repeat, boosted, i, m.mean(), m.trips, m.cycles.stable, m.switched, m.commandedTrips);
            }
        }
        printf("{\"type\":\"summary\",\"repeat\":%d,\"accepted\":%zu,\"candidates_or_options\":%zu,\"rejected\":%d,\"normal_rate\":%.9f,\"boosted_rate\":%.9f,\"seconds\":%.6f}\n",
            repeat, accepted, candidates, rejected, rates[0], rates[1], elapsed);
        fflush(stdout);
    }
    return 0;
}
} // namespace boost_study
