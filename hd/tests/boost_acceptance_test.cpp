#include "../src/return_boost.h"
#include <cassert>
#include <cstdio>

static return_boost::Metric metric(int frames, double distance) {
    return_boost::Metric m;
    m.trips = m.commandedTrips = 24; m.frames = frames*24; m.distance = distance*24;
    m.cycles.stable = true;
    for (auto &b : m.blocks) {
        b.trips = b.commandedTrips = 12; b.frames = frames*12; b.distance = distance*12; b.stable = true;
    }
    return m;
}
int main() {
    using return_boost::Plan;
    auto control = metric(167, 83.261297);
    // Protoss left-spawn end patch: reliable 150f cycles on the very same walking route.
    auto boosted = metric(150, 83.261297);
    assert(Plan::improves(boosted, control) && Plan::improves(boosted, control, true));
    boosted = metric(150, 84.414214); // a slightly longer walk can also complete sooner
    assert(Plan::improves(boosted, control, true));
    assert(!Plan::improves(metric(167, 70), control, true));
    assert(!Plan::improves(metric(175, 70), control, true));
    boosted.switched = true; assert(!Plan::improves(boosted, control, true)); boosted.switched = false;
    --boosted.commandedTrips; assert(!Plan::improves(boosted, control, true)); ++boosted.commandedTrips;
    boosted.cycles.stable = false; assert(!Plan::improves(boosted, control, true)); boosted.cycles.stable = true;
    boosted.blocks[0].stable = false; assert(!Plan::improves(boosted, control, true)); boosted.blocks[0].stable = true;
    --boosted.blocks[1].commandedTrips; assert(!Plan::improves(boosted, control, true)); ++boosted.blocks[1].commandedTrips;
    boosted.blocks[1].frames = control.blocks[1].frames; assert(!Plan::improves(boosted, control, true));
    std::puts("Boost acceptance measures sustained cycle speed; route length cannot hide a valid faster cycle.");
}
