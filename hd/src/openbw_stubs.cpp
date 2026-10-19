// Copied from scbwfactory/engine/shim/headless_stubs.cpp (the user's project).
// headless_stubs.cpp — no-op implementations of OpenBW's ui platform layer.
//
// ui/ui.h's draw functions render into plain byte buffers; SDL is only needed
// for the interactive window, sound, and SC:R "new image" PNGs — none of which
// the shim's render() uses. These stubs satisfy the linker so we can reuse the
// real game renderer (tiles + sprites from the MPQs) without an SDL dependency.
// Anything that would require an actual window/audio device aborts loudly.

#include "ui/common.h"
#include "ui/native_window.h"
#include "ui/native_window_drawing.h"
#include "ui/native_sound.h"

#include <cstdio>
#include <cstdlib>

namespace bwgame {
namespace ui {

void log_str(a_string str) {
    std::fputs(str.c_str(), stderr);
}

void fatal_error_str(a_string str) {
    std::fprintf(stderr, "openbw ui fatal error: %s\n", str.c_str());
    std::abort();
}

} // namespace ui
} // namespace bwgame

namespace native_window {

struct window_impl {};   // never instantiated

window::window() = default;
window::~window() = default;
window::window(window&&) = default;
void window::destroy() {}
bool window::create(const char*, int, int, int, int) {
    bwgame::ui::fatal_error_str("headless build: window::create is unavailable");
    return false;
}
void window::get_cursor_pos(int* x, int* y) { *x = -1; *y = -1; }
bool window::peek_message(event_t&) { return false; }
bool window::show_cursor(bool) { return false; }
bool window::get_key_state(int) { return false; }
bool window::get_mouse_button_state(int) { return false; }
void window::update_surface() {}
window::operator bool() const { return impl != nullptr; }  // always false

} // namespace native_window

namespace native_window_drawing {

palette* new_palette() {
    bwgame::ui::fatal_error_str("headless build: palettes are unavailable");
    return nullptr;
}
void delete_palette(palette*) {}
std::unique_ptr<surface> create_rgba_surface(int, int) {
    bwgame::ui::fatal_error_str("headless build: surfaces are unavailable");
    return nullptr;
}
std::unique_ptr<surface> get_window_surface(native_window::window*) {
    bwgame::ui::fatal_error_str("headless build: surfaces are unavailable");
    return nullptr;
}
std::unique_ptr<surface> convert_to_8_bit_indexed(surface*) {
    bwgame::ui::fatal_error_str("headless build: surfaces are unavailable");
    return nullptr;
}
std::unique_ptr<surface> load_image(const char*) { return nullptr; }
std::unique_ptr<surface> load_image(const void*, size_t) { return nullptr; }

} // namespace native_window_drawing

namespace native_sound {

int frequency = 0;
int channels = 0;

void init() {}
void play(int, sound*, int, int) {}
bool is_playing(int) { return false; }
void stop(int) {}
void set_volume(int, int) {}
std::unique_ptr<sound> load_wav(const void*, size_t) { return nullptr; }

} // namespace native_sound
