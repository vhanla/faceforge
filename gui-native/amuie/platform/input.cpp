#include "platform/input.hpp"

#include "core/types.hpp"
#include "platform/window.hpp"

#include <GLFW/glfw3.h>

namespace platform {

namespace {

// Resets the fields widgets consume for a single frame. Persistent state
// (focused_id, edits) is intentionally left alone.
void reset_frame_input(UIInput& in) {
    in.char_count = 0;

    in.key_backspace = 0;
    in.key_delete = 0;
    in.key_left = 0;
    in.key_right = 0;
    in.key_home = 0;
    in.key_end = 0;
    in.key_enter = 0;
    in.key_escape = 0;
    in.key_tab = 0;

    in.key_shift = 0;
    in.key_ctrl = 0;
    in.key_a = 0;
    in.key_c = 0;
    in.key_v = 0;
    in.key_x = 0;

    in.clipboard = nullptr;
    in.clipboard_out = nullptr;

    in.dropped_count = 0;
}

} // namespace

void feed_ui_input(Window* window, UIContext* ctx) {
    if (!ctx) return;
    UIInput& in = ctx->input;
    reset_frame_input(in);
    if (!window) return;

    in.key_shift = key_raw_down(window, GLFW_KEY_LEFT_SHIFT) ||
                   key_raw_down(window, GLFW_KEY_RIGHT_SHIFT);
    in.key_ctrl = key_raw_down(window, GLFW_KEY_LEFT_CONTROL) ||
                  key_raw_down(window, GLFW_KEY_RIGHT_CONTROL);

    KeyEvent events[64];
    const int n = poll_key_events(window, events, 64);
    for (int i = 0; i < n; ++i) {
        const KeyEvent& e = events[i];
        if (e.action == GLFW_RELEASE) continue;
        const bool ctrl = in.key_ctrl || (e.mods & GLFW_MOD_CONTROL);
        switch (e.key) {
            case GLFW_KEY_BACKSPACE: ++in.key_backspace; break;
            case GLFW_KEY_DELETE: ++in.key_delete; break;
            case GLFW_KEY_LEFT: ++in.key_left; break;
            case GLFW_KEY_RIGHT: ++in.key_right; break;
            case GLFW_KEY_HOME: ++in.key_home; break;
            case GLFW_KEY_END: ++in.key_end; break;
            case GLFW_KEY_ENTER:
            case GLFW_KEY_KP_ENTER: ++in.key_enter; break;
            case GLFW_KEY_ESCAPE: ++in.key_escape; break;
            case GLFW_KEY_TAB: ++in.key_tab; break;
            case GLFW_KEY_A: if (ctrl) in.key_a = 1; break;
            case GLFW_KEY_C: if (ctrl) in.key_c = 1; break;
            case GLFW_KEY_V: if (ctrl) in.key_v = 1; break;
            case GLFW_KEY_X: if (ctrl) in.key_x = 1; break;
            default: break;
        }
    }

    in.char_count = poll_chars(window, in.characters, kMaxInputChars);
    in.clipboard = clipboard_get_text(window);
    in.dropped_count = poll_drops(window, in.dropped_files, kMaxDroppedFiles);
}

} // namespace platform
