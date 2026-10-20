#pragma once
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif

// Reverse-engineered addresses refer to the supported executable at its preferred base.
// Windows may relocate that executable; object field offsets do not need relocation.
namespace host_image {
constexpr uintptr_t preferredBase = 0x00400000;
constexpr uintptr_t relocate(uintptr_t loadedBase, uintptr_t preferredAddress) {
    return loadedBase + (preferredAddress - preferredBase);
}
#ifdef _WIN32
inline uintptr_t address(uintptr_t preferredAddress) {
    return relocate(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), preferredAddress);
}
template<class T> inline T pointer(uintptr_t preferredAddress) {
    return reinterpret_cast<T>(address(preferredAddress));
}
bool createHook(uintptr_t preferredAddress, void *replacement, void **original, const char *name);
#endif
} // namespace host_image
