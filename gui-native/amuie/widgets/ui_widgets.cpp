#include "widgets/ui_widgets.hpp"
#include "core/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

RenderCommand* ui_alloc_cmd(Arena* arena) {
    auto* cmd = static_cast<RenderCommand*>(arena->alloc(sizeof(RenderCommand)));
    if (!cmd) return nullptr;
    *cmd = RenderCommand{};
    return cmd;
}

void push_cmd(UIContext* ctx, RenderCommand* cmd) {
    if (!cmd || !ctx) return;
    cmd->next = nullptr;
    if (ctx->in_popup_layer) {
        if (!ctx->popup_first_command) {
            ctx->popup_first_command = cmd;
            ctx->popup_last_command = cmd;
        } else {
            ctx->popup_last_command->next = cmd;
            ctx->popup_last_command = cmd;
        }
        return;
    }
    if (!ctx->first_command) {
        ctx->first_command = cmd;
        ctx->last_command = cmd;
    } else {
        ctx->last_command->next = cmd;
        ctx->last_command = cmd;
    }
}

void ui_push_id(UIContext* ctx, uint64_t id) {
    if (!ctx) return;
    if (ctx->id_stack_depth < 16) {
        ctx->id_stack[ctx->id_stack_depth++] = id;
    }
}

void ui_push_id_str(UIContext* ctx, const char* str) {
    if (!ctx) return;
    uint64_t seed = ctx->id_stack_depth > 0 ? ctx->id_stack[ctx->id_stack_depth - 1] : UI_FNV1A_OFFSET;
    ui_push_id(ctx, ui_hash_str(str, seed));
}

void ui_pop_id(UIContext* ctx) {
    if (ctx && ctx->id_stack_depth > 0) {
        --ctx->id_stack_depth;
    }
}

uint64_t ui_get_id(UIContext* ctx, const char* label, uint64_t extra) {
    uint64_t seed = (ctx && ctx->id_stack_depth > 0) ? ctx->id_stack[ctx->id_stack_depth - 1] : UI_FNV1A_OFFSET;
    uint64_t h = seed;
    if (label && *label) {
        h = ui_hash_str(label, h);
    }
    if (extra != 0 || (!label || !*label)) {
        h = ui_hash_int(extra, h);
    }
    return h;
}

uint64_t ui_get_id_rect(UIContext* ctx, Rect r, uint64_t extra) {
    const uint64_t rx = static_cast<uint64_t>(std::round(r.x * 10.0f));
    const uint64_t ry = static_cast<uint64_t>(std::round(r.y * 10.0f));
    const uint64_t pos_id = (rx << 32) ^ ry ^ extra;
    return ui_get_id(ctx, nullptr, pos_id);
}

UIInteraction ui_process_interaction(UIContext* ctx, uint64_t id, Rect r, UIBehavior behavior) {
    UIInteraction res{};
    if (!ctx) return res;

    // If an active popup exists and this widget is not inside the popup's rect,
    // block normal hover and clicks (modal dismissal behavior).
    if (ctx->active_popup_id != 0 && id != ctx->active_popup_id) {
        const bool inside_popup = (r.x >= ctx->active_popup_rect.x - 1.0f &&
                                   r.x + r.w <= ctx->active_popup_rect.x + ctx->active_popup_rect.w + 1.0f &&
                                   r.y >= ctx->active_popup_rect.y - 1.0f &&
                                   r.y + r.h <= ctx->active_popup_rect.y + ctx->active_popup_rect.h + 1.0f);
        if (!inside_popup) {
            return res;
        }
    }

    const bool cursor_inside = ui_hovered(ctx, r);

    // Hover is true only if cursor is inside and either no widget captured mouse or this widget captured it
    if (cursor_inside && (ctx->active_id == 0 || ctx->active_id == id)) {
        res.hovered = true;
        ctx->hot_id = id;
    }

    // Mouse DOWN: only capture and register down if click started INSIDE this widget and no other widget captured
    if (ctx->mouse_left_clicked && cursor_inside && ctx->active_id == 0) {
        ctx->active_id = id;
        res.down = true;
        if (behavior.trigger == UIClickTrigger::OnDown) {
            res.clicked = true;
        }
    }

    // While mouse is held DOWN (this widget must be the one that captured):
    if (ctx->active_id == id) {
        if (ctx->mouse_left_down) {
            res.dragging = true;
            if (cursor_inside || behavior.draggable) {
                res.pressed = cursor_inside;
            }
        }

        // Mouse RELEASE:
        if (ctx->mouse_left_released) {
            if (cursor_inside || behavior.trigger == UIClickTrigger::OnReleaseAnywhere) {
                res.released = true;
                if (behavior.trigger == UIClickTrigger::OnRelease ||
                    behavior.trigger == UIClickTrigger::OnReleaseAnywhere) {
                    res.clicked = true;
                }
            }
            ctx->active_id = 0; // release capture
        }
    }

    return res;
}

bool ui_right_clicked(UIContext* ctx, uint64_t id, Rect r) {
    if (!ctx) return false;
    const bool cursor_inside = ui_hovered(ctx, r);
    if (ctx->mouse_right_clicked && cursor_inside && ctx->active_right_id == 0) {
        ctx->active_right_id = id;
    }
    if (ctx->active_right_id == id && ctx->mouse_right_released) {
        ctx->active_right_id = 0;
        if (cursor_inside) return true;
    }
    return false;
}

void ui_end_frame(Arena* arena, UIContext* ctx) {
    if (!ctx) return;

    if (ctx->popup_first_command) {
        // Reset clip rect so popups are never clipped by parent scroll panels or containers
        RenderCommand* clip_cmd = ui_alloc_cmd(arena);
        if (clip_cmd) {
            clip_cmd->type = CMD_SET_CLIP;
            clip_cmd->bounds = Rect{0.0f, 0.0f, ctx->screen_w, ctx->screen_h};
            clip_cmd->next = nullptr;
            if (!ctx->first_command) {
                ctx->first_command = clip_cmd;
                ctx->last_command = clip_cmd;
            } else {
                ctx->last_command->next = clip_cmd;
                ctx->last_command = clip_cmd;
            }
        }

        // Append all popup commands to the end of the frame
        if (!ctx->first_command) {
            ctx->first_command = ctx->popup_first_command;
            ctx->last_command = ctx->popup_last_command;
        } else {
            ctx->last_command->next = ctx->popup_first_command;
            ctx->last_command = ctx->popup_last_command;
        }
        ctx->popup_first_command = nullptr;
        ctx->popup_last_command = nullptr;
    }

    // Reset interaction IDs if mouse released or idle. On the release frame the
    // press/drag cycle ends, so also clear the "unclaimed" sentinel set when the
    // click began on empty space (otherwise it lingers one extra frame).
    if (!ctx->mouse_left_down && !ctx->mouse_left_released) {
        ctx->active_id = 0;
    }
    if (ctx->mouse_left_released) {
        ctx->active_id = 0;
    }
    if (!ctx->mouse_right_down && !ctx->mouse_right_released) {
        ctx->active_right_id = 0;
    }
    if (ctx->mouse_right_released) {
        ctx->active_right_id = 0;
    }

    // If mouse was clicked on empty area and no widget claimed active_id this frame,
    // mark active_id as unclaimed sentinel so dragging over other widgets doesn't activate them
    if (ctx->mouse_left_clicked && ctx->active_id == 0) {
        ctx->active_id = UI_UNCLAIMED_ID;
    }
    if (ctx->mouse_right_clicked && ctx->active_right_id == 0) {
        ctx->active_right_id = UI_UNCLAIMED_ID;
    }
}

void ui_clip(Arena* arena, UIContext* ctx, Rect r) {
    RenderCommand* cmd = ui_alloc_cmd(arena);
    if (!cmd) return;
    cmd->type = CMD_SET_CLIP;
    cmd->bounds = r;
    push_cmd(ctx, cmd);
}

bool ui_hovered(const UIContext* ctx, Rect r) {
    return ctx->mouse_x >= r.x && ctx->mouse_x <= r.x + r.w &&
           ctx->mouse_y >= r.y && ctx->mouse_y <= r.y + r.h;
}

Color ui_hover_color(Color base, bool hovered, bool pressed) {
    if (pressed) return color_darken(base, 18);
    if (hovered) return color_lighten(base, 14);
    return base;
}

Color ui_contrast_text(Color bg) {
    const int lum = (static_cast<int>(bg.r) * 299 + static_cast<int>(bg.g) * 587 +
                     static_cast<int>(bg.b) * 114) / 1000;
    return lum > 150 ? Color{0x18, 0x1b, 0x21, 0xff} : Color{0xff, 0xff, 0xff, 0xff};
}

void ui_rect(Arena* arena, UIContext* ctx, Rect r, Color c, float radius) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    RenderCommand* cmd = ui_alloc_cmd(arena);
    if (!cmd) return;
    cmd->type = radius > 0.0f ? CMD_DRAW_RECT_ROUNDED : CMD_DRAW_RECT;
    cmd->bounds = r;
    cmd->color = c;
    cmd->corner_radius = radius;
    push_cmd(ctx, cmd);
}

void ui_rect_rounded_inverse(Arena* arena, UIContext* ctx, Rect r, Color c,
                             float radius) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    RenderCommand* cmd = ui_alloc_cmd(arena);
    if (!cmd) return;
    cmd->type = CMD_DRAW_RECT_INVERSE;
    cmd->bounds = r;
    cmd->color = c;
    cmd->corner_radius = radius;
    push_cmd(ctx, cmd);
}

void ui_rect_border(Arena* arena, UIContext* ctx, Rect r, Color c,
                    float radius, float width) {    if (r.w <= 0.0f || r.h <= 0.0f) return;
    RenderCommand* cmd = ui_alloc_cmd(arena);
    if (!cmd) return;
    cmd->type = CMD_DRAW_RECT_BORDER;
    cmd->bounds = r;
    cmd->color = Color{0, 0, 0, 0};
    cmd->border_color = c;
    cmd->corner_radius = radius;
    cmd->border_width = width;
    push_cmd(ctx, cmd);
}

void ui_panel(Arena* arena, UIContext* ctx, Rect r, Color bg, Color border,
              float radius) {
    ui_rect(arena, ctx, r, bg, radius);
    if (border.a > 0) ui_rect_border(arena, ctx, r, border, radius, 1.0f);
}

void ui_separator(Arena* arena, UIContext* ctx, float x, float y, float w, Color c) {
    ui_rect(arena, ctx, Rect{x, y, w, 1.0f}, c);
}

void ui_dashed_rect(Arena* arena, UIContext* ctx, Rect r, Color c,
                    float dash, float gap) {
    const float step = dash + gap;
    for (float x = r.x; x < r.x + r.w; x += step) {
        const float w = std::min(dash, r.x + r.w - x);
        ui_rect(arena, ctx, Rect{x, r.y, w, 1.0f}, c);
        ui_rect(arena, ctx, Rect{x, r.y + r.h - 1.0f, w, 1.0f}, c);
    }
    for (float y = r.y; y < r.y + r.h; y += step) {
        const float h = std::min(dash, r.y + r.h - y);
        ui_rect(arena, ctx, Rect{r.x, y, 1.0f, h}, c);
        ui_rect(arena, ctx, Rect{r.x + r.w - 1.0f, y, 1.0f, h}, c);
    }
}

void ui_text(Arena* arena, UIContext* ctx, float x, float y, const char* text,
             Color color, float size, uint8_t weight) {
    if (!text || !*text) return;
    RenderCommand* cmd = ui_alloc_cmd(arena);
    if (!cmd) return;
    cmd->type = CMD_DRAW_TEXT;
    cmd->bounds = Rect{x, y, 0.0f, 0.0f};
    cmd->color = color;
    cmd->text = text;
    cmd->text_size = size;
    cmd->text_weight = weight;
    push_cmd(ctx, cmd);
}

void ui_text_bold(Arena* arena, UIContext* ctx, float x, float y, const char* text,
                  Color color, float size) {
    ui_text(arena, ctx, x, y, text, color, size, 1);
}

void ui_text_in_rect(Arena* arena, UIContext* ctx, Rect r, const char* text,
                     Color color, float size, uint8_t weight) {
    const float y = r.y + (r.h - font_line_height(size)) * 0.5f;
    ui_text(arena, ctx, r.x, y, text, color, size, weight);
}

void ui_text_center(Arena* arena, UIContext* ctx, Rect r, const char* text,
                    Color color, float size, uint8_t weight) {
    const float w = text_width(text, size, weight);
    const float x = r.x + (r.w - w) * 0.5f;
    const float y = r.y + (r.h - font_line_height(size)) * 0.5f;
    ui_text(arena, ctx, x, y, text, color, size, weight);
}

void ui_text_right(Arena* arena, UIContext* ctx, Rect r, const char* text,
                   Color color, float size, uint8_t weight) {
    const float w = text_width(text, size, weight);
    const float y = r.y + (r.h - font_line_height(size)) * 0.5f;
    ui_text(arena, ctx, r.x + r.w - w, y, text, color, size, weight);
}

void ui_icon(Arena* arena, UIContext* ctx, float x, float y, uint32_t codepoint,
             Color color, float size) {
    char* buf = static_cast<char*>(arena->alloc(5, 1));
    if (!buf) return;
    const int n = utf8_encode(codepoint, buf);
    buf[n] = '\0';
    ui_text(arena, ctx, x, y, buf, color, size);
}

void ui_icon_center(Arena* arena, UIContext* ctx, Rect r, uint32_t codepoint,
                    Color color, float size) {
    ui_icon(arena, ctx, r.x + (r.w - size) * 0.5f, r.y + (r.h - size) * 0.5f,
            codepoint, color, size);
}

void ui_icon_gradient(Arena* arena, UIContext* ctx, float x, float y,
                      uint32_t codepoint, Color left, Color right, float size) {
    char* buf = static_cast<char*>(arena->alloc(5, 1));
    if (!buf) return;
    const int n = utf8_encode(codepoint, buf);
    buf[n] = '\0';
    RenderCommand* cmd = ui_alloc_cmd(arena);
    if (!cmd) return;
    cmd->type = CMD_DRAW_TEXT;
    cmd->bounds = Rect{x, y, 0.0f, 0.0f};
    cmd->color = left;
    cmd->color2 = right;
    cmd->text = buf;
    cmd->text_size = size;
    cmd->text_weight = 0;
    push_cmd(ctx, cmd);
}

bool ui_icon_button_ex(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                       Color icon_color, Color hover_bg, float radius,
                       float icon_size, UIBehavior behavior, uint64_t id) {
    if (!id) {
        id = ui_get_id(ctx, nullptr, (uint64_t)icon ^ (uint64_t)(r.x * 1000.0f + r.y));
    }
    const UIInteraction inter = ui_process_interaction(ctx, id, r, behavior);
    if (inter.hovered && hover_bg.a > 0) {
        Color bg = inter.pressed ? color_darken(hover_bg, 15) : hover_bg;
        ui_rect(arena, ctx, r, bg, radius);
    }
    Color ic = inter.hovered ? color_lighten(icon_color, 20) : icon_color;
    if (inter.pressed) ic = color_darken(ic, 15);
    ui_icon_center(arena, ctx, r, icon, ic, icon_size);
    return inter.clicked;
}

bool ui_icon_button(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                    Color icon_color, Color hover_bg, float radius,
                    float icon_size) {
    return ui_icon_button_ex(arena, ctx, r, icon, icon_color, hover_bg, radius, icon_size);
}

bool ui_button_icon_text_ex(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                            const char* label, Color bg, float radius,
                            float icon_size, float text_size, bool bold,
                            Color text_color, Color icon_color, float text_gap,
                            UIBehavior behavior, uint64_t id) {
    if (!id) {
        id = ui_get_id(ctx, label, (uint64_t)icon ^ (uint64_t)(r.x * 1000.0f + r.y));
    }
    const UIInteraction inter = ui_process_interaction(ctx, id, r, behavior);
    const Color base = ui_hover_color(bg, inter.hovered, inter.pressed);
    ui_rect(arena, ctx, r, base, radius);

    Color fg = text_color.a > 0 ? text_color : ui_contrast_text(base);
    if (inter.hovered && text_color.a > 0) fg = color_lighten(fg, 12);
    Color ifg = icon_color.a > 0 ? icon_color : fg;
    if (inter.hovered) ifg = color_lighten(ifg, 10);
    const float tw = text_width(label, text_size, bold ? 1 : 0);
    const float gap = (label && *label && icon) ? text_gap : 0.0f;
    const float iw = icon ? icon_size : 0.0f;
    const float total = iw + gap + tw;
    float x = r.x + (r.w - total) * 0.5f;

    if (icon) {
        ui_icon(arena, ctx, x, r.y + (r.h - icon_size) * 0.5f, icon, ifg, icon_size);
        x += icon_size + gap;
    }
    if (label && *label) {
        const float ty = r.y + (r.h - font_line_height(text_size)) * 0.5f;
        if (bold) ui_text_bold(arena, ctx, x, ty, label, fg, text_size);
        else ui_text(arena, ctx, x, ty, label, fg, text_size);
    }
    return inter.clicked;
}

bool ui_button_icon_text(Arena* arena, UIContext* ctx, Rect r, uint32_t icon,
                         const char* label, Color bg, float radius,
                         float icon_size, float text_size, bool bold,
                         Color text_color, Color icon_color, float text_gap) {
    return ui_button_icon_text_ex(arena, ctx, r, icon, label, bg, radius,
                                  icon_size, text_size, bold, text_color,
                                  icon_color, text_gap);
}

bool ui_button_raw(Arena* arena, UIContext* ctx, Rect rect, const char* label,
                   Color color, float radius) {
    return ui_button_icon_text(arena, ctx, rect, 0, label, color, radius, 0.0f);
}

void ui_label_raw(Arena* arena, UIContext* ctx, Rect rect, const char* text, Color color) {
    ui_text_in_rect(arena, ctx, rect, text, color, ctx->theme ? ctx->theme->font_md : 14.0f);
}

bool ui_checkbox(Arena* arena, UIContext* ctx, float x, float y,
                 const char* label, bool* checked, const Theme* theme) {
    const float box = 22.0f;
    const float label_size = theme ? theme->font_sm + 0.9f : 13.9f;
    const float label_w = text_width(label, label_size, 2);
    const Rect row{x, y, box + 7.0f + label_w, box};
    const uint64_t id = ui_get_id(ctx, label, (uint64_t)(x * 1000.0f + y));
    const UIInteraction inter = ui_process_interaction(ctx, id, row);
    if (inter.clicked && checked) *checked = !*checked;

    const Color border = (checked && *checked) ? theme->accent
                                               : (inter.hovered ? theme->border_strong : theme->border);
    ui_rect(arena, ctx, Rect{x, y, box, box}, (checked && *checked) ? theme->accent : theme->bg_app, 5.5f);
    ui_rect_border(arena, ctx, Rect{x, y, box, box}, border, 5.5f, 1.0f);
    if (checked && *checked) {
        ui_icon(arena, ctx, x + 4.0f, y + 4.0f, 0xE017u, theme->on_accent, 16.0f);
    }

    const Color base_text = color_mix(theme->text, theme->text_muted, 0.18f);
    const Color text = inter.hovered ? theme->text : base_text;
    ui_text(arena, ctx, x + box + 7.0f,
            y - 0.8f + (box - font_line_height(label_size)) * 0.5f,
            label, text, label_size, 2);

    return inter.clicked;
}

bool ui_toggle(Arena* arena, UIContext* ctx, Rect r, bool* value, const Theme* theme) {
    const uint64_t id = ui_get_id_rect(ctx, r);
    const UIInteraction inter = ui_process_interaction(ctx, id, r);
    if (inter.clicked && value) *value = !*value;

    const Color bg = (value && *value) ? theme->accent : theme->bg_control;
    ui_rect(arena, ctx, r, inter.hovered ? color_lighten(bg, 10) : bg, r.h * 0.5f);
    const float knob = r.h - 4.0f;
    const float kx = (value && *value) ? r.x + r.w - knob - 2.0f : r.x + 2.0f;
    ui_rect(arena, ctx, Rect{kx, r.y + 2.0f, knob, knob}, theme->on_accent, knob * 0.5f);
    return inter.clicked;
}

const char* ui_text_ellipsized(Arena* arena, const char* text, float size,
                               uint8_t weight, float max_w) {
    if (!text || max_w <= 0.0f) return text;
    if (text_width(text, size, weight) <= max_w) return text;

    constexpr const char* kEllipsis = "\xE2\x80\xA6";  // U+2026
    const float budget = std::max(0.0f, max_w - text_width(kEllipsis, size, weight));
    const float scale = size / static_cast<float>(kFontBakeSize);

    char* out = static_cast<char*>(arena->alloc(std::strlen(text) + 4, 1));
    if (!out) return text;

    size_t n = 0;
    float used = 0.0f;
    const char* p = text;
    while (*p) {
        const char* start = p;
        const uint32_t cp = utf8_next(p);
        if (cp == 0) break;
        const Glyph* g = font_find(cp, weight);
        const float adv = g ? g->advance * scale : 0.0f;
        if (used + adv > budget) break;
        used += adv;
        for (const char* q = start; q < p; ++q) out[n++] = *q;
    }
    for (const char* q = kEllipsis; *q; ++q) out[n++] = *q;
    out[n] = '\0';
    return out;
}

void ui_image_uv(Arena* arena, UIContext* ctx, Rect dst, uint64_t texture_id,
                 Rect uv, Color tint) {
    if (texture_id == 0) return;
    RenderCommand* cmd = ui_alloc_cmd(arena);
    if (!cmd) return;
    cmd->type = CMD_DRAW_TEXTURE;
    cmd->bounds = dst;
    cmd->color = tint;
    cmd->texture_id = texture_id;
    cmd->uv = uv;
    push_cmd(ctx, cmd);
}

void ui_image(Arena* arena, UIContext* ctx, Rect dst, uint64_t texture_id,
              Color tint) {
    ui_image_uv(arena, ctx, dst, texture_id, Rect{0.0f, 0.0f, 1.0f, 1.0f}, tint);
}

bool ui_text_input(Arena* arena, UIContext* ctx, Rect r, const char* text,
                   Color bg, Color border, const Theme* theme) {
    const uint64_t id = ui_get_id_rect(ctx, r);
    const UIInteraction inter = ui_process_interaction(ctx, id, r);
    ui_rect(arena, ctx, r, bg, theme->radius_sm);
    ui_rect_border(arena, ctx, r, inter.hovered ? theme->border_strong : border,
                   theme->radius_sm, 1.0f);
    const float path_size = theme->font_sm;
    const Color path_color = color_mix(theme->text_muted, theme->text, 0.7f);
    ui_text_in_rect(arena, ctx, Rect{r.x + 7.0f, r.y + 2.0f, r.w - 14.0f, r.h},
                    ui_text_ellipsized(arena, text, path_size, 2, r.w - 14.0f),
                    path_color, path_size, 2);
    return inter.clicked;
}

void ui_progress(Arena* arena, UIContext* ctx, Rect r, float fraction,
                 Color bg, Color fill) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    ui_rect(arena, ctx, r, bg, r.h * 0.5f);
    if (fraction > 0.0f) {
        ui_rect(arena, ctx, Rect{r.x, r.y, r.w * fraction, r.h}, fill, r.h * 0.5f);
    }
}

void ui_stat_row(Arena* arena, UIContext* ctx, Rect r, const char* label,
                 const char* value, const Theme* theme) {
    ui_text_in_rect(arena, ctx, Rect{r.x, r.y, r.w * 0.55f, r.h}, label,
                    theme->text_muted, theme->font_sm);
    ui_text_right(arena, ctx, Rect{r.x, r.y, r.w, r.h}, value,
                  theme->text, theme->font_sm);
}

bool ui_combo(Arena* arena, UIContext* ctx, Rect rect,
              const char* const* items, int item_count,
              int* selected_index, const Theme* theme,
              uint64_t id) {
    if (!ctx || !items || item_count <= 0) return false;
    if (!theme) theme = ctx->theme ? ctx->theme : &theme_faceforge();
    if (!id) id = ui_get_id_rect(ctx, rect);

    const bool is_open = (ctx->active_popup_id == id);
    const UIInteraction inter = ui_process_interaction(ctx, id, rect);

    // Toggle dropdown on click
    if (inter.clicked) {
        if (is_open) {
            ctx->active_popup_id = 0;
            ctx->active_popup_rect = Rect{};
        } else {
            ctx->active_popup_id = id;
            ctx->popup_just_opened = true;
        }
    }

    // Draw main combo button
    const Color bg = inter.hovered ? theme->bg_control_hover : theme->bg_control;
    ui_rect(arena, ctx, rect, bg, 6.0f);
    ui_rect_border(arena, ctx, rect, (inter.hovered || is_open) ? theme->border_strong : theme->border,
                   6.0f, 1.0f);

    const int sel = (selected_index && *selected_index >= 0 && *selected_index < item_count)
                        ? *selected_index : 0;
    const char* sel_text = items[sel];

    ui_text(arena, ctx, rect.x + 7.0f, rect.y + 6.0f,
            sel_text, theme->text, 14.2f, 2);

    // Chevron down (0xE00A)
    ui_icon(arena, ctx, rect.x + rect.w - 30.0f, rect.y + 9.0f,
            0xE00Au, (inter.hovered || is_open) ? theme->text : theme->text_muted, 14.0f);

    bool changed = false;

    // Render dropdown menu in popup overlay layer if open
    if (ctx->active_popup_id == id) {
        const float item_h = 28.0f;
        const float pad = 4.0f;
        const float popup_h = item_count * item_h + pad * 2.0f;
        const float popup_w = rect.w;
        const float popup_x = rect.x;
        float popup_y = rect.y + rect.h + 3.0f;

        // Clamp inside viewport
        if (popup_y + popup_h > ctx->screen_h - 6.0f && rect.y - popup_h - 3.0f >= 0.0f) {
            popup_y = rect.y - popup_h - 3.0f;
        }

        const Rect popup_rect{popup_x, popup_y, popup_w, popup_h};
        ctx->active_popup_rect = popup_rect;

        // Switch to popup layer so items render on top of all normal content
        ctx->in_popup_layer = true;

        // Drop shadow & background panel
        ui_rect(arena, ctx, Rect{popup_x + 2.0f, popup_y + 2.0f, popup_w, popup_h}, Color{0, 0, 0, 0x50}, 6.0f);
        ui_rect(arena, ctx, popup_rect, theme->bg_raised, 6.0f);
        ui_rect_border(arena, ctx, popup_rect, theme->border_strong, 6.0f, 1.0f);

        for (int i = 0; i < item_count; ++i) {
            const Rect item_r{popup_x + pad, popup_y + pad + i * item_h, popup_w - pad * 2.0f, item_h};
            const uint64_t item_id = ui_hash_int(static_cast<uint64_t>(i), id);
            const UIInteraction item_in = ui_process_interaction(ctx, item_id, item_r);

            const bool is_sel = (selected_index && *selected_index == i);
            if (is_sel) {
                ui_rect(arena, ctx, item_r, theme->accent_dim, 4.0f);
            } else if (item_in.hovered) {
                ui_rect(arena, ctx, item_r, theme->bg_control_hover, 4.0f);
            }

            const Color text_col = is_sel ? theme->on_accent
                                          : (item_in.hovered ? theme->text : color_mix(theme->text, theme->text_muted, 0.2f));

            if (is_sel) {
                ui_icon(arena, ctx, item_r.x + 8.0f, item_r.y + (item_h - 13.0f) * 0.5f,
                        0xE017u, theme->on_accent, 13.0f);
            }

            const float text_x = item_r.x + (is_sel ? 26.0f : 10.0f);
            ui_text(arena, ctx, text_x,
                    item_r.y + (item_h - font_line_height(13.5f)) * 0.5f,
                    items[i], text_col, 13.5f, 2);

            if (item_in.clicked) {
                if (selected_index) *selected_index = i;
                ctx->active_popup_id = 0;
                ctx->active_popup_rect = Rect{};
                changed = true;
            }
        }

        ctx->in_popup_layer = false;
    }

    return changed;
}

bool ui_combo_selector_items(Arena* arena, UIContext* ctx, LayoutContext* layout, float height,
                             const char* title, const char* const* items, int item_count,
                             int* selected_index, ComboStyle /*style*/,
                             uint64_t id) {
    const Rect total{layout->bounds.x + layout->padding, layout->current_y,
                     layout->bounds.w - layout->padding * 2.0f, height};
    layout->current_y += height + layout->padding;

    const Rect label_rect{total.x, total.y, total.w * 0.3f, total.h};
    const Rect box_rect{total.x + total.w * 0.35f, total.y, total.w * 0.65f, total.h};

    const Theme* theme = ctx->theme ? ctx->theme : &theme_faceforge();
    ui_text_in_rect(arena, ctx, label_rect, title, theme->text, theme->font_md);

    return ui_combo(arena, ctx, box_rect, items, item_count, selected_index, theme, id);
}

bool ui_combo_selector(Arena* arena, UIContext* ctx, LayoutContext* layout, float height,
                       const char* title, const char* current_selection, ComboStyle style) {
    const Rect total{layout->bounds.x + layout->padding, layout->current_y,
                     layout->bounds.w - layout->padding * 2.0f, height};
    layout->current_y += height + layout->padding;

    const Rect label_rect{total.x, total.y, total.w * 0.3f, total.h};
    const Rect box_rect{total.x + total.w * 0.35f, total.y, total.w * 0.65f, total.h};

    const Theme* theme = ctx->theme ? ctx->theme : &theme_faceforge();
    ui_text_in_rect(arena, ctx, label_rect, title, theme->text, theme->font_md);

    const uint64_t id = ui_get_id_rect(ctx, box_rect);
    const UIInteraction inter = ui_process_interaction(ctx, id, box_rect);
    const Color bg = inter.hovered ? style.bg_hover : style.bg_normal;
    ui_rect(arena, ctx, box_rect, bg, style.radius);
    ui_rect_border(arena, ctx, box_rect, inter.hovered ? theme->border_strong : theme->border,
                   style.radius, 1.0f);

    ui_text_in_rect(arena, ctx,
                    Rect{box_rect.x + 12.0f, box_rect.y, box_rect.w - 40.0f, box_rect.h},
                    current_selection, style.text_color, theme->font_md);
    ui_icon(arena, ctx, box_rect.x + box_rect.w - 26.0f,
            box_rect.y + (box_rect.h - 14.0f) * 0.5f, 0xE00Au,
            inter.hovered ? theme->text : theme->text_muted, 14.0f);
    return inter.clicked;
}

bool ui_context_menu_area(UIContext* ctx, uint64_t menu_id, Rect area) {
    if (!ctx) return false;
    if (ui_right_clicked(ctx, menu_id, area)) {
        ui_open_context_menu(ctx, menu_id, ctx->mouse_x, ctx->mouse_y);
        return true;
    }
    return false;
}

void ui_open_context_menu(UIContext* ctx, uint64_t menu_id, float x, float y) {
    if (!ctx) return;
    ctx->active_popup_id = menu_id;
    ctx->menu_x = x;
    ctx->menu_y = y;
    ctx->popup_just_opened = true;
}

int ui_context_menu(Arena* arena, UIContext* ctx, uint64_t menu_id,
                    const MenuItem* items, int item_count,
                    const Theme* theme) {
    if (!ctx || !items || item_count <= 0) return -1;
    if (ctx->active_popup_id != menu_id) return -1;
    if (!theme) theme = ctx->theme ? ctx->theme : &theme_faceforge();

    const float item_h = 28.0f;
    const float sep_h = 9.0f;
    const float pad = 4.0f;
    const float menu_w = 210.0f;

    float total_h = pad * 2.0f;
    for (int i = 0; i < item_count; ++i) {
        total_h += items[i].separator ? sep_h : item_h;
    }

    float x = ctx->menu_x;
    float y = ctx->menu_y;

    // Clamp inside viewport
    if (x + menu_w > ctx->screen_w - 6.0f) x = ctx->screen_w - menu_w - 6.0f;
    if (y + total_h > ctx->screen_h - 6.0f) y = ctx->screen_h - total_h - 6.0f;
    if (x < 6.0f) x = 6.0f;
    if (y < 6.0f) y = 6.0f;

    const Rect menu_rect{x, y, menu_w, total_h};
    ctx->active_popup_rect = menu_rect;

    int clicked_index = -1;

    ctx->in_popup_layer = true;

    // Menu panel background with subtle drop shadow
    ui_rect(arena, ctx, Rect{x + 2.0f, y + 2.0f, menu_w, total_h}, Color{0, 0, 0, 0x60}, 6.0f);
    ui_rect(arena, ctx, menu_rect, theme->bg_raised, 6.0f);
    ui_rect_border(arena, ctx, menu_rect, theme->border_strong, 6.0f, 1.0f);

    float cur_y = y + pad;
    for (int i = 0; i < item_count; ++i) {
        const MenuItem& item = items[i];
        if (item.separator) {
            ui_separator(arena, ctx, x + 8.0f, cur_y + 4.0f, menu_w - 16.0f,
                         color_alpha(theme->border, 0xd0));
            cur_y += sep_h;
            continue;
        }

        const Rect item_r{x + pad, cur_y, menu_w - pad * 2.0f, item_h};
        const uint64_t item_id = ui_hash_int(static_cast<uint64_t>(i), menu_id);
        const UIInteraction inter = ui_process_interaction(ctx, item_id, item_r);

        if (!item.disabled) {
            if (inter.hovered) {
                ui_rect(arena, ctx, item_r, theme->bg_control, 4.0f);
            }
            if (inter.clicked) {
                clicked_index = i;
                ctx->active_popup_id = 0;
                ctx->active_popup_rect = Rect{};
            }
        }

        const Color text_col = item.disabled ? theme->text_dim
                                             : (inter.hovered ? theme->text : color_mix(theme->text, theme->text_muted, 0.25f));
        const Color icon_col = item.disabled ? theme->text_dim
                                             : (inter.hovered ? theme->text : theme->text_muted);

        if (item.icon != 0) {
            ui_icon(arena, ctx, item_r.x + 8.0f, item_r.y + (item_h - 15.0f) * 0.5f,
                    item.icon, icon_col, 15.0f);
        }

        const float label_x = item_r.x + (item.icon != 0 ? 30.0f : 12.0f);
        ui_text(arena, ctx, label_x,
                item_r.y + (item_h - font_line_height(13.5f)) * 0.5f,
                item.label ? item.label : "", text_col, 13.5f, 2);

        if (item.shortcut && *item.shortcut) {
            const float sw = text_width(item.shortcut, 12.0f, 0);
            ui_text(arena, ctx, item_r.x + item_r.w - sw - 8.0f,
                    item_r.y + (item_h - font_line_height(12.0f)) * 0.5f,
                    item.shortcut, theme->text_muted, 12.0f, 0);
        }

        cur_y += item_h;
    }

    ctx->in_popup_layer = false;
    return clicked_index;
}
