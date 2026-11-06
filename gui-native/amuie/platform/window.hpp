#pragma once

namespace platform {

struct Window;

struct WindowDesc {
    int width = 1280;
    int height = 800;
    const char* title = "AMUIE";
    bool undecorated = false;
    bool transparent = false;
    bool resizable = true;
    int min_width = 480;   // logical minimum so the UI stays usable on resize
    int min_height = 360;
    bool floating = false;
    bool visible = true;
    bool center = true;
};

enum class Key { Escape, F2, F3, Space, Enter };

Window* create(const WindowDesc& desc);
void destroy(Window* window);

void poll_events();
bool should_close(Window* window);
void set_should_close(Window* window);
void swap_buffers(Window* window);
void show(Window* window);

void get_framebuffer_size(Window* window, int* w, int* h);
void get_window_size(Window* window, int* w, int* h);
void get_cursor(Window* window, double* x, double* y);
bool mouse_down(Window* window);
bool mouse_right_down(Window* window);
// Returns the accumulated scroll-wheel delta since the last call and resets it.
double scroll_poll(Window* window);
bool key_pressed(Window* window, Key key);

void minimize(Window* window);
void toggle_maximize(Window* window);
void drag_update(Window* window, bool active);
void drag_stop(Window* window);
void resize_update(Window* window, bool active);
void resize_stop(Window* window);
void set_cursor_resize(Window* window, bool resize);

double time_seconds();
const char* platform_name();

} // namespace platform
