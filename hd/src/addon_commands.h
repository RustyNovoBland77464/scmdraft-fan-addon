#pragma once

namespace addon_commands {
struct Entry { const char *label; unsigned key; };
constexpr Entry entries[] = {
    {"HD Graphics: Off", 0},
    {"HD Graphics: Remastered", 0},
    {"HD Graphics: Carbot", 0},
    {"Test Mining\tCtrl+Shift+M", 'M'},
    {"Copy Mirrored (Left-Right)\tCtrl+Shift+C", 'C'},
    {"Replace by Unit Sprite\tCtrl+Shift+R", 'R'},
    {"Show Building Hitboxes\tCtrl+Shift+1", '1'},
    {"Show Unit Hitboxes\tCtrl+Shift+2", '2'},
};
constexpr int shortcut(unsigned key, bool control, bool shift, bool alt) {
    if (!control || !shift || alt) return -1;
    for (int i = 0; i < 8; ++i) if (entries[i].key && entries[i].key == key) return i;
    return -1;
}
} // namespace addon_commands
