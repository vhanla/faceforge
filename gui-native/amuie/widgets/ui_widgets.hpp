#pragma once
#include "core/arena.hpp"
#include "core/theme.hpp"
#include "core/types.hpp"

#include <cstddef>

struct LayoutContext {
    Rect bounds;
    float current_y;
    float padding;
};

struct ComboStyle {
    Color bg_normal;
    Color bg_hover;
    Color text_color;
    float radius;
};

// Master API signatures:
void push_cmd(UIContext* ctx, RenderCommand* cmd);

bool ui_button_raw(Arena* arena, UIContext* ctx, Rect rect, const char* label,
                   Color color, float radius);
void ui_label_raw(Arena* arena, UIContext* ctx, Rect rect, const char* text, Color color);
bool ui_combo_selector(Arena* arena, UIContext* ctx, LayoutContext* layout, float height,
                       const char* title, const char* current_selection, ComboStyle style);

// ---- Interaction & ID system ---------------------------------------------
constexpr uint64_t UI_FNV1A_OFFSET = 0xcbf29ce484222325ULL;
constexpr uint64_t UI_FNV1A_PRIME = 0x100000001b3ULL;
constexpr uint64_t UI_UNCLAIMED_ID = 0xFFFFFFFFFFFFFFFFULL;

inline uint64_t ui_hash_str(const char* str, uint64_t seed = UI_FNV1A_OFFSET) {
    if (!str) return seed;
    uint64_t h = seed;
    while (*str) {
        h ^= static_cast<uint8_t>(*str++);
        h *= UI_FNV1A_PRIME;
    }
    return h;
}

inline uint64_t ui_hash_int(uint64_t val, uint64_t seed = UI_FNV1A_OFFSET) {
    uint64_t h = seed;
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<uint8_t>(val & 0xFF);
        h *= UI_FNV1A_PRIME;
        val >>= 8;
    }
    return h;
}

void ui_push_id(UIContext* ctx, uint64_t id);
void ui_push_id_str(UIContext* ctx, const char* str);
void ui_pop_id(UIContext* ctx);
uint64_t ui_get_id(UIContext* ctx, const char* label, uint64_t extra = 0);
uint64_t ui_get_id_rect(UIContext* ctx, Rect r, uint64_t extra = 0);

enum class UIClickTrigger : uint8_t {
    OnRelease,         // Standard button: mouse down inside AND release inside
    OnDown,            // Immediate: mouse down inside triggers immediately
    OnReleaseAnywhere, // Drag drop / commit: mouse down inside, release anywhere
};

struct UIBehavior {
    UIClickTrigger trigger = UIClickTrigger::OnRelease;
    bool draggable = false; // keeps active while dragging even outside rect
};

struct UIInteraction {
    bool hovered = false;
    bool pressed = false;  // active && hovered && mouse_left_down
    bool clicked = false;  // triggered click this frame according to trigger rule
    bool down = false;     // mouse down just started inside this widget this frame
    bool released = false; // mouse release occurred inside this widget (was active)
    bool dragging = false; // mouse held down while this widget is active
};

UIInteraction ui_process_interaction(UIContext* ctx, uint64_t id, Rect r, UIBehavior behavior = {});
bool ui_right_clicked(UIContext* ctx, uint64_t id, Rect r);

// Finalizes the UI pass, appending deferred popup/overlay layers and managing interaction state
void ui_end_frame(Arena* arena, UIContext* ctx);

// ---- reusable primitives / components ------------------------------------
RenderCommand* ui_alloc_cmd(Arena* arena);

// Sets the active clip rect. Single rect, reset by the backend each frame;
// draw each scrollable region contiguously and reset with the full-screen rect.
void ui_clip(Arena* arena, UIContext* ctx, Rect r);

bool ui_hovered(const UIContext* ctx, Rect r);

void ui_rect(Arena* arena, UIContext* ctx, Rect r, Color c, float radius = 0.0f);
// Fills the part of `r` lying outside a rounded outline of `radius`. Used to
// punch the transparent/desktop-colored corners of a shaped window.
void ui_rect_rounded_inverse(Arena* arena, UIContext* ctx, Rect r, Color c,
                             float radius);
void ui_rect_border(Arena* arena, UIContext* ctx, Rect r, Color c,
                    float radius, float width);
void ui_panel(Arena* arena, UIContext* ctx, Rect r, Color bg, Color border,
              float radius);
void ui_separator(Arena* arena, UIContext* ctx, float x, float y, float w, Color c);
void ui_dashed_rect(Arena* arena, UIContext* ctx, Rect r, Color c,
                    float dash = 5.0f, float gap = 4.0f);

void ui_text(Arena* arena, UIContext* ctx, float x, float y, const char* text,
             Color color, float size, uint8_t weight = 0);
// Approximates the semibold face by double-stroking the regular atlas. Keeps
// the single-atlas/single-weight asset pipeline while matching the reference.
void ui_text_bold(Arena* arena, UIContext* ctx, float x, float y, const char* text,
                  Color color, float size);
void ui_text_in_rect(Arena* arena, UIContext* ctx, Rect r, const char* text,
                     Color color, float size, uint8_t weight = 0);
void ui_text_center(Arena* arena, UIContext* ctx, Rect r, const char* text,
                    Color color, float size, uint8_t weight = 0);
void ui_text_right(Arena* arena, UIContext* ctx, Rect r, const char* text,
                   Color color, float size, uint8_t weight = 0);
void ui_icon(Arena* arena, UIContext* ctx, float x, float y, uint32_t codepoint,
             Color color, float size);
// Horizontal tint gradient, left -> right. Used for the brand mark.
void ui_icon_gradient(Arena* arena, UIContext* ctx, float x, float y,
                      uint32_t codepoint, Color left, Color right, float size);
void ui_icon_center(Arena* arena, UIContext* ctx, Rect r, uint32_t codepoint,
                    Color color, float size);

// Draws a registered texture (see IRendererBackend::create_texture_bgr) scaled
// into `dst`, multiplied by `tint`. `texture_id == 0` draws nothing. The source
// quad is the full image; pass a non-identity `uv` for sub-rect blits (video
// thumbnails, atlas regions).
void ui_image(Arena* arena, UIContext* ctx, Rect dst, uint64_t texture_id,
              Color tint = Color{0xff, 0xff, 0xff, 0xff});
void ui_image_uv(Arena* arena, UIContext* ctx, Rect dst, uint64_t texture_id,
                 Rect uv, Color tint = Color{0xff, 0xff, 0xff, 0xff});

bool ui_icon_button(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                    Color icon_color, Color hover_bg, float radius,
                    float icon_size = 16.0f);
bool ui_icon_button_ex(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                       Color icon_color, Color hover_bg, float radius,
                       float icon_size = 16.0f,
                       UIBehavior behavior = {}, uint64_t id = 0);

bool ui_button_icon_text(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                         const char* label, Color bg, float radius,
                         float icon_size = 16.0f, float text_size = 14.0f,
                         bool bold = false, Color text_color = Color{0, 0, 0, 0},
                         Color icon_color = Color{0, 0, 0, 0},
                         float text_gap = 10.0f);
bool ui_button_icon_text_ex(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                            const char* label, Color bg, float radius,
                            float icon_size = 16.0f, float text_size = 14.0f,
                            bool bold = false, Color text_color = Color{0, 0, 0, 0},
                            Color icon_color = Color{0, 0, 0, 0},
                            float text_gap = 10.0f,
                            UIBehavior behavior = {}, uint64_t id = 0);

// ---- Functional Comboboxes -----------------------------------------------
bool ui_combo(Arena* arena, UIContext* ctx, Rect rect,
              const char* const* items, int item_count,
              int* selected_index, const Theme* theme,
              uint64_t id = 0);

bool ui_combo_selector_items(Arena* arena, UIContext* ctx, LayoutContext* layout, float height,
                             const char* title, const char* const* items, int item_count,
                             int* selected_index, ComboStyle style,
                             uint64_t id = 0);

// ---- Context Menus -------------------------------------------------------
struct MenuItem {
    const char* label = nullptr;
    uint32_t icon = 0;
    const char* shortcut = nullptr;
    bool disabled = false;
    bool separator = false;
};

// Checks if right-click occurred in area. If so, opens menu_id at cursor position.
bool ui_context_menu_area(UIContext* ctx, uint64_t menu_id, Rect area);

// Programmatically open context menu at specific coordinates
void ui_open_context_menu(UIContext* ctx, uint64_t menu_id, float x, float y);

// Renders context menu if menu_id is currently open.
// Returns clicked item index (0..item_count-1), or -1 if nothing clicked this frame.
int ui_context_menu(Arena* arena, UIContext* ctx, uint64_t menu_id,
                    const MenuItem* items, int item_count,
                    const Theme* theme);
bool ui_checkbox(Arena* arena, UIContext* ctx, float x, float y,
                 const char* label, bool* checked, const Theme* theme);
bool ui_toggle(Arena* arena, UIContext* ctx, Rect r, bool* value,
               const Theme* theme);
bool ui_text_input(Arena* arena, UIContext* ctx, Rect r, const char* text,
                   Color bg, Color border, const Theme* theme);

// ---- Editable text & slider ----------------------------------------------
enum class UITextEditResult : uint8_t { None, Changed, Submitted };

// Editable single-line text field. `buf`/`cap` are app-owned; the widget edits
// in place and returns Changed when the text changes, Submitted on Enter.
// Focus, caret, selection and horizontal scroll persist in ctx->input, keyed by
// `id`. Pass a stable id when several fields share the same rect. `theme`
// defaults to ctx->theme.
UITextEditResult ui_text_edit(Arena* arena, UIContext* ctx, Rect r, char* buf, size_t cap,
                              const Theme* theme, const char* placeholder = nullptr,
                              uint64_t id = 0);

// Horizontal slider over [min,max]. Updates *value while pressed/dragged and
// returns true on the frames where it changed.
bool ui_slider(Arena* arena, UIContext* ctx, Rect r, float* value, float min, float max,
               const Theme* theme, uint64_t id = 0);
void ui_progress(Arena* arena, UIContext* ctx, Rect r, float fraction,
                 Color bg, Color fill);
void ui_stat_row(Arena* arena, UIContext* ctx, Rect r, const char* label,
                 const char* value, const Theme* theme);

// Returns a frame-arena copy of `text` trimmed to fit `max_w` logical px, with a
// trailing ellipsis. Returns `text` unchanged when it already fits. Single-line
// only; used by fields and status readouts so long values never overflow.
const char* ui_text_ellipsized(Arena* arena, const char* text, float size,
                               uint8_t weight, float max_w);

Color ui_hover_color(Color base, bool hovered, bool pressed);
Color ui_contrast_text(Color bg);
