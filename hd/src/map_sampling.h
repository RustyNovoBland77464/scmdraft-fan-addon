#pragma once
#include <algorithm>

namespace map_sampling {
// Source-buffer coordinates that belong to the map, excluding buffer padding.
struct Axis {
    int first, last;
    int sample(float source) const {
        return source >= first && source < last ? static_cast<int>(source) : -1;
    }
};
inline Axis bounds(int bufferSize, int mapOrigin, int mapSize) {
    return {std::max(0, -mapOrigin), std::min(bufferSize, mapSize - mapOrigin)};
}
} // namespace map_sampling
