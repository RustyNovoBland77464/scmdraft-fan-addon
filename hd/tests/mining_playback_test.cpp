#include "../src/mining_playback.h"
#include <cassert>
#include <cstdio>
int main() {
    using mining_playback::Clock;
    for (int speed : {1, 2, 4}) {
        Clock clock; clock.speed = speed;
        unsigned elapsed = 0, frames = 0;
        for (int tick = 0; tick < 2000; ++tick) {
            unsigned dt = 7 + (tick * 13) % 32; // irregular UI timer delivery
            elapsed += dt; frames += clock.advance(dt);
            assert(frames == elapsed * speed / 42);
        }
        clock.reset();
        assert(clock.advance(10000) == 8 * speed); // no huge catch-up after a modal UI stall
    }
    Clock clock;
    assert(clock.advance(41) == 0);
    clock.reset(); clock.speed = 4;
    assert(clock.advance(10) == 0 && clock.advance(1) == 1);
    // Pausing for search/switching speed clears partial wall time, not engine statistics.
    clock.reset(); assert(clock.advance(0) == 0);
    puts("1x/2x/4x timing stays exact across jitter, pauses, speed changes and UI stalls.");
}
