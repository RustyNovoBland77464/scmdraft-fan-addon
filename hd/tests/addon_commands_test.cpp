#include "../src/addon_commands.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main() {
    using namespace addon_commands;
    assert(shortcut('C', true, true, false) == 4);
    assert(shortcut('M', true, true, false) == 3);
    assert(shortcut('R', true, true, false) == 5);
    assert(shortcut('1', true, true, false) == 6);
    assert(shortcut('2', true, true, false) == 7);
    for (const auto &entry : entries) if (entry.key) {
        assert(shortcut(entry.key, false, true, false) == -1);
        assert(shortcut(entry.key, true, false, false) == -1);
        assert(shortcut(entry.key, true, true, true) == -1);
    }
    // Existing ScmDraft Ctrl+Shift accelerators remain available (Copy is the intentional override).
    for (char key : {'X', 'Z', 'F', 'W', 'S', 'E', 'H', 'U', 'T', 'B'})
        assert(shortcut(key, true, true, false) == -1);
    for (int i = 0; i < 8; ++i) for (int j = i+1; j < 8; ++j)
        assert(!entries[i].key || entries[i].key != entries[j].key);
    std::puts("Add-on shortcuts preserve native chords and require exact modifiers.");
}
