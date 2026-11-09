#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct Window;

struct WindowDesc {
    int width = 1280;
    int height = 800;
    const char* title = "AMUIE";
    bool undecorated = false;
    // Request an ARGB framebuffer so the OS compositor can blend the desktop
    // behind the window.
    bool transparent = false;
    // Ask the compositor to blur the desktop behind the (translucent) window:
    // DWM acrylic on Windows 11, KWin blur-behind on X11/KDE. Best effort; no
    // effect on compositors without a matching API.
    bool acrylic = false;
    bool resizable = true;
    int min_width = 480;   // logical minimum so the UI stays usable on resize
    int min_height = 360;
    bool floating = false;
    bool visible = true;
    bool center = true;
};

enum class Key { Escape, F2, F3, Space, Enter };

// Raw key event: `action` is 0 = release, 1 = press, 2 = repeat (matches GLFW);
// `key` is a GLFW key code; `mods` is a GLFW modifier bitmask. Drained by
// platform::feed_ui_input.
struct KeyEvent {
    int key;
    int action;
    int mods;
};

Window* create(const WindowDesc& desc);
void destroy(Window* window);

// True when the running desktop exposes a blur-behind API this platform can
// drive: DWM on Windows and KWin on X11/KDE. Applications use it to decide
// whether acrylic should be on by default.
bool acrylic_available();

// Whether the window actually got an alpha-capable framebuffer. When false the
// app should keep its opaque theme, otherwise translucent surfaces would be
// composited against black.
bool framebuffer_transparent(Window* window);

void poll_events();

// Event queues, drained once per frame (by feed_ui_input). Each returns the
// number written to `out` (up to `max`).
int poll_key_events(Window* window, KeyEvent* out, int max);
int poll_chars(Window* window, uint32_t* out, int max);
// Dropped file paths since the last call; owned by the platform and valid until
// the next poll_events.
int poll_drops(Window* window, const char** out, int max);

// Clipboard text. The pointer from clipboard_get_text is owned by GLFW and
// valid until the next clipboard call or window destruction.
const char* clipboard_get_text(Window* window);
void clipboard_set_text(Window* window, const char* text);

// Image clipboard (PNG). Implemented on Linux through xclip/wl-copy/wl-paste;
// returns false on platforms/desktops without a supported backend or when the
// clipboard holds no PNG image.
bool clipboard_set_image_png(Window* window, const uint8_t* png, size_t size);
bool clipboard_get_image_png(Window* window, std::vector<uint8_t>& out);
bool should_close(Window* window);
void set_should_close(Window* window);
void swap_buffers(Window* window);
void show(Window* window);

void get_framebuffer_size(Window* window, int* w, int* h);
void get_window_size(Window* window, int* w, int* h);
void get_cursor(Window* window, double* x, double* y);
bool mouse_down(Window* window);
bool mouse_right_down(Window* window);
// True while the middle mouse button is held (used for canvas panning).
bool mouse_middle_down(Window* window);
// Returns the accumulated scroll-wheel delta since the last call and resets it.
double scroll_poll(Window* window);
bool key_pressed(Window* window, Key key);
// Raw GLFW key state (GLFW_KEY_*), for modifiers and text-edit keys.
bool key_raw_down(Window* window, int glfw_key);

void minimize(Window* window);
void toggle_maximize(Window* window);
// Starts a native drag of the PNG image in `png`. Blocks until the drag ends,
// so call it from the UI thread and let the OS own the interaction. Used to
// drag the canvas result out to file managers and browsers without saving it
// first. Returns true when a target accepted the drop; false when the platform
// has no drag-out backend or nothing accepted. Best-effort on X11 (XDND
// text/uri-list pointing at a temporary file).
bool drag_source_file(Window* window, const uint8_t* png, size_t size);
void drag_update(Window* window, bool active);
void drag_stop(Window* window);
void resize_update(Window* window, bool active);
void resize_stop(Window* window);
void set_cursor_resize(Window* window, bool resize);

double time_seconds();
const char* platform_name();

// Native file choosers. `filter` is a space-separated list of globs
// ("*.png *.jpg"); an empty filter allows anything. Return an empty string when
// cancelled or when no dialog backend is available (e.g. no zenity/kdialog).
// On Linux these block the UI thread while the external dialog runs.
std::string open_file_dialog(const char* title, const char* filter,
                             const char* initial_path);
std::string save_file_dialog(const char* title, const char* filter,
                             const char* initial_path, const char* default_name);

} // namespace platform
