// Search workers own private engine states; only immutable game/global data is shared.
#pragma once
#include <algorithm>
#include <memory>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <stdexcept>
#include <map>
#include <cmath>
#include <cstdint>

namespace boost_compute {
// OpenBW's nominally const region graph has mutable pathfinder scratch pointers. Give
// each concurrent task its own graph, including neighbor/split links and existing long paths.
// Type-table links still refer to the original immutable data, kept alive by the owning Game.
struct World {
    bwgame::game_state game;
    bwgame::state st;
    explicit World(const bwgame::state &source) : st(bwgame::copy_state(source)) {
        const auto &from = *source.game;
        game.map_tile_width = from.map_tile_width;
        game.map_tile_height = from.map_tile_height;
        game.map_walk_width = from.map_walk_width;
        game.map_walk_height = from.map_walk_height;
        game.map_width = from.map_width;
        game.map_height = from.map_height;
        game.map_strings = from.map_strings;
        game.scenario_name = from.scenario_name;
        game.scenario_description = from.scenario_description;
        game.unit_air_strength = from.unit_air_strength;
        game.unit_ground_strength = from.unit_ground_strength;
        game.forces = from.forces;
        game.sight_values = from.sight_values;
        game.tileset_index = from.tileset_index;
        game.gfx_tiles = from.gfx_tiles;
        game.cv5 = from.cv5;
        game.vf4 = from.vf4;
        game.mega_tile_flags = from.mega_tile_flags;
        game.unit_types.vec = from.unit_types.vec;
        game.weapon_types.vec = from.weapon_types.vec;
        game.upgrade_types.vec = from.upgrade_types.vec;
        game.tech_types.vec = from.tech_types.vec;
        game.unit_type_allowed = from.unit_type_allowed;
        game.max_upgrade_levels = from.max_upgrade_levels;
        game.tech_available = from.tech_available;
        game.start_locations = from.start_locations;
        game.max_unit_width = from.max_unit_width;
        game.max_unit_height = from.max_unit_height;
        game.repulse_field_width = from.repulse_field_width;
        game.repulse_field_height = from.repulse_field_height;
        game.regions = from.regions;
        game.triggers = from.triggers;
        auto remap = [&](auto *p) { return p ? &game.regions.regions.at(p->index) : nullptr; };
        for (auto &r : game.regions.regions) {
            for (auto &p : r.walkable_neighbors) p = remap(p);
            for (auto &p : r.non_walkable_neighbors) p = remap(p);
            r.pathfinder_flag = 0; r.pathfinder_node = nullptr;
        }
        for (auto &r : game.regions.split_regions) { r.a = remap(r.a); r.b = remap(r.b); }
        for (auto &path : st.paths) for (auto &p : path.long_path) p = remap(p);
        st.game = &game;
    }
};
struct Cancelled : std::exception {};
struct Executor {
    std::atomic<bool> cancelled{false}, paused{false};
    mutable std::mutex pauseMutex;
    mutable std::condition_variable pauseWake;
    std::atomic<size_t> completed{0}, running{0};
    mutable std::atomic<size_t> parked{0};
    std::mutex mutex;
    std::condition_variable wake;
    std::queue<std::function<void()>> queue;
    std::vector<std::thread> threads;
    bool stopping = false;
    static unsigned concurrency() {
        unsigned n = std::thread::hardware_concurrency();
        return std::min(9u, n > 1 ? n - 1 : 1u);
    }
    explicit Executor(unsigned n = concurrency()) {
        try {
            for (unsigned i = 0; i < std::max(1u, n); ++i) threads.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [&] { return stopping || !queue.empty(); });
                        if (stopping && queue.empty()) return;
                        task = std::move(queue.front()); queue.pop(); ++running;
                    }
                    task(); ++completed;
                    {std::lock_guard<std::mutex> lock(pauseMutex); --running;}
                    pauseWake.notify_all();
                }
            });
        } catch (...) { stop(); throw; }
    }
    void check() const {
        if (cancelled.load()) throw Cancelled();
        if (paused.load()) {
            std::unique_lock<std::mutex> lock(pauseMutex);
            if(paused.load()) {
                ++parked; pauseWake.notify_all();
                pauseWake.wait(lock, [&] { return cancelled.load() || !paused.load(); });
                --parked;
            }
        }
        if (cancelled.load()) throw Cancelled();
    }
    void pause(bool value) {
        std::unique_lock<std::mutex> lock(pauseMutex);
        paused=value;
        if(!value) {lock.unlock(); pauseWake.notify_all(); return;}
        pauseWake.wait(lock,[&]{return cancelled.load() || running.load()==parked.load();});
    }
    void cancel() { {std::lock_guard<std::mutex> lock(pauseMutex); cancelled=true;} pauseWake.notify_all(); }
    template<class F> auto submit(F f) -> std::future<decltype(f())> {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        auto future = task->get_future();
        { std::lock_guard<std::mutex> lock(mutex); queue.push([task] { (*task)(); }); }
        wake.notify_one(); return future;
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_all();
        for (auto &t : threads) if (t.joinable()) t.join();
    }
    ~Executor() { cancel(); stop(); }
};

// Memoize the complete lineup, not just the worker's command/position: changing another
// worker is a different phase-space case. Reuse in-flight results as well as completed results.
template<class T> struct Memo {
    std::map<std::vector<int64_t>, std::shared_future<T>> entries;
    size_t hits=0;
    void remember(std::vector<int64_t> key,const T &value) {
        if(entries.count(key)) return;
        std::promise<T> ready; ready.set_value(value);
        entries.emplace(std::move(key),ready.get_future().share());
    }
    template<class F> std::shared_future<T> submit(Executor &pool, std::vector<int64_t> key, F run) {
        auto it=entries.find(key);
        if (it!=entries.end()) {++hits; return it->second;}
        auto future=pool.submit(std::move(run)).share();
        entries.emplace(std::move(key),future); return future;
    }
};

// A time slice pauses the controller and every worker at a simulation checkpoint. The private
// plan, queued futures, fixed input, candidate cursors and memo tables survive until Continue.
// Validation uses a separate private state and never exposes provisional choices to the UI.
template<class Plan> struct Job {
    std::unique_ptr<World> world;
    bwgame::state *input;
    std::unique_ptr<typename Plan::Output> result;
    std::unique_ptr<Plan> frontier;
    Executor executor;
    std::atomic<bool> ready{false}, validating{false}, exhausted{false}, changed{false};
    std::atomic<double> elapsed{0}, sliceElapsed{0}, sliceLimit{0};
    std::exception_ptr failure;
    std::mutex mutex;
    std::condition_variable wake;
    double allowance=0;
    bool requested=true;
    std::thread control;
    double seconds() const { return elapsed.load(); }
    void extend(double seconds) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!ready.load() || exhausted.load()) return;
        allowance=seconds; requested=true; ready=false; wake.notify_one();
    }
    template<class Factory> Job(const bwgame::state &st, Factory factory, double seconds=0)
        : world(std::make_unique<World>(st)), input(&world->st), allowance(seconds) {
        control=std::thread([this,factory=std::move(factory)] {
            try {
                frontier=factory(*input,&executor);
                std::vector<int64_t> lastProposal;
                for (;;) {
                    double limit;
                    { std::unique_lock<std::mutex> lock(mutex);
                      wake.wait(lock,[&]{return requested || executor.cancelled.load();});
                      if (executor.cancelled.load()) return;
                      requested=false; limit=allowance; }
                    changed=false; sliceElapsed=0; sliceLimit=limit; executor.pause(false);
                    double previous=elapsed.load();
                    auto start=std::chrono::steady_clock::now();
                    auto spent=[&]{return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();};
                    bool empty=false;
                    while (limit<=0 || spent()<limit) {
                        executor.check(); sliceElapsed=spent(); elapsed=previous+spent();
                        if (frontier->done() && (limit<=0 || !frontier->expand())) { empty=true; break; }
                        frontier->advance(128);
                        std::this_thread::yield();
                    }
                    executor.pause(true);
                    sliceElapsed=spent(); elapsed=previous+spent();
                    validating=true;
                    auto proposal=frontier->proposal();
                    if (!result || proposal!=lastProposal) {
                        auto verified=frontier->checkpoint([&]{if(executor.cancelled.load()) throw Cancelled();});
                        if (!result || verified->score()>result->score()+0.000001 ||
                            (std::abs(verified->score()-result->score())<=0.000001 && verified->selections()>result->selections())) {
                            result=std::move(verified); changed=true;
                        }
                        lastProposal=std::move(proposal);
                    }
                    validating=false; exhausted=empty;
                    ready.store(true,std::memory_order_release);
                }
            } catch (...) {
                failure=std::current_exception(); validating=false;
                ready.store(true,std::memory_order_release);
            }
        });
    }
    bool done() {
        if (!ready.load(std::memory_order_acquire)) return false;
        if (failure) std::rethrow_exception(failure);
        return true;
    }
    ~Job() {
        executor.cancel();
        {std::lock_guard<std::mutex> lock(mutex); requested=true;}
        wake.notify_all();
        if (control.joinable()) control.join();
        executor.stop();
    }
};
} // namespace boost_compute
