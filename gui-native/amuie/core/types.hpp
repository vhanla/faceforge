#pragma once
#include <cstdint>
#include "core/input.hpp"

struct Theme;

enum CommandType : uint8_t {
    CMD_DRAW_RECT,
    CMD_DRAW_RECT_ROUNDED,
    CMD_DRAW_RECT_BORDER,
    CMD_DRAW_RECT_INVERSE,  // fills the area outside a rounded rect (corner masks)
    CMD_DRAW_TEXT,
    CMD_SET_CLIP,
    CMD_DRAW_TEXTURE       // additive: registered image/video texture
};

struct Rect { float x, y, w, h; };
struct Color { uint8_t r, g, b, a; };

struct RenderCommand {
    CommandType type;
    Rect bounds;
    Color color;
    Color border_color;
    Color color2;  // gradient end for CMD_DRAW_TEXT (a == 0 -> solid)
    float corner_radius;
    float border_width;
    const char* text;
    float text_size;
    uint8_t text_weight;  // 0 = regular, 1 = semibold
    // CMD_DRAW_TEXTURE only: backend-registered handle (0 = invalid), the
    // normalized source rect, and a multiply tint carried in `color`.
    uint64_t texture_id;
    Rect uv;
    RenderCommand* next;
};

struct UIContext {
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool mouse_left_down = false;
    bool mouse_left_clicked = false;

    const Theme* theme = nullptr;
    float screen_w = 0.0f;   // logical content width (window size / dpi scale)
    float screen_h = 0.0f;   // logical content height

    // Per-axis device scale: how many framebuffer px one logical px occupies.
    // Uniform (sx == sy) when letterboxing; non-uniform for responsive layouts
    // that stretch horizontally while keeping a fixed vertical DPI ratio.
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    float time_seconds = 0.0f;
    float dt = 0.0f;
    float scroll_delta = 0.0f;  // wheel ticks this frame (for scrollable panels)

    RenderCommand* first_command = nullptr;
    RenderCommand* last_command = nullptr;

    // Mouse button extensions:
    bool mouse_left_released = false;
    bool mouse_right_down = false;
    bool mouse_right_clicked = false;
    bool mouse_right_released = false;

    // Interaction IDs (persistent across frames):
    uint64_t active_id = 0;        // widget capturing left mouse down
    uint64_t active_right_id = 0;  // widget capturing right mouse down
    uint64_t hot_id = 0;           // widget under cursor

    // Popups, Combos & Context Menus:
    uint64_t active_popup_id = 0;  // ID of currently open popup / combo / context menu
    Rect active_popup_rect = {};   // bounds of the open popup for click-outside testing
    bool popup_just_opened = false;
    float menu_x = 0.0f;
    float menu_y = 0.0f;

    // Popup rendering layer (queued separately, drawn on top at frame end):
    bool in_popup_layer = false;
    RenderCommand* popup_first_command = nullptr;
    RenderCommand* popup_last_command = nullptr;

    // ID scope stack:
    uint64_t id_stack[16] = {};
    int id_stack_depth = 0;

    // Keyboard/text/clipboard/drag-and-drop state (see core/input.hpp).
    UIInput input;
};
