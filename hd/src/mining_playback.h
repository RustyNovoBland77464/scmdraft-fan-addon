#pragma once
#include <algorithm>
#include <cstdint>

namespace mining_playback {
// Scale wall time, never engine frames. Fractional milliseconds survive timer jitter;
// long UI stalls retain the previous eight-frame catch-up limit at each playback rate.
struct Clock {
    static constexpr unsigned frameMs = 42;
    int speed = 1;
    unsigned credit = 0;
    void reset() { credit = 0; }
    int advance(uint32_t elapsedMs) {
        credit += std::min(elapsedMs, 8 * frameMs) * speed;
        int frames = credit / frameMs;
        credit %= frameMs;
        return frames;
    }
};
}
