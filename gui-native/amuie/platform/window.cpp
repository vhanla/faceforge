#include "platform/window.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__) && defined(AMUIE_HAVE_X11)
#define GLFW_EXPOSE_NATIVE_X11
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#endif

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

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

// Backdrop blur, best effort. The window still has to be created with an ARGB
// framebuffer (desc.transparent) and the app has to draw translucent surfaces
// for the blur to be visible.
void activate_acrylic(GLFWwindow* handle) {
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(handle);
    if (!hwnd) return;
    // Windows 11 22H2+: system backdrop type 4 = acrylic.
    int backdrop = 4; // DWMSBT_ACRYLIC
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */,
                                        &backdrop, sizeof(backdrop)))) {
        return;
    }
    // Windows 10 fallback: the undocumented accent policy (acrylic blur behind).
    struct AccentPolicy {
        int state;
        int flags;
        int gradient_color;
        int animation_id;
    };
    struct WindowCompositionAttribData {
        int attribute;
        void* data;
        size_t size;
    };
    using SetWindowCompositionAttributeFn =
        BOOL(WINAPI*)(HWND, WindowCompositionAttribData*);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    auto set_attr = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!set_attr) return;
    AccentPolicy policy{};
    policy.state = 4; // ACCENT_ENABLE_ACRYLICBLURBEHIND
    policy.flags = 2; // draw the gradient over the blurred region
    policy.gradient_color = 0x991C1715; // AABBGGRR (theme bg_app)
    WindowCompositionAttribData data{19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy)};
    set_attr(hwnd, &data);
#elif defined(__linux__) && defined(AMUIE_HAVE_X11)
    // KWin (Plasma) blurs the desktop behind X11 windows that carry the
    // _KDE_NET_WM_BLUR_BEHIND_REGION hint. An empty region means the whole
    // window. Other compositors have no portable equivalent.
    Display* display = glfwGetX11Display();
    const ::Window xid = glfwGetX11Window(handle);
    if (!display || !xid) return;
    const Atom atom = XInternAtom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
    if (atom == None) return;
    // Zero rectangles: blur the whole window. The data pointer is unused.
    const unsigned char empty = 0;
    XChangeProperty(display, xid, atom, XA_CARDINAL, 32, PropModeReplace, &empty, 0);
    XFlush(display);
#else
    (void)handle;
#endif
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

    if (desc.acrylic) activate_acrylic(handle);

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

bool acrylic_available() {
#if defined(_WIN32)
    // DWM acrylic (Win11) with the accent-policy fallback for Windows 10.
    return true;
#elif defined(__linux__) && defined(AMUIE_HAVE_X11)
    // KWin needs an X11 window for _KDE_NET_WM_BLUR_BEHIND_REGION; there is no
    // portable Wayland blur request.
    if (!std::getenv("DISPLAY")) return false;
    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP")) {
        if (std::strstr(desktop, "KDE") || std::strstr(desktop, "Plasma")) return true;
    }
    return std::getenv("KDE_FULL_SESSION") != nullptr;
#else
    return false;
#endif
}

bool framebuffer_transparent(Window* window) {
    if (!window || !window->handle) return false;
    return glfwGetWindowAttrib(window->handle, GLFW_TRANSPARENT_FRAMEBUFFER) != 0;
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

bool mouse_middle_down(Window* window) {
    return window && glfwGetMouseButton(window->handle, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
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

// ---- Native file choosers --------------------------------------------------

namespace {

#if defined(_WIN32)
std::string win_filter(const char* filter) {
    // Comdlg32 wants "Label\0pattern\0...\0\0"; derive a single Images group
    // from the space-separated globs.
    std::string globs = filter && *filter ? filter : "*.*";
    for (char& c : globs) {
        if (c == ' ') c = ';';
    }
    std::string out = "Images (";
    std::string pretty;
    for (char c : globs) pretty.push_back(c == ';' ? ' ' : c);
    out += pretty;
    out += ")\0";
    out += globs;
    out.push_back('\0');
    out += "All files (*.*)\0*.*\0";
    out.push_back('\0');
    return out;
}

std::string win_dialog(bool save, const char* title, const char* filter,
                       const char* initial_path, const char* default_name) {
    char file[MAX_PATH] = {0};
    if (initial_path && *initial_path) {
        std::snprintf(file, sizeof(file), "%s", initial_path);
    } else if (save && default_name && *default_name) {
        std::snprintf(file, sizeof(file), "%s", default_name);
    }
    const std::string filt = win_filter(filter);

    OPENFILENAMEA ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filt.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    const BOOL ok = save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    return ok ? std::string(file) : std::string();
}
#else
std::string shq(const char* s) {
    std::string out = "'";
    for (const char* p = s; p && *p; ++p) {
        if (*p == '\'') out += "'\\''";
        else out.push_back(*p);
    }
    out.push_back('\'');
    return out;
}

bool run_capture(const std::string& cmd, std::string& out) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[1024];
    out.clear();
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    const int rc = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return rc == 0 && !out.empty();
}

bool have_cmd(const char* name) {
    std::string c = "command -v ";
    c += name;
    c += " >/dev/null 2>&1";
    return std::system(c.c_str()) == 0;
}

std::string kdialog_filter(const char* filter) {
    // kdialog: "Images (*.png *.jpg)"
    std::string globs = filter && *filter ? filter : "*";
    return "Images (" + globs + ")";
}

std::string unix_dialog(bool save, const char* title, const char* filter,
                        const char* initial_path, const char* default_name) {
    std::string start = initial_path && *initial_path ? initial_path
                        : (default_name ? default_name : "");
    std::string out;
    if (have_cmd("zenity")) {
        std::string cmd = "zenity --file-selection";
        if (save) cmd += " --save --confirm-overwrite";
        if (title && *title) cmd += " --title=" + shq(title);
        if (!start.empty()) cmd += " --filename=" + shq(start.c_str());
        if (filter && *filter) {
            cmd += " --file-filter=" + shq((std::string("Images | ") + filter).c_str());
        }
        if (run_capture(cmd, out)) return out;
        return {};
    }
    if (have_cmd("kdialog")) {
        std::string cmd = "kdialog ";
        cmd += save ? "--getsavefilename " : "--getopenfilename ";
        cmd += shq(start.empty() ? "." : start.c_str());
        cmd += " " + shq(kdialog_filter(filter).c_str());
        if (title && *title) cmd += " --title " + shq(title);
        if (run_capture(cmd, out)) return out;
    }
    return {};
}
#endif

} // namespace

std::string open_file_dialog(const char* title, const char* filter,
                             const char* initial_path) {
#if defined(_WIN32)
    return win_dialog(false, title, filter, initial_path, nullptr);
#else
    return unix_dialog(false, title, filter, initial_path, nullptr);
#endif
}

std::string save_file_dialog(const char* title, const char* filter,
                             const char* initial_path, const char* default_name) {
#if defined(_WIN32)
    return win_dialog(true, title, filter, initial_path, default_name);
#else
    return unix_dialog(true, title, filter, initial_path, default_name);
#endif
}

// ---- Image clipboard -------------------------------------------------------
// GLFW only handles text; image copy/paste goes through the desktop tools on
// Linux (xclip on X11, wl-copy/wl-paste on Wayland) using a temporary PNG file.
// Other platforms report failure so the app can show a hint.

bool clipboard_set_image_png(Window* /*window*/, const uint8_t* png, size_t size) {
#if defined(__linux__)
    if (!png || size == 0) return false;
    char tmpl[] = "/tmp/faceforge-clip-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < size) {
        const ssize_t n = write(fd, png + off, size - off);
        if (n <= 0) { close(fd); unlink(tmpl); return false; }
        off += static_cast<size_t>(n);
    }
    close(fd);
    bool ok = false;
    if (std::getenv("WAYLAND_DISPLAY") && have_cmd("wl-copy")) {
        ok = std::system((std::string("wl-copy --type image/png < ") + shq(tmpl)).c_str()) == 0;
    } else if (have_cmd("xclip")) {
        ok = std::system((std::string("xclip -selection clipboard -t image/png -i ") + shq(tmpl)).c_str()) == 0;
    }
    unlink(tmpl);
    return ok;
#else
    (void)png;
    (void)size;
    return false;
#endif
}

bool clipboard_get_image_png(Window* /*window*/, std::vector<uint8_t>& out) {
#if defined(__linux__)
    out.clear();
    char tmpl[] = "/tmp/faceforge-paste-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd < 0) return false;
    close(fd);
    bool ok = false;
    const std::string redir = " > " + shq(tmpl) + " 2>/dev/null";
    if (std::getenv("WAYLAND_DISPLAY") && have_cmd("wl-paste")) {
        ok = std::system(("wl-paste --type image/png --no-newline" + redir).c_str()) == 0;
    } else if (have_cmd("xclip")) {
        ok = std::system(("xclip -selection clipboard -t image/png -o" + redir).c_str()) == 0;
    }
    if (ok) {
        FILE* f = std::fopen(tmpl, "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0) {
                out.resize(static_cast<size_t>(n));
                const size_t got = std::fread(out.data(), 1, out.size(), f);
                out.resize(got);
            }
            std::fclose(f);
        }
    }
    unlink(tmpl);
    return ok && !out.empty();
#else
    (void)out;
    return false;
#endif
}

// ---- Drag-out (X11 XDND source) -------------------------------------------
// Dragging out of the canvas with GLFW's event pump is awkward, so the drag
// runs on a *separate* X11 connection and a hidden source window: it owns
// XdndSelection, offers the result as a file (text/uri-list) and blocks until
// the button is released. File managers and browsers accept that type, so the
// result can be dropped without saving it first. Windows and other backends
// return false (the app falls back to the clipboard).

bool drag_source_file(Window* window, const uint8_t* png, size_t size) {
#if defined(__linux__) && defined(AMUIE_HAVE_X11)
    if (!window || !window->handle || !png || size == 0) return false;
    // Materialise the PNG so the drag can offer it as a real file (text/uri-list).
    char tmpl[] = "/tmp/faceforge-drag-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < size) {
        const ssize_t n = write(fd, png + off, size - off);
        if (n <= 0) { close(fd); unlink(tmpl); return false; }
        off += static_cast<size_t>(n);
    }
    close(fd);
    const std::string path = tmpl;

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { unlink(tmpl); return false; }
    // Xlib aborts the process on BadWindow, which is easy to hit here because
    // drag targets can vanish mid-drag. Ignore X errors for the drag and
    // restore the previous handler afterwards.
    XErrorHandler old_handler = XSetErrorHandler(+[](Display*, XErrorEvent*) { return 0; });
    const int screen = DefaultScreen(dpy);
    const ::Window root = RootWindow(dpy, screen);
    const ::Window src = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    // GLFW marks the app window XdndAware to accept drops. Never target it:
    // GLFW would answer asynchronously after this source window is gone.
    const ::Window self = glfwGetX11Window(window->handle);

    const Atom a_aware = XInternAtom(dpy, "XdndAware", False);
    const Atom a_sel = XInternAtom(dpy, "XdndSelection", False);
    const Atom a_enter = XInternAtom(dpy, "XdndEnter", False);
    const Atom a_pos = XInternAtom(dpy, "XdndPosition", False);
    const Atom a_status = XInternAtom(dpy, "XdndStatus", False);
    const Atom a_leave = XInternAtom(dpy, "XdndLeave", False);
    const Atom a_drop = XInternAtom(dpy, "XdndDrop", False);
    const Atom a_finished = XInternAtom(dpy, "XdndFinished", False);
    const Atom a_action = XInternAtom(dpy, "XdndActionCopy", False);
    const Atom a_urilist = XInternAtom(dpy, "text/uri-list", False);
    const Atom a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);

    std::string uri = "file://";
    uri += path;
    uri += "\r\n";

    XSetSelectionOwner(dpy, a_sel, src, CurrentTime);
    XFlush(dpy);

    auto has_aware = [&](::Window w) -> bool {
        Atom type = None;
        int fmt = 0;
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        const bool ok = XGetWindowProperty(dpy, w, a_aware, 0, 1, False, AnyPropertyType,
                                           &type, &fmt, &n, &after, &data) == Success &&
                        data != nullptr;
        if (data) XFree(data);
        return ok;
    };

    // Deepest window under the pointer, plus root coordinates and button mask.
    auto pick = [&](int* rx, int* ry, unsigned int* mask) -> ::Window {
        ::Window cur = root;
        ::Window child = None;
        int x = 0, y = 0, wx = 0, wy = 0;
        unsigned int m = 0;
        for (;;) {
            ::Window rr = None;
            if (!XQueryPointer(dpy, cur, &rr, &child, &x, &y, &wx, &wy, &m)) return None;
            *rx = x;
            *ry = y;
            *mask = m;
            if (child == None) break;
            cur = child;
        }
        return cur;
    };

    auto send = [&](::Window target, Atom type, long l0, long l1, long l2, long l3, long l4) {
        XEvent ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.display = dpy;
        ev.xclient.window = target;
        ev.xclient.message_type = type;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = l0;
        ev.xclient.data.l[1] = l1;
        ev.xclient.data.l[2] = l2;
        ev.xclient.data.l[3] = l3;
        ev.xclient.data.l[4] = l4;
        XSendEvent(dpy, target, False, NoEventMask, &ev);
        XFlush(dpy);
    };

    bool accepted = false;
    bool finished = false;
    auto pump = [&]() {
        while (XPending(dpy) > 0) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == ClientMessage && e.xclient.message_type == a_status &&
                e.xclient.data.l[0] == static_cast<long>(src)) {
                accepted = (e.xclient.data.l[1] & 1) != 0;
            } else if (e.type == ClientMessage && e.xclient.message_type == a_finished &&
                       e.xclient.data.l[0] == static_cast<long>(src)) {
                finished = true;
            } else if (e.type == SelectionRequest) {
                XSelectionRequestEvent* r = &e.xselectionrequest;
                XSelectionEvent se;
                std::memset(&se, 0, sizeof(se));
                se.type = SelectionNotify;
                se.display = dpy;
                se.requestor = r->requestor;
                se.selection = r->selection;
                se.target = r->target;
                se.time = r->time;
                se.property = None;
                if (r->target == a_urilist || r->target == a_utf8 || r->target == XA_STRING) {
                    const Atom prop = r->property != None ? r->property : a_urilist;
                    XChangeProperty(dpy, r->requestor, prop, r->target, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(uri.c_str()),
                                    static_cast<int>(uri.size()));
                    se.property = prop;
                }
                XSendEvent(dpy, r->requestor, False, 0, reinterpret_cast<XEvent*>(&se));
                XFlush(dpy);
            }
        }
    };

    ::Window target = None;
    int last_x = -1, last_y = -1;
    for (int iter = 0; iter < 60000; ++iter) { // safety cap ~15 min
        int rx = 0, ry = 0;
        unsigned int mask = 0;
        ::Window cur = pick(&rx, &ry, &mask);
        ::Window cand = None;
        if (cur != None) {
            for (::Window w = cur; w != None && w != root; ) {
                if (has_aware(w)) { cand = w; break; }
                ::Window rr = None, parent = None;
                ::Window* kids = nullptr;
                unsigned int nkids = 0;
                if (!XQueryTree(dpy, w, &rr, &parent, &kids, &nkids)) break;
                if (kids) XFree(kids);
                w = parent;
            }
        }
        if (cand == self) cand = None;
        const long pos = ((rx & 0xFFFF) << 16) | (ry & 0xFFFF);
        if (cand != target) {
            if (target != None) send(target, a_leave, static_cast<long>(src), 0, 0, 0, 0);
            target = cand;
            accepted = false;
            if (target != None) {
                send(target, a_enter, static_cast<long>(src), (5L << 24), static_cast<long>(a_urilist), 0, 0);
                send(target, a_pos, static_cast<long>(src), 0, pos, CurrentTime, static_cast<long>(a_action));
            }
        } else if (target != None && (rx != last_x || ry != last_y)) {
            send(target, a_pos, static_cast<long>(src), 0, pos, CurrentTime, static_cast<long>(a_action));
        }
        last_x = rx;
        last_y = ry;
        pump();

        if ((mask & Button1Mask) == 0) break;
        usleep(12000);
    }

    bool dropped = false;
    if (target != None && accepted) {
        send(target, a_drop, static_cast<long>(src), 0, CurrentTime, 0, 0);
        for (int i = 0; i < 200 && !finished; ++i) { // up to ~2 s to serve the selection
            pump();
            usleep(10000);
        }
        dropped = true;
    } else if (target != None) {
        send(target, a_leave, static_cast<long>(src), 0, 0, 0, 0);
    }

    XDestroyWindow(dpy, src);
    // Drain any pending X errors (BadWindow from targets that vanished mid-drag)
    // while the ignore handler is still installed.
    XSync(dpy, False);
    XCloseDisplay(dpy);
    XSetErrorHandler(old_handler);
    // The target has acknowledged the transfer by now, so the temp file is safe
    // to remove.
    unlink(tmpl);
    return dropped;
#else
    (void)window;
    (void)png;
    (void)size;
    return false;
#endif
}

} // namespace platform