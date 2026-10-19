// clang++ -std=c++17 -fsanitize=address,undefined hd/tests/map_sampling_test.cpp -o /tmp/map_sampling_test
#include "../src/map_sampling.h"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
int main() {
    // Deliberately bright edge/padding exposes accidental edge replication. Black is zero.
    std::vector<uint32_t> source(128 + 64, 0xff00ff);
    for (int x = 0; x < 128; ++x) source[x] = 0x100 + x;
    auto axis = map_sampling::bounds(source.size(), 0, 128);
    for (float scale : {4.f, 2.f, 1.f, .5f, .25f}) {
        int edge = int(128 / scale);
        std::vector<uint32_t> full(edge + 100), partial(60);
        auto paint = [&](auto &out, int start) {
            for (size_t i = 0; i < out.size(); ++i) {
                int sample = axis.sample((start + i + .5f) * scale);
                out[i] = sample < 0 ? 0 : source[sample];
            }
        };
        paint(full, 0); paint(partial, edge - 20);
        assert(full[edge - 1] != 0);
        for (size_t i = edge; i < full.size(); ++i) assert(full[i] == 0);
        for (size_t i = 0; i < partial.size(); ++i) assert(partial[i] == full[edge - 20 + i]);
    }
    // Scrolled buffer origin, a negative source coordinate, and half-size-buffer conversion.
    auto scrolled = map_sampling::bounds(192, 96, 128);
    assert(scrolled.sample(31.5f) == 31 && scrolled.sample(32.f) == -1);
    assert(axis.sample(-.5f) == -1);
    auto inset = map_sampling::bounds(192, -32, 128);
    assert(inset.sample(31.5f) == -1 && inset.sample(32.f) == 32 && inset.sample(160.f) == -1);
    auto beyond = map_sampling::bounds(192, 256, 128);
    assert(beyond.sample(0.f) == -1);
    assert(axis.sample((63 + .5f) * 2) == 127 && axis.sample((64 + .5f) * 2) == -1);
    puts("Map edges and partial repaints stay black at 25–400% zoom.");
}
