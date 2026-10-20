#include "../src/host_image.h"
#include <array>
#include <cassert>
#include <cstdio>
int main() {
    // Covers hook targets, direct calls and the sprite-editor vtable identity check.
    constexpr std::array<uintptr_t, 26> addresses{{
        0x44cff0, 0x486570, 0x488360, 0x453170, 0x501570, 0x488700,
        0x4868d0, 0x487e60, 0x486e40, 0x49f310, 0x43e1d0, 0x438f00,
        0x4b5730, 0x446500, 0x446110, 0x44ad20, 0x44aa10, 0x44bdb0,
        0x486000, 0x486640, 0x459500, 0x48e200, 0x489670, 0x436880,
        0x44d870, 0x4b1af0
    }};
    for (uintptr_t base : {uintptr_t(0x400000), uintptr_t(0x14000000), uintptr_t(0x200000)})
        for (uintptr_t a : addresses) {
            assert(host_image::relocate(base, a)-base == a-0x400000);
            if (base == 0x400000) assert(host_image::relocate(base, a) == a);
        }
    assert(host_image::relocate(0x14000000, 0x44d930) == 0x1404d930);
    std::puts("Host hooks, direct calls and vtable checks follow the executable's loaded base.");
}
