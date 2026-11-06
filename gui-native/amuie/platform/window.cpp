#include "platform/window.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace platform {

struct Window {
    GLFWwindow* handle = nullptr;
    bool dragging = false;
    double drag_last_x = 0.0;
    double drag_last_y = 0.0;
    bool resizing = false;
    double resize_last_x = 0.0;
    double resize_last_y = 0.0;
    double scroll_y = 0.0;
    GLFWcursor* cursor_arrow = nullptr;
    GLFWcursor* cursor_resize = nullptr;

    // Input queues filled by GLFW callbacks and drained by feed_ui_input.
    static constexpr int kQueueCap = 64;
    static constexpr int kMaxDrops = 8;
    static constexpr int kDropPathLen = 512;
    KeyEvent key_queue[kQueueCap] = {};
    int key_count = 0;
    uint32_t char_queue[kQueueCap] = {};
    int char_count = 0;
    char drop_buf[kMaxDrops][kDropPathLen] = {};
    const char* drop_ptrs[kMaxDrops] = {};
    int drop_count = 0;
};

namespace {

void on_scroll(GLFWwindow* handle, double /*xoffset*/, double yoffset) {
    auto* w = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (w) w->scroll_y += yoffset;
}

void on_key(GLFWwindow* handle, int key, int /*scancode*/, int action, int mods) {
    auto* w = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (w && w->key_count < Window::kQueueCap) {
        w->key_queue[w->key_count++] = KeyEvent{key, action, mods};
    }
}

void on_char(GLFWwindow* handle, unsigned int codepoint) {
    auto* w = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (w && w->char_count < Window::kQueueCap) {
        w->char_queue[w->char_count++] = codepoint;
    }
}

void on_drop(GLFWwindow* handle, int count, const char** paths) {
    auto* w = static_cast<Window*>(glfwGetWindowUserPointer(handle));
    if (!w) return;
    for (int i = 0; i < count && w->drop_count < Window::kMaxDrops; ++i) {
        std::snprintf(w->drop_buf[w->drop_count], Window::kDropPathLen, "%s", paths[i]);
        w->drop_ptrs[w->drop_count] = w->drop_buf[w->drop_count];
        ++w->drop_count;
    }
}

int to_glfw_key(Key key) {
    switch (key) {
        case Key::Escape: return GLFW_KEY_ESCAPE;
        case Key::F2: return GLFW_KEY_F2;
        case Key::F3: return GLFW_KEY_F3;
        case Key::Space: return GLFW_KEY_SPACE;
        case Key::Enter: return GLFW_KEY_ENTER;
    }
    return GLFW_KEY_UNKNOWN;
}

} // namespace

Window* create(const WindowDesc& desc) {
#if defined(__linux__)
    // X11 (XWayland) is used by default because it supports window positioning
    // and reliable screenshots. Unset DISPLAY to let GLFW pick Wayland.
    if (std::getenv("DISPLAY")) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
#endif
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return nullptr;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_DECORATED, desc.undecorated ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, desc.transparent ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, desc.floating ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* handle = glfwCreateWindow(desc.width, desc.height, desc.title, nullptr, nullptr);
    if (!handle) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return nullptr;
    }

    // Keep the UI reachable when the window is resized small: the demo layout
    // has a sensible minimum around half the reference design size.
    glfwSetWindowSizeLimits(handle, desc.min_width > 0 ? desc.min_width : 480,
                            desc.min_height > 0 ? desc.min_height : 360,
                            GLFW_DONT_CARE, GLFW_DONT_CARE);

    if (desc.center) {
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
            if (const GLFWvidmode* mode = glfwGetVideoMode(monitor)) {
                const int x = (mode->width - desc.width) / 2;
                const int y = (mode->height - desc.height) / 2;
                glfwSetWindowPos(handle, x, y);
            }
        }
    }

    glfwMakeContextCurrent(handle);
    glfwSwapInterval(1);

    Window* w = new Window();
    w->handle = handle;
    w->cursor_arrow = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
    w->cursor_resize = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
    glfwSetWindowUserPointer(handle, w);
    glfwSetScrollCallback(handle, on_scroll);
    glfwSetKeyCallback(handle, on_key);
    glfwSetCharCallback(handle, on_char);
    glfwSetDropCallback(handle, on_drop);
    return w;
}

void destroy(Window* window) {
    if (!window) return;
    if (window->cursor_arrow) glfwDestroyCursor(window->cursor_arrow);
    if (window->cursor_resize) glfwDestroyCursor(window->cursor_resize);
    if (window->handle) {
        glfwDestroyWindow(window->handle);
        window->handle = nullptr;
    }
    delete window;
    glfwTerminate();
}

void poll_events() { glfwPollEvents(); }
bool should_close(Window* window) { return window && glfwWindowShouldClose(window->handle); }
void set_should_close(Window* window) { if (window) glfwSetWindowShouldClose(window->handle, GLFW_TRUE); }
void swap_buffers(Window* window) { if (window) glfwSwapBuffers(window->handle); }
void show(Window* window) { if (window) glfwShowWindow(window->handle); }

void get_framebuffer_size(Window* window, int* w, int* h) {
    *w = 0; *h = 0;
    if (window) glfwGetFramebufferSize(window->handle, w, h);
}

void get_window_size(Window* window, int* w, int* h) {
    *w = 0; *h = 0;
    if (window) glfwGetWindowSize(window->handle, w, h);
}

void get_cursor(Window* window, double* x, double* y) {
    *x = 0.0; *y = 0.0;
    if (window) glfwGetCursorPos(window->handle, x, y);
}

bool mouse_down(Window* window) {
    return window && glfwGetMouseButton(window->handle, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
}

bool mouse_right_down(Window* window) {
    return window && glfwGetMouseButton(window->handle, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
}

double scroll_poll(Window* window) {
    if (!window) return 0.0;
    const double d = window->scroll_y;
    window->scroll_y = 0.0;
    return d;
}

bool key_pressed(Window* window, Key key) {
    if (!window) return false;
    return glfwGetKey(window->handle, to_glfw_key(key)) == GLFW_PRESS;
}

bool key_raw_down(Window* window, int glfw_key) {
    if (!window) return false;
    return glfwGetKey(window->handle, glfw_key) == GLFW_PRESS;
}

int poll_key_events(Window* window, KeyEvent* out, int max) {
    if (!window || !out || max <= 0) return 0;
    const int n = std::min(window->key_count, max);
    for (int i = 0; i < n; ++i) out[i] = window->key_queue[i];
    for (int i = n; i < window->key_count; ++i) window->key_queue[i - n] = window->key_queue[i];
    window->key_count -= n;
    return n;
}

int poll_chars(Window* window, uint32_t* out, int max) {
    if (!window || !out || max <= 0) return 0;
    const int n = std::min(window->char_count, max);
    for (int i = 0; i < n; ++i) out[i] = window->char_queue[i];
    for (int i = n; i < window->char_count; ++i) window->char_queue[i - n] = window->char_queue[i];
    window->char_count -= n;
    return n;
}

int poll_drops(Window* window, const char** out, int max) {
    if (!window || !out || max <= 0) return 0;
    const int n = std::min(window->drop_count, max);
    for (int i = 0; i < n; ++i) out[i] = window->drop_ptrs[i];
    for (int i = n; i < window->drop_count; ++i) {
        window->drop_ptrs[i - n] = window->drop_ptrs[i];
    }
    window->drop_count -= n;
    return n;
}

const char* clipboard_get_text(Window* window) {
    if (!window) return nullptr;
    return glfwGetClipboardString(window->handle);
}

void clipboard_set_text(Window* window, const char* text) {
    if (window) glfwSetClipboardString(window->handle, text ? text : "");
}

void minimize(Window* window) { if (window) glfwIconifyWindow(window->handle); }

void toggle_maximize(Window* window) {
    if (!window) return;
    if (glfwGetWindowAttrib(window->handle, GLFW_MAXIMIZED)) glfwRestoreWindow(window->handle);
    else glfwMaximizeWindow(window->handle);
}

void drag_update(Window* window, bool active) {
    if (!window) return;
    if (!active) { window->dragging = false; return; }

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window->handle, &mx, &my);
    if (!window->dragging) {
        window->dragging = true;
        window->drag_last_x = mx;
        window->drag_last_y = my;
        return;
    }

    const double dx = mx - window->drag_last_x;
    const double dy = my - window->drag_last_y;
    if (dx == 0.0 && dy == 0.0) return;

    const int platform_id = glfwGetPlatform();
    if (platform_id == GLFW_PLATFORM_X11 || platform_id == GLFW_PLATFORM_WIN32) {
        int wx = 0, wy = 0;
        glfwGetWindowPos(window->handle, &wx, &wy);
        glfwSetWindowPos(window->handle, wx + static_cast<int>(dx), wy + static_cast<int>(dy));
        // Moving the window changes the cursor's window-relative position by -d.
        window->drag_last_x = mx - dx;
        window->drag_last_y = my - dy;
    } else {
        window->drag_last_x = mx;
        window->drag_last_y = my;
    }
}

void drag_stop(Window* window) { if (window) window->dragging = false; }

void resize_update(Window* window, bool active) {
    if (!window) return;
    if (!active) {
        window->resizing = false;
        return;
    }

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window->handle, &mx, &my);
    if (!window->resizing) {
        window->resizing = true;
        window->resize_last_x = mx;
        window->resize_last_y = my;
        return;
    }

    const double dx = mx - window->resize_last_x;
    const double dy = my - window->resize_last_y;
    if (dx == 0.0 && dy == 0.0) return;

    int win_w = 0, win_h = 0;
    glfwGetWindowSize(window->handle, &win_w, &win_h);

    const int new_w = std::max(480, win_w + static_cast<int>(dx));
    const int new_h = std::max(360, win_h + static_cast<int>(dy));

    if (new_w != win_w || new_h != win_h) {
        glfwSetWindowSize(window->handle, new_w, new_h);
        window->resize_last_x = mx;
        window->resize_last_y = my;
    }
}

void resize_stop(Window* window) { if (window) window->resizing = false; }

void set_cursor_resize(Window* window, bool resize) {
    if (!window) return;
    glfwSetCursor(window->handle, resize ? window->cursor_resize : window->cursor_arrow);
}

double time_seconds() { return glfwGetTime(); }

const char* platform_name() {
    switch (glfwGetPlatform()) {
        case GLFW_PLATFORM_WIN32: return "Windows";
        case GLFW_PLATFORM_COCOA: return "macOS";
        case GLFW_PLATFORM_WAYLAND: return "Wayland";
        case GLFW_PLATFORM_X11: return "X11";
        default: return "unknown";
    }
}

} // namespace platform
