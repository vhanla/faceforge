// FaceForge native screen (see ui_faceforge.hpp).
//
// This began as the AMUIE demo mockup and is now the real application screen:
// the photo workspace is driven by the UI-agnostic app layer (app::App), while
// the video workspace is still the design mockup and will be wired in a later
// phase. The draw pass never blocks on inference: app::App runs jobs on its own
// worker thread and publishes status, faces and the composed result, which this
// file reads through thread-safe snapshots.

#include "ui_faceforge.hpp"

#include "platform/window.hpp"

#include "core/font.hpp"
#include "core/theme.hpp"
#include "widgets/ui_widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr float kDesignW = 1532.0f;
constexpr float kDesignH = 1027.0f;

// Format into the frame arena. RenderCommands store a raw `const char*` that the
// backend consumes only after the whole UI pass (and this function) has returned,
// so a stack buffer would dangle and render as garbage. Arena memory lives until
// the frame is dispatched.
char* arena_fmt(Arena* a, const char* fmt, ...) {
    constexpr size_t kCap = 256;
    char* buf = static_cast<char*>(a->alloc(kCap, 1));
    if (!buf) return const_cast<char*>("");
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, kCap, fmt, args);
    va_end(args);
    return buf;
}


// Icon codepoints baked by tools/gen_assets.py.
enum Icon : uint32_t {
    ICON_USER = 0xE001,
    ICON_SPARKLES = 0xE002,
    ICON_CLOCK = 0xE003,
    ICON_PANELS = 0xE004,
    ICON_CHART = 0xE005,
    ICON_SAVE = 0xE006,
    ICON_COPY = 0xE007,
    ICON_PLAY = 0xE008,
    ICON_TRASH = 0xE009,
    ICON_CHEVRON = 0xE00A,
    ICON_IMAGE_PLUS = 0xE00B,
    ICON_SETTINGS = 0xE00C,
    ICON_INFO = 0xE00D,
    ICON_PLUS = 0xE00E,
    ICON_X = 0xE00F,
    ICON_MINUS = 0xE010,
    ICON_SQUARE = 0xE011,
    ICON_EXTERNAL = 0xE012,
    ICON_UNDO = 0xE013,
    ICON_REDO = 0xE014,
    ICON_FOLDER = 0xE015,
    ICON_SCAN_FACE = 0xE016,
    ICON_CHECK = 0xE017,
    ICON_IMAGE = 0xE018,
    ICON_CLIPBOARD = 0xE019,
    ICON_ROTATE_CCW = 0xE01B,
    ICON_MONITOR = 0xE01E,
    ICON_LOGO = 0xE01F,
    ICON_SLIDERS = 0xE020,
    ICON_HISTORY = 0xE021,
    ICON_BOX = 0xE022,
    ICON_FOLDER_CLOSED = 0xE023,
    ICON_PHOTO = 0xE024,
    ICON_USER_FILLED = 0xE030,
    ICON_SLIDERS_FILLED = 0xE031,
    ICON_HISTORY_FILLED = 0xE032,
    ICON_BOX_FILLED = 0xE033,
    ICON_FOLDER_FILLED = 0xE034,
    ICON_CHART_BARS = 0xE035,
    ICON_SETTINGS_FILLED = 0xE036,
    ICON_SCAN_FACE_FILLED = 0xE037,
    ICON_SCAN_PERSON = 0xE107,
    ICON_IMAGE_HERO = 0xE106,
    ICON_SCAN_LARGE = 0xE103,
    ICON_USER_LARGE = 0xE104,
    ICON_PLAY_FILLED = 0xE105,
    ICON_SCAN_FACE_LARGE = 0xE101,
};

// Demo-local tones measured from the reference screenshot.
constexpr Color kToolbarBtn{0x25, 0x28, 0x2e, 0xff};
constexpr Color kToolbarText{0xee, 0xf0, 0xf2, 0xff};
constexpr Color kControlText{0xf0, 0xf2, 0xf4, 0xff};
constexpr Color kFacesCard{0x22, 0x24, 0x2a, 0xff};

inline bool demo_dark(const Theme& t) {
    return static_cast<int>(t.bg_app.r) + t.bg_app.g + t.bg_app.b < 384;
}
inline Color tone_toolbar_btn(const Theme& t) {
    return demo_dark(t) ? kToolbarBtn : t.bg_control;
}
inline Color tone_toolbar_text(const Theme& t) {
    return demo_dark(t) ? kToolbarText : t.text;
}
inline Color tone_control_text(const Theme& t) {
    return demo_dark(t) ? kControlText : t.text;
}
inline Color tone_faces_card(const Theme& t) {
    return demo_dark(t) ? kFacesCard : t.bg_raised;
}

struct Layout {
    float w;
    float h;
    float header_h = 64.0f;
    float toolbar_h = 57.0f;
    float status_h = 26.0f;
    float rail_w = 54.0f;
    float left_x = 54.0f;
    float left_w = 288.0f;
    float right_w = 255.0f;
    float right_x = 1267.0f;
    float canvas_x = 352.0f;
    float canvas_y = 196.0f;
    float canvas_w = 900.0f;
    float canvas_h = 740.0f;
    bool show_right = true;
};

inline Layout compute_layout(const UIContext* ctx, const UIState* state) {
    Layout L;
    L.w = ctx->screen_w > 0.0f ? ctx->screen_w : kDesignW;
    L.h = ctx->screen_h > 0.0f ? ctx->screen_h : kDesignH;
    const Theme& t = *ctx->theme;

    L.rail_w = state->sidebar_expanded ? 148.0f : 54.0f;
    L.left_x = L.rail_w;
    L.left_w = 288.0f;

    L.show_right = L.w >= (t.bp_sidebar_collapsed + L.rail_w);
    if (!L.show_right) {
        L.right_w = 0.0f;
        L.right_x = L.w;
    } else {
        L.right_x = L.w - 265.0f;
    }

    L.canvas_x = L.left_x + L.left_w + 10.0f;
    L.canvas_y = 124.0f + 68.0f; // 192.0f
    const float right_edge = L.show_right ? L.right_x - 19.0f : L.w - 19.0f;
    L.canvas_w = right_edge - L.canvas_x;
    L.canvas_h = L.h - L.status_h - 10.0f - L.canvas_y;
    if (L.canvas_w < 120.0f) L.canvas_w = 120.0f;
    if (L.canvas_h < 120.0f) L.canvas_h = 120.0f;
    return L;
}

void section_header(Arena* a, UIContext* ctx, const Theme& t, float left_x, float top,
                    uint32_t icon, const char* label) {
    ui_icon(a, ctx, left_x + 20.0f, top, icon, t.accent, 24.5f);
    ui_text_bold(a, ctx, left_x + 55.5f, top - 0.5f, label, t.text, 15.0f);
}

bool toolbar_text_button(Arena* a, UIContext* ctx, Rect r, const char* label) {
    const Theme& t = *ctx->theme;
    const uint64_t id = ui_get_id(ctx, label, static_cast<uint64_t>(r.x * 1000.0f + r.y));
    const UIInteraction inter = ui_process_interaction(ctx, id, r);
    const Color bg = ui_hover_color(tone_toolbar_btn(t), inter.hovered, inter.pressed);
    ui_rect(a, ctx, r, bg, 7.0f);
    ui_text_bold(a, ctx, r.x + (r.w - text_width(label, 14.0f, 1)) * 0.5f,
                 r.y + (r.h - font_line_height(14.0f)) * 0.5f - 0.7f,
                 label, inter.hovered ? color_lighten(tone_toolbar_text(t), 12) : tone_toolbar_text(t), 14.0f);
    return inter.clicked;
}

bool pill(Arena* a, UIContext* ctx, const Theme& t, Rect r, const char* label,
          bool active) {
    const uint64_t id = ui_get_id(ctx, label, static_cast<uint64_t>(r.x * 1000.0f + r.y));
    const UIInteraction inter = ui_process_interaction(ctx, id, r);
    const Color bg = active ? t.accent : ui_hover_color(t.bg_control, inter.hovered, inter.pressed);
    ui_rect(a, ctx, r, bg, 10.0f);
    if (active) ui_text_center(a, ctx, r, label, t.on_accent, 14.2f, 1);
    else ui_text_center(a, ctx, r, label, t.text, 14.2f, 1);
    return inter.clicked;
}

app::App* app_of(UIState* state) { return state->app; }

// ---------------------------------------------------------------------------
// 1. Navigation Rail (Collapsible Sidebar Menu)
// ---------------------------------------------------------------------------
void nav_sidebar(Arena* a, UIContext* ctx, UIState* state, float rail_w) {
    const Theme& t = *ctx->theme;
    const float H = ctx->screen_h;
    const float status_h = 26.0f;
    const float rail_h = H - 64.0f - status_h;

    ui_rect(a, ctx, Rect{0, 64, rail_w, rail_h}, t.bg_header);
    ui_separator(a, ctx, rail_w - 1.0f, 64, 1.0f, color_alpha(t.border, 0xc0));

    const Rect toggle_r = state->sidebar_expanded
                              ? Rect{rail_w - 42.0f, 72.0f, 34.0f, 32.0f}
                              : Rect{9.0f, 72.0f, 36.0f, 32.0f};
    if (ui_icon_button(a, ctx, toggle_r, ICON_PANELS, t.text_muted, t.bg_control_hover, 6.0f, 17.0f)) {
        state->sidebar_expanded = !state->sidebar_expanded;
    }

    auto draw_rail_item = [&](float y, uint32_t icon, const char* label, bool active) -> bool {
        const Rect r = state->sidebar_expanded
                           ? Rect{8.0f, y, rail_w - 16.0f, 40.0f}
                           : Rect{7.0f, y, 40.0f, 40.0f};
        const uint64_t id = ui_get_id(ctx, label, (uint64_t)icon ^ (uint64_t)(r.x * 1000.0f + r.y));
        const UIInteraction in = ui_process_interaction(ctx, id, r);

        const Color bg = active ? t.accent : (in.hovered ? t.bg_control_hover : Color{0, 0, 0, 0});
        if (bg.a > 0) {
            ui_rect(a, ctx, r, in.pressed ? color_darken(bg, 15) : bg, 8.0f);
        }
        if (active) {
            ui_rect(a, ctx, Rect{0, y + 6.0f, 3.5f, 28.0f}, t.accent, 2.0f);
        }

        const Color fg = active ? t.on_accent : (in.hovered ? t.text : color_mix(t.text_muted, t.text, 0.35f));
        if (state->sidebar_expanded) {
            ui_icon(a, ctx, r.x + 12.0f, r.y + 10.0f, icon, fg, 20.0f);
            ui_text_bold(a, ctx, r.x + 40.0f, r.y + 11.0f, label, fg, 14.0f);
        } else {
            ui_icon_center(a, ctx, r, icon, fg, 20.0f);
        }
        return in.clicked;
    };

    if (draw_rail_item(116.0f, ICON_PHOTO, "Photo", state->mode == AppMode::Photo)) {
        state->mode = AppMode::Photo;
    }
    if (draw_rail_item(162.0f, ICON_PLAY_FILLED, "Video", state->mode == AppMode::Video)) {
        state->mode = AppMode::Video;
    }

    const float bot_y = 64.0f + rail_h - 10.0f;
    ui_separator(a, ctx, 8.0f, bot_y - 94.0f, rail_w - 16.0f, color_alpha(t.border, 0x80));

    if (draw_rail_item(bot_y - 84.0f, ICON_SETTINGS_FILLED, "Settings", state->show_settings)) {
        state->show_settings = !state->show_settings;
        if (state->show_settings) state->show_about = false;
    }
    if (draw_rail_item(bot_y - 40.0f, ICON_INFO, "About", state->show_about)) {
        state->show_about = !state->show_about;
        if (state->show_about) state->show_settings = false;
    }
}

// ---------------------------------------------------------------------------
// 2. Header
// ---------------------------------------------------------------------------
void header(Arena* a, UIContext* ctx, UIState* state) {
    const Theme& t = *ctx->theme;
    const float W = ctx->screen_w;
    ui_rect(a, ctx, Rect{0, 0, W, 64}, t.bg_header);
    ui_separator(a, ctx, 0, 63.5f, W, color_alpha(t.border, 0x90));

    ui_icon_gradient(a, ctx, 19.0f, 13.8f, ICON_LOGO, Color{0x41, 0x76, 0xee, 0xff},
                     Color{0xb3, 0x2e, 0xf2, 0xff}, 32.0f);
    ui_text_bold(a, ctx, 67.0f, 18.0f, "FaceForge", t.text, 15.0f);
    ui_text(a, ctx, 152.0f, 21.0f, "restore \xC2\xB7 swap \xC2\xB7 enhance", t.text_dim, 12.0f);

    const Rect btn_close{W - 48.0f, 15, 32, 28};
    const Rect btn_max{W - 108.0f, 15, 32, 28};
    const Rect btn_min{W - 168.0f, 15, 32, 28};

    const bool over_buttons =
        ui_hovered(ctx, btn_min) || ui_hovered(ctx, btn_max) || ui_hovered(ctx, btn_close);

    if (ui_icon_button(a, ctx, btn_min, ICON_MINUS, t.text, t.bg_control_hover, 6.0f, 17.0f))
        state->window_action = WindowAction::Minimize;
    if (ui_icon_button(a, ctx, btn_max, ICON_SQUARE, t.text, t.bg_control_hover, 6.0f, 15.0f))
        state->window_action = WindowAction::Maximize;
    if (ui_icon_button(a, ctx, btn_close, ICON_X, t.text, t.danger, 6.0f, 22.0f))
        state->window_action = WindowAction::Close;

    state->header_drag = !over_buttons && ui_hovered(ctx, Rect{0, 0, W - 170.0f, 64});
}

// ---------------------------------------------------------------------------
// 3. Top Toolbar
// ---------------------------------------------------------------------------
void toolbar(Arena* a, UIContext* ctx, UIState* state, float rail_w) {
    const Theme& t = *ctx->theme;
    const float W = ctx->screen_w;
    const float tool_w = W - rail_w;

    ui_rect(a, ctx, Rect{rail_w, 65, tool_w, 56}, t.bg_toolbar);
    ui_rect(a, ctx, Rect{rail_w, 64, tool_w, 1}, color_darken(t.bg_toolbar, 3));
    ui_rect(a, ctx, Rect{rail_w, 121, tool_w, 1}, Color{0x1a, 0x1c, 0x21, 0xff});
    ui_rect(a, ctx, Rect{rail_w, 122, tool_w, 2}, Color{0x12, 0x13, 0x17, 0xff});

    app::App* app = app_of(state);
    const bool busy = app && app->busy();

    const float start_x = rail_w + 14.0f;
    float cur_x = start_x;

    static const char* kPhotoLabels[] = {"Open target...", "Paste target",
                                         "Add identity...", "Paste identity"};
    static const char* kVideoLabels[] = {"Open video...", "Paste video",
                                         "Add identity...", "Paste identity"};
    static const float kToolbarW[] = {114.0f, 106.0f, 114.0f, 112.0f};
    const bool video = state->mode == AppMode::Video;
    const char* const* labels = video ? kVideoLabels : kPhotoLabels;
    const float right_limit = W - 20.0f;
    for (int i = 0; i < 4; ++i) {
        if (cur_x + kToolbarW[i] > right_limit) break;
        const Rect r{cur_x, 74, kToolbarW[i], 40};
        if (toolbar_text_button(a, ctx, r, labels[i])) {
            if (!app) {
            } else if (video) {
                // Video pipeline is not wired yet.
            } else if (i == 0) {
                const std::string p = platform::open_file_dialog(
                    "Open target image",
                    "*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff",
                    app->target_path.c_str());
                if (!p.empty()) {
                    app->openTarget(p);
                    state->sync_output_path = true;
                }
            } else if (i == 1) {
                app->setStatus("Clipboard image paste is not supported yet", true);
            } else if (i == 2) {
                const std::string p = platform::open_file_dialog(
                    "Add identity image",
                    "*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff", "");
                if (!p.empty()) app->addIdentityFile(p);
            } else {
                app->setStatus("Clipboard image paste is not supported yet", true);
            }
        }
        cur_x += kToolbarW[i] + 8.0f;
    }

    const Rect readout{W - 200.0f, 74, 180, 40};
    if (readout.x >= cur_x + 130.0f) {
        const char* txt;
        if (app && !app->providerName().empty()) {
            txt = arena_fmt(a, "Engine %s", app->providerName().c_str());
        } else if (busy && app) {
            txt = arena_fmt(a, "%s %d%%", app::stageText(app->stage()),
                            app->progressPermille() / 10);
        } else {
            txt = "Engine idle";
        }
        ui_text_right(a, ctx, readout, txt, t.text_dim, 12.0f);
    }
}

// ---------------------------------------------------------------------------
// 4. Photo Mode: Left Sidebar, Center Canvas, Right Sidebar
// ---------------------------------------------------------------------------
void photo_left_sidebar(Arena* a, UIContext* ctx, UIState* state, const Layout& L) {
    const Theme& t = *ctx->theme;
    const float H = ctx->screen_h;
    const float x = L.left_x;
    const float w = L.left_w;
    const float view_y = 124.0f;
    const float view_bottom = H - L.status_h;

    ui_rect(a, ctx, Rect{x, view_y, w, view_bottom - view_y}, t.bg_panel);
    ui_separator(a, ctx, x + w - 0.5f, view_y, 1.0f, t.border);

    app::App* app = app_of(state);
    app::IdentitiesSnapshot ids;
    if (app) ids = app->identitiesSnapshot();

    const float footer_h = 68.0f;
    const float scroll_view_bottom = view_bottom - footer_h;
    const float content_bottom = 900.0f;
    const float max_scroll = std::max(0.0f, content_bottom - (scroll_view_bottom - view_y));
    state->left_scroll = std::clamp(state->left_scroll, 0.0f, max_scroll);
    const float oy = view_y + 12.0f - state->left_scroll;

    ui_clip(a, ctx, Rect{x, view_y, w, scroll_view_bottom - view_y});

    // Identities
    section_header(a, ctx, t, x, oy, ICON_USER_FILLED, "Identities");
    if (ids.count == 0) {
        ui_text(a, ctx, x + 21.0f, oy + 36.0f, "No identities yet.", t.text, 14.4f);
        ui_text(a, ctx, x + 21.0f, oy + 61.0f, "Add a face photo to swap.", t.text_dim, 13.5f);
    } else {
        // Up to five identity chips; the default gets an accent ring.
        const float chip = 44.0f;
        const float gap = 7.0f;
        const int max_chips = 5;
        for (int i = 0; i < ids.count && i < max_chips; ++i) {
            const Rect cr{x + 21.0f + static_cast<float>(i) * (chip + gap), oy + 34.0f, chip, chip};
            const uint64_t cid = ui_hash_int(static_cast<uint64_t>(i), 0x1d3e);
            const UIInteraction in = ui_process_interaction(ctx, cid, cr);
            if (state->identity_tex[i] != 0) {
                ui_image(a, ctx, cr, state->identity_tex[i], Color{0xff, 0xff, 0xff, 0xff});
            } else {
                ui_rect(a, ctx, cr, t.bg_raised, 6.0f);
                ui_icon_center(a, ctx, cr, ICON_USER_FILLED, t.text_muted, 20.0f);
            }
            const bool is_default = i == ids.default_identity;
            const Color ring = is_default ? t.accent
                                          : (in.hovered ? t.text_muted : color_alpha(t.border, 0xc0));
            ui_rect_border(a, ctx, cr, ring, 6.0f, is_default ? 2.5f : 1.0f);
            if (in.clicked && app) app->selectDefaultIdentity(i);
        }
        if (ids.count > max_chips) {
            ui_text(a, ctx, x + 21.0f + static_cast<float>(max_chips) * (chip + gap), oy + 48.0f,
                    arena_fmt(a, "+%d", ids.count - max_chips), t.text_dim, 13.0f);
        }
        const int di = std::clamp(ids.default_identity, 0, ids.count - 1);
        ui_text(a, ctx, x + 21.0f, oy + 82.0f,
                arena_fmt(a, "Default: %s", ids.labels[di].c_str()), t.text_dim, 12.5f);
    }

    const Rect ident_area{x + 21.0f, oy + 30.0f, 245.0f, 105.0f};
    const uint64_t ident_menu_id = ui_hash_str("identities_ctx_menu");
    ui_context_menu_area(ctx, ident_menu_id, ident_area);

    static const MenuItem kIdentMenuItems[] = {
        { .label = "Add identity...", .icon = ICON_PLUS },
        { .label = "Paste identity", .icon = ICON_CLIPBOARD, .shortcut = "Ctrl+V" },
        { .separator = true },
        { .label = "Clear all", .icon = ICON_TRASH }
    };
    const int ident_pick = ui_context_menu(a, ctx, ident_menu_id, kIdentMenuItems, 4, &t);
    if (app) {
        if (ident_pick == 0) {
            const std::string p = platform::open_file_dialog(
                "Add identity image", "*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff", "");
            if (!p.empty()) app->addIdentityFile(p);
        } else if (ident_pick == 1) {
            app->setStatus("Clipboard image paste is not supported yet", true);
        } else if (ident_pick == 3) {
            app->clearIdentities();
        }
    }

    if (ui_button_icon_text(a, ctx, Rect{x + 21.0f, oy + 100.0f, 133.0f, 42.0f}, ICON_PLUS, "Add...",
                            t.accent_dim, 8.0f, 18.0f, 14.0f, true)) {
        if (app) {
            const std::string p = platform::open_file_dialog(
                "Add identity image", "*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff", "");
            if (!p.empty()) app->addIdentityFile(p);
        }
    }
    if (ui_button_icon_text(a, ctx, Rect{x + 163.0f, oy + 100.0f, 103.0f, 42.0f}, ICON_TRASH, "Clear",
                            t.bg_control, 8.0f, 18.0f, 13.5f, true, tone_control_text(t))) {
        if (app) app->clearIdentities();
    }

    // Quality
    float qy = oy + 168.0f;
    section_header(a, ctx, t, x, qy, ICON_SLIDERS_FILLED, "Quality");
    ui_text(a, ctx, x + 22.0f, qy + 38.0f, "Enhancer", t.text, 13.5f, 2);

    static const char* kEnhancers[] = {
        "None",
        "GFPGAN 1.4",
        "Real-ESRGAN x2",
        "Real-ESRGAN x4"
    };
    const Rect combo{x + 109.0f, qy + 32.0f, 157.0f, 32.0f};
    int enhancer_index = app ? static_cast<int>(app->enhancer) : 0;
    if (ui_combo(a, ctx, combo, kEnhancers, 4, &enhancer_index, &t)) {
        if (app) {
            app->enhancer = static_cast<app::Enhancer>(enhancer_index);
            app->processAll();
        }
    }

    bool restore = app ? app->restore_mouth : true;
    bool fast = app ? app->fast_preview : true;
    bool unload = app ? app->unload_after_run : false;
    ui_checkbox(a, ctx, x + 24.0f, qy + 75.0f, "Restore mouths from original", &restore, &t);
    ui_checkbox(a, ctx, x + 24.0f, qy + 103.0f, "Fast preview (downscaled)", &fast, &t);
    ui_checkbox(a, ctx, x + 24.0f, qy + 131.0f, "Unload models between runs", &unload, &t);
    if (app) {
        if (restore != app->restore_mouth) { app->restore_mouth = restore; app->processAll(); }
        if (fast != app->fast_preview) { app->fast_preview = fast; app->processAll(); }
        if (unload != app->unload_after_run) { app->unload_after_run = unload; app->markConfigDirty(); }
    }

    // History
    float hy = qy + 180.0f;
    section_header(a, ctx, t, x, hy, ICON_HISTORY_FILLED, "History");
    if (ui_button_icon_text(a, ctx, Rect{x + 21.0f, hy + 29.0f, 245.0f, 36.0f}, 0, "Apply as input",
                            t.bg_control, 7.0f, 0.0f, 14.0f, true, tone_control_text(t))) {
        if (app) app->applyResultAsInput();
    }
    if (ui_button_icon_text(a, ctx, Rect{x + 21.0f, hy + 70.0f, 116.0f, 36.0f}, ICON_UNDO, "Undo",
                            t.bg_control, 7.0f, 13.0f, 13.5f, true, tone_control_text(t), Color{}, 7.0f)) {
        if (app) app->undoHistory();
    }
    if (ui_button_icon_text(a, ctx, Rect{x + 149.0f, hy + 70.0f, 117.0f, 36.0f}, ICON_REDO, "Redo",
                            t.bg_control, 7.0f, 13.0f, 13.5f, true, tone_control_text(t), Color{}, 7.0f)) {
        if (app) app->redoHistory();
    }
    ui_text(a, ctx, x + 21.0f, hy + 113.0f,
            arena_fmt(a, "undo %d \xC2\xB7 redo %d", app ? app->undoCount() : 0,
                      app ? app->redoCount() : 0),
            t.text_dim, 14.5f);

    ui_separator(a, ctx, x + 16.0f, hy + 146.0f, 250.0f, color_alpha(t.border, 0xb0));

    // Studio
    float sy = hy + 160.0f;
    section_header(a, ctx, t, x, sy, ICON_BOX_FILLED, "Studio");
    if (ui_button_icon_text(a, ctx, Rect{x + 21.0f, sy + 28.0f, 245.0f, 36.0f}, ICON_EXTERNAL, "Open Studio",
                            t.bg_control, 7.0f, 16.0f, 14.0f, true, tone_control_text(t))) {
        if (app) app->setStatus("Studio is not wired in the native UI yet", true);
    }
    ui_text(a, ctx, x + 21.0f, sy + 70.0f, "Paint the original back over the result.", t.text_muted, 13.0f);
    ui_text(a, ctx, x + 21.0f, sy + 87.0f, "Erase added hair, blemishes, halos...", t.text_dim, 13.0f);

    // Output
    float oy2 = sy + 120.0f;
    section_header(a, ctx, t, x, oy2, ICON_FOLDER_FILLED, "Output");
    const UITextEditResult ed = ui_text_edit(a, ctx, Rect{x + 21.0f, oy2 + 26.0f, 213.0f, 32.0f},
                                             state->output_path, sizeof(state->output_path), &t,
                                             "output.png", 0x0f01);
    if (app && (ed == UITextEditResult::Changed || ed == UITextEditResult::Submitted)) {
        app->save_path = state->output_path;
        app->save_explicit = true;
    }
    const Rect fld_r{x + 234.0f, oy2 + 26.0f, 32.0f, 32.0f};
    if (ui_icon_button(a, ctx, fld_r, ICON_FOLDER_FILLED, t.text, t.bg_control_hover, 6.0f, 20.0f)) {
        if (app) {
            const std::string p = platform::save_file_dialog(
                "Choose output file", "*.png *.jpg *.jpeg *.webp *.bmp",
                state->output_path, "output.png");
            if (!p.empty()) {
                std::snprintf(state->output_path, sizeof(state->output_path), "%s", p.c_str());
                app->save_path = p;
                app->save_explicit = true;
            }
        }
    }

    if (ui_button_icon_text(a, ctx, Rect{x + 21.0f, oy2 + 69.0f, 141.0f, 37.0f}, ICON_SAVE, "Save as...",
                            t.bg_control, 7.0f, 18.0f, 14.0f, true, tone_control_text(t))) {
        if (app) {
            const std::string p = platform::save_file_dialog(
                "Save result image", "*.png *.jpg *.jpeg *.webp *.bmp",
                state->output_path, "output.png");
            if (!p.empty()) {
                std::snprintf(state->output_path, sizeof(state->output_path), "%s", p.c_str());
                app->save_path = p;
                app->save_explicit = true;
                app->saveResult(p);
            }
        }
    }
    if (ui_button_icon_text(a, ctx, Rect{x + 173.0f, oy2 + 69.0f, 93.0f, 37.0f}, ICON_COPY, "Copy",
                            t.bg_control, 7.0f, 18.0f, 14.0f, true, tone_control_text(t))) {
        if (app) app->copyResult();
    }
    if (ui_button_icon_text(a, ctx, Rect{x + 21.0f, oy2 + 112.0f, 149.0f, 38.0f}, ICON_PLAY_FILLED, "Process all",
                            t.bg_control, 7.0f, 18.0f, 14.0f, true, tone_control_text(t), t.accent)) {
        if (app) app->processAll();
    }
    if (ui_button_icon_text(a, ctx, Rect{x + 182.0f, oy2 + 112.0f, 84.0f, 38.0f}, ICON_TRASH, "Clear",
                            t.bg_control, 7.0f, 18.0f, 14.0f, true, tone_control_text(t))) {
        if (app) app->clearTarget();
    }

    // Pinned footer
    ui_clip(a, ctx, Rect{0, 0, ctx->screen_w, H});
    ui_rect(a, ctx, Rect{x, scroll_view_bottom, w, footer_h}, t.bg_panel);
    ui_separator(a, ctx, x, scroll_view_bottom, w, color_alpha(t.border, 0x90));

    const bool have_target = app && app->base.valid();
    const Color dot = have_target ? t.accent : t.text_muted;
    ui_rect(a, ctx, Rect{x + 20.0f, scroll_view_bottom + 16.0f, 16.0f, 16.0f}, dot, 8.0f);
    if (have_target) {
        ui_icon(a, ctx, x + 22.0f, scroll_view_bottom + 18.0f, ICON_CHECK, t.on_accent, 12.0f);
    } else {
        ui_rect(a, ctx, Rect{x + 26.5f, scroll_view_bottom + 19.5f, 3.0f, 3.0f}, t.on_accent, 1.5f);
        ui_rect(a, ctx, Rect{x + 26.5f, scroll_view_bottom + 24.0f, 3.0f, 5.5f}, t.on_accent, 1.5f);
    }
    if (have_target) {
        ui_text(a, ctx, x + 44.0f, scroll_view_bottom + 16.0f,
                ui_text_ellipsized(a, app ? app->target_path.c_str() : "", 13.5f, 2, w - 64.0f),
                t.text_muted, 13.5f);
    } else {
        ui_text(a, ctx, x + 44.0f, scroll_view_bottom + 16.0f, "Load a target image first", t.text_muted, 13.5f);
    }
}

void photo_center_panel(Arena* a, UIContext* ctx, UIState* state, const Layout& L) {
    const Theme& t = *ctx->theme;
    app::App* app = app_of(state);
    const float dx = L.canvas_x - (L.left_x + L.left_w + 10.0f);
    const float row_right = L.canvas_x + L.canvas_w;
    auto row_fits = [&](float px, float pw) { return px + pw <= row_right + 0.5f; };

    const float top_ctrl_y = 144.0f;
    ui_checkbox(a, ctx, L.canvas_x + 1.0f, top_ctrl_y + 6.0f, "Overlay", &state->overlay, &t);
    if (row_fits(L.canvas_x + 100.0f, 92.0f)) {
        ui_checkbox(a, ctx, L.canvas_x + 100.0f, top_ctrl_y + 6.0f, "Original", &state->original, &t);
    }

    if (row_fits(L.canvas_x + 200.0f, 39.0f)) {
        const Rect fit{L.canvas_x + 200.0f, top_ctrl_y, 39.0f, 34.0f};
        if (ui_button_icon_text(a, ctx, fit, 0, "Fit", t.bg_control, 6.0f, 0.0f, 13.0f, true, tone_control_text(t))) {
            state->fit = true;
            state->zoom = 1.0f;
        }
    }
    if (row_fits(L.canvas_x + 245.0f, 40.0f)) {
        const Rect one{L.canvas_x + 245.0f, top_ctrl_y, 40.0f, 34.0f};
        if (ui_button_icon_text(a, ctx, one, 0, "1:1", color_alpha(t.bg_control, 0x80), 6.0f, 0.0f, 13.0f, true, color_mix(t.text_muted, t.text, 0.6f))) {
            state->fit = false;
            state->zoom = 1.0f;
        }
    }
    if (row_fits(L.canvas_x + 300.0f, 70.0f)) {
        ui_text(a, ctx, L.canvas_x + 300.0f, top_ctrl_y + 8.0f,
                arena_fmt(a, "zoom %d%%", static_cast<int>(state->zoom * 100.0f)),
                t.text, 13.9f, 2);
    }
    if (L.canvas_w >= 850.0f) {
        ui_text(a, ctx, L.canvas_x + 400.0f + dx, top_ctrl_y + 9.0f,
                "click face = toggle \xC2\xB7 right-click = menu \xC2\xB7 wheel = zoom",
                t.text_dim, 12.5f);
    }

    const Rect canvas{L.canvas_x, L.canvas_y, L.canvas_w, L.canvas_h};
    ui_dashed_rect(a, ctx, canvas, color_alpha(t.border, 0xd0), 5.0f, 4.0f);

    const uint64_t canvas_menu_id = ui_hash_str("canvas_ctx_menu");
    ui_context_menu_area(ctx, canvas_menu_id, canvas);

    static const MenuItem kCanvasMenuItems[] = {
        { .label = "Paste target", .icon = ICON_CLIPBOARD, .shortcut = "Ctrl+V" },
        { .label = "Open target...", .icon = ICON_IMAGE_PLUS, .shortcut = "Ctrl+O" },
        { .separator = true },
        { .label = "Reset View / Fit", .icon = ICON_ROTATE_CCW },
        { .label = "1:1 Pixel View", .icon = ICON_MONITOR },
        { .separator = true },
        { .label = "Clear Canvas", .icon = ICON_TRASH }
    };
    const int menu_pick = ui_context_menu(a, ctx, canvas_menu_id, kCanvasMenuItems, 7, &t);
    if (menu_pick == 1) {
        if (app) {
            const std::string p = platform::open_file_dialog(
                "Open target image", "*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff",
                app->target_path.c_str());
            if (!p.empty()) {
                app->openTarget(p);
                state->sync_output_path = true;
            }
        }
    } else if (menu_pick == 3) {
        state->fit = true;
        state->zoom = 1.0f;
    } else if (menu_pick == 4) {
        state->fit = false;
        state->zoom = 1.0f;
    } else if (menu_pick == 6) {
        if (app) app->clearTarget();
    }

    // Image canvas. The composed result is shown by default; "Original" swaps in
    // the pristine base, and the base is also used as a fallback until the first
    // job produces a result. The rect is sized from the base image so the face
    // overlay (stored in base pixels) maps correctly even at preview resolution.
    const bool have_base = app && app->base.valid();
    const bool show_original = state->original || state->result_tex == 0;
    const uint64_t tex = show_original ? state->base_tex : state->result_tex;
    if (tex != 0 && have_base) {
        const float ref_w = static_cast<float>(app->base.width);
        const float ref_h = static_cast<float>(app->base.height);
        const float pad = 8.0f;
        const float avail_w = canvas.w - pad * 2.0f;
        const float avail_h = canvas.h - pad * 2.0f;
        float scale = std::min(avail_w / ref_w, avail_h / ref_h);
        if (!state->fit) scale = state->zoom;
        const float iw = ref_w * scale;
        const float ih = ref_h * scale;
        const Rect dst{canvas.x + (canvas.w - iw) * 0.5f,
                       canvas.y + (canvas.h - ih) * 0.5f, iw, ih};
        ui_image(a, ctx, dst, tex, Color{0xff, 0xff, 0xff, 0xff});
        ui_rect_border(a, ctx, dst, t.border, 4.0f, 1.0f);

        // Face overlay + click-to-toggle. Faces are stored in base pixels.
        if (state->overlay && !show_original && app) {
            app::FacesSnapshot snap = app->facesSnapshot();
            for (int i = 0; i < snap.count; ++i) {
                const float bx = dst.x + snap.faces[i].bbox[0] * scale;
                const float by = dst.y + snap.faces[i].bbox[1] * scale;
                const float bw = (snap.faces[i].bbox[2] - snap.faces[i].bbox[0]) * scale;
                const float bh = (snap.faces[i].bbox[3] - snap.faces[i].bbox[1]) * scale;
                const Rect fr{bx, by, bw, bh};
                const Color box_col = snap.sel[i] ? Color{0x34, 0xd3, 0x99, 0xff}
                                                  : Color{0x8b, 0x95, 0xa5, 0xff};
                ui_rect_border(a, ctx, fr, box_col, 3.0f, 2.0f);
                const uint64_t fid = ui_hash_int(static_cast<uint64_t>(i), 0xf00d);
                const UIInteraction fin = ui_process_interaction(ctx, fid, fr);
                if (fin.clicked) app->toggleFace(i);
                const Rect badge{fr.x, fr.y - 18.0f, 66.0f, 16.0f};
                ui_rect(a, ctx, badge, Color{0x0f, 0x17, 0x2a, 0xe0}, 3.0f);
                ui_text(a, ctx, badge.x + 6.0f, badge.y + 2.0f,
                        arena_fmt(a, "Face %d", i + 1), box_col, 10.5f, 2);
            }
        }
        return;
    }

    // Hero Empty-State
    const float cx = canvas.x + canvas.w * 0.5f;
    float k = 1.0f;
    if (canvas.h < 300.0f) k = std::min(k, canvas.h / 300.0f);
    if (canvas.w < 520.0f) k = std::min(k, canvas.w / 520.0f);
    k = std::clamp(k, 0.4f, 1.0f);

    const float icon_size = 98.0f * k;
    const float group_h = 164.0f * k;
    float group_y = canvas.y + canvas.h * 0.5f - 124.5f * k;
    const float group_lo = canvas.y + 4.0f;
    const float group_hi = canvas.y + canvas.h - group_h - 4.0f;
    if (group_y < group_lo) group_y = group_lo;
    if (group_hi >= group_lo && group_y > group_hi) group_y = group_hi;

    ui_icon(a, ctx, cx - 48.0f * k, group_y, ICON_IMAGE_HERO,
            Color{0x92, 0x98, 0xa7, 0xff}, icon_size);

    const float badge = 49.0f * k;
    const float badge_x = cx + 18.5f * k;
    const float badge_y = group_y + 56.5f * k;
    ui_rect(a, ctx, Rect{badge_x, badge_y, badge, badge}, t.accent, badge * 0.5f);
    ui_rect(a, ctx, Rect{badge_x + 14.25f * k, badge_y + 22.5f * k, 21.0f * k, 4.0f * k}, t.on_accent, 2.0f * k);
    ui_rect(a, ctx, Rect{badge_x + 22.75f * k, badge_y + 14.0f * k, 4.0f * k, 21.0f * k}, t.on_accent, 2.0f * k);

    const float caption_size = std::max(13.0f, 19.0f * k);
    const float caption_y = group_y + 133.5f * k;
    if (caption_y + font_line_height(caption_size) <= canvas.y + canvas.h - 2.0f) {
        const char* drop = L.canvas_w >= 850.0f
                               ? "Drop an image, paste from the clipboard, or open a file to begin."
                               : "Drop an image to begin.";
        const float drop_w = text_width(drop, caption_size);
        ui_text(a, ctx, cx - drop_w * 0.5f, caption_y, drop, t.text, caption_size);
    }
}

// Builds the combo item labels for a face row: Default, each identity, Skip.
void face_combo_items(Arena* a, const app::IdentitiesSnapshot& ids,
                      std::vector<const char*>& items, std::vector<std::string>& storage) {
    storage.clear();
    items.clear();
    storage.reserve(static_cast<size_t>(ids.count) + 2);
    storage.push_back("Default");
    items.push_back(storage.back().c_str());
    for (int i = 0; i < ids.count; ++i) {
        storage.push_back(ids.labels[i]);
        items.push_back(storage.back().c_str());
    }
    storage.push_back("Skip face");
    items.push_back(storage.back().c_str());
    (void)a;
}

void photo_right_sidebar(Arena* a, UIContext* ctx, UIState* state, const Layout& L) {
    if (!L.show_right) return;
    const Theme& t = *ctx->theme;
    const float H = ctx->screen_h;
    const float rx = L.right_x;
    const float w = L.right_w;

    app::App* app = app_of(state);
    app::FacesSnapshot snap;
    app::IdentitiesSnapshot ids;
    if (app) {
        snap = app->facesSnapshot();
        ids = app->identitiesSnapshot();
    }

    const float stats_h = 225.0f;
    const float stats_y = H - L.status_h - 16.0f - stats_h;
    const float card_y = 124.0f;
    const float card_h = std::max(180.0f, stats_y - 11.0f - card_y);
    const float inner_y = card_y + 110.0f;
    const float inner_h = std::max(80.0f, card_y + card_h - 2.0f - inner_y);

    ui_rect(a, ctx, Rect{rx, card_y, w, card_h}, t.bg_panel, 10.0f);
    ui_rect(a, ctx, Rect{rx + 4.0f, inner_y, w - 8.0f, inner_h}, tone_faces_card(t), 8.0f);
    ui_rect_border(a, ctx, Rect{rx, card_y, w, card_h}, t.border, 10.0f, 1.0f);

    ui_icon(a, ctx, rx + 17.0f, card_y + 17.0f, ICON_SCAN_FACE_FILLED, t.accent, 22.0f);
    ui_text_bold(a, ctx, rx + 38.0f, card_y + 15.0f, "Detected faces", t.text, 17.5f);

    if (pill(a, ctx, t, Rect{rx + 15.0f, card_y + 60.0f, 70.0f, 37.0f}, "All",
             snap.count > 0 && [&] {
                 for (int i = 0; i < snap.count; ++i) if (!snap.sel[i]) return false;
                 return true;
             }())) {
        if (app) for (int i = 0; i < snap.count; ++i) app->setFaceSelected(i, true);
    }
    if (pill(a, ctx, t, Rect{rx + 93.0f, card_y + 60.0f, 70.0f, 37.0f}, "None",
             snap.count > 0 && [&] {
                 for (int i = 0; i < snap.count; ++i) if (snap.sel[i]) return false;
                 return true;
             }())) {
        if (app) for (int i = 0; i < snap.count; ++i) app->setFaceSelected(i, false);
    }
    ui_text_right(a, ctx, Rect{rx + 173.0f, card_y + 60.0f, 67.0f, 37.0f},
                  arena_fmt(a, "%d found", snap.count), t.text, 14.2f, 1);

    // Face list (scroll is approximated by clipping the inner card).
    ui_clip(a, ctx, Rect{rx + 4.0f, inner_y, w - 8.0f, inner_h});
    if (snap.count == 0) {
        const float list_cy = inner_y + inner_h * 0.5f;
        ui_icon_center(a, ctx, Rect{rx + 101.0f, list_cy - 59.0f, 58.0f, 58.0f},
                       ICON_SCAN_PERSON, Color{0x8b, 0x95, 0xa5, 0xff}, 58.0f);
        ui_text_center(a, ctx, Rect{rx, list_cy + 8.0f, w, 26.0f}, "No faces detected yet.",
                       t.text_muted, 14.3f);
    } else {
        std::vector<const char*> items;
        std::vector<std::string> storage;
        face_combo_items(a, ids, items, storage);
        float ry = inner_y + 8.0f;
        const float row_h = 52.0f;
        for (int i = 0; i < snap.count && ry + row_h <= inner_y + inner_h; ++i) {
            const Rect row{rx + 8.0f, ry, w - 16.0f, row_h - 6.0f};
            const uint64_t rid = ui_hash_int(static_cast<uint64_t>(i), 0xface);
            const UIInteraction rin = ui_process_interaction(ctx, rid, row);
            const Color rbg = snap.sel[i] ? color_alpha(t.accent, 0x30)
                                          : (rin.hovered ? t.bg_control_hover : Color{0, 0, 0, 0});
            if (rbg.a > 0) ui_rect(a, ctx, row, rbg, 6.0f);

            const Rect th{row.x + 3.0f, row.y + 3.0f, 38.0f, 38.0f};
            if (state->face_tex[i] != 0) {
                ui_image(a, ctx, th, state->face_tex[i], Color{0xff, 0xff, 0xff, 0xff});
            } else {
                ui_rect(a, ctx, th, t.bg_raised, 4.0f);
                ui_icon_center(a, ctx, th, ICON_USER_FILLED, t.text_muted, 16.0f);
            }
            ui_rect_border(a, ctx, th, snap.sel[i] ? t.accent : t.border, 4.0f,
                           snap.sel[i] ? 2.0f : 1.0f);
            ui_text_bold(a, ctx, row.x + 48.0f, row.y + 5.0f,
                         arena_fmt(a, "Face %d", i + 1), t.text, 12.5f);

            const Rect cr{row.x + 92.0f, row.y + 9.0f, row.w - 96.0f, 26.0f};
            int sel_idx;
            if (!snap.sel[i]) sel_idx = ids.count + 1;
            else if (snap.identity[i] >= 0 && snap.identity[i] < ids.count) sel_idx = snap.identity[i] + 1;
            else sel_idx = 0;
            if (ui_combo(a, ctx, cr, items.data(), static_cast<int>(items.size()), &sel_idx, &t,
                         ui_hash_int(static_cast<uint64_t>(i), 0xc0b0))) {
                if (app) {
                    if (sel_idx == ids.count + 1) {
                        app->setFaceSelected(i, false);
                    } else if (sel_idx == 0) {
                        app->setFaceSelected(i, true);
                        app->setFaceIdentity(i, -1);
                    } else {
                        app->setFaceSelected(i, true);
                        app->setFaceIdentity(i, sel_idx - 1);
                    }
                }
            }

            // Clicking the row body (not the combo/thumb border) toggles it.
            if (rin.clicked && !ui_hovered(ctx, cr) && app) app->toggleFace(i);
            ry += row_h;
        }
    }
    // Restore full-screen clip for the statistics card below.
    ui_clip(a, ctx, Rect{0, 0, ctx->screen_w, ctx->screen_h});

    // Statistics
    ui_panel(a, ctx, Rect{rx, stats_y, w, stats_h}, t.bg_panel, t.border, 10.0f);
    ui_icon(a, ctx, rx + 16.0f, stats_y + 14.0f, ICON_CHART_BARS, t.accent, 27.0f);
    ui_text_bold(a, ctx, rx + 48.0f, stats_y + 18.0f, "Statistics", t.text, 15.0f);

    const char* labels[] = {"detect", "embed", "swap", "enhance", "total"};
    const app::Timings tm = app ? app->lastTimings() : app::Timings{};
    const double vals[] = {tm.detect_ms, tm.embed_ms, tm.swap_ms, tm.enhance_ms, tm.total_ms};
    const Color stat_label = color_mix(t.text, t.text_muted, 0.35f);
    for (int i = 0; i < 5; ++i) {
        const float y = stats_y + 51.0f + static_cast<float>(i) * 25.0f;
        ui_text_in_rect(a, ctx, Rect{rx + 19.0f, y, 160.0f, 22.0f}, labels[i],
                        stat_label, 13.5f, 2);
        ui_text_right(a, ctx, Rect{rx + 17.0f, y, 160.0f, 22.0f},
                      arena_fmt(a, "%.1f ms", vals[i]), t.text, 13.5f, 2);
    }

    ui_separator(a, ctx, rx + 17.0f, stats_y + 181.0f, 160.0f, color_alpha(t.border, 0xa0));
    ui_text_bold(a, ctx, rx + 17.0f, stats_y + 185.0f,
                 (app && app->fast_preview) ? "fast preview" : "full resolution",
                 t.accent, 14.1f);
}

// ---------------------------------------------------------------------------
// 5. Video Mode: Left Sidebar, Center Canvas & Scrubber, Right Sidebar
// ---------------------------------------------------------------------------
void video_left_sidebar(Arena* a, UIContext* ctx, UIState* state, const Layout& L) {
    const Theme& t = *ctx->theme;
    const float H = ctx->screen_h;
    const float x = L.left_x;
    const float w = L.left_w;
    const float view_y = 124.0f;
    const float view_bottom = H - L.status_h;

    ui_rect(a, ctx, Rect{x, view_y, w, view_bottom - view_y}, t.bg_panel);
    ui_separator(a, ctx, x + w - 0.5f, view_y, 1.0f, t.border);

    const float content_h = 920.0f;
    const float max_scroll = std::max(0.0f, content_h - (view_bottom - view_y));
    state->video_left_scroll = std::clamp(state->video_left_scroll, 0.0f, max_scroll);
    const float oy = view_y + 8.0f - state->video_left_scroll;

    ui_clip(a, ctx, Rect{x, view_y, w, view_bottom - view_y});

    const Rect file_card{x + 10.0f, oy, w - 20.0f, 72.0f};
    ui_rect(a, ctx, file_card, t.bg_raised, 6.0f);
    ui_rect_border(a, ctx, file_card, t.border, 6.0f, 1.0f);

    ui_text_bold(a, ctx, file_card.x + 10.0f, file_card.y + 8.0f,
                 "No video loaded", t.text, 13.0f);
    ui_button_icon_text(a, ctx, Rect{file_card.x + file_card.w - 52.0f, file_card.y + 6.0f, 44.0f, 22.0f},
                        0, "Close", t.bg_control, 4.0f, 0.0f, 11.5f, false, t.text_muted);
    ui_text(a, ctx, file_card.x + 10.0f, file_card.y + 29.0f,
            "Video workspace is not wired yet.", t.text_muted, 12.0f);
    ui_text(a, ctx, file_card.x + 10.0f, file_card.y + 48.0f,
            "Photo mode is fully functional.", t.text_dim, 12.0f);

    float cur_y = oy + 84.0f;

    section_header(a, ctx, t, x, cur_y, ICON_CHART_BARS, "Analysis range");
    cur_y += 32.0f;

    ui_text(a, ctx, x + 16.0f, cur_y + 4.0f,
            arena_fmt(a, "in %d \xC2\xB7 out %d", state->in_frame, state->out_frame),
            t.text, 13.5f, 2);

    if (ui_button_icon_text(a, ctx, Rect{x + w - 124.0f, cur_y, 52.0f, 26.0f}, 0, "Set in",
                            t.bg_control, 5.0f, 0.0f, 12.0f, false, tone_control_text(t))) {
        state->in_frame = state->current_frame;
    }
    if (ui_button_icon_text(a, ctx, Rect{x + w - 66.0f, cur_y, 56.0f, 26.0f}, 0, "Set out",
                            t.bg_control, 5.0f, 0.0f, 12.0f, false, tone_control_text(t))) {
        state->out_frame = state->current_frame;
    }
    cur_y += 32.0f;

    ui_text(a, ctx, x + 16.0f, cur_y, "Chunk 300 \xC2\xB7 Sample every 1", t.text_muted, 12.0f);
    cur_y += 18.0f;
    ui_progress(a, ctx, Rect{x + 14.0f, cur_y, w - 28.0f, 6.0f}, 0.0f, t.bg_control, t.accent);
    cur_y += 14.0f;

    ui_button_icon_text(a, ctx, Rect{x + 12.0f, cur_y, 110.0f, 30.0f}, 0, "Analyze range",
                        t.accent_dim, 6.0f, 0.0f, 12.5f, true, t.on_accent);
    ui_button_icon_text(a, ctx, Rect{x + 128.0f, cur_y, 64.0f, 30.0f}, 0, "Cancel",
                        t.bg_control, 6.0f, 0.0f, 12.0f, false, tone_control_text(t));
    ui_button_icon_text(a, ctx, Rect{x + 198.0f, cur_y, 56.0f, 30.0f}, 0, "Clear",
                        t.bg_control, 6.0f, 0.0f, 12.0f, false, tone_control_text(t));
    cur_y += 36.0f;
    ui_text(a, ctx, x + 16.0f, cur_y, "No analysis yet", t.text_dim, 12.0f);
    cur_y += 24.0f;

    ui_separator(a, ctx, x + 14.0f, cur_y, w - 28.0f, color_alpha(t.border, 0x80));
    cur_y += 12.0f;

    section_header(a, ctx, t, x, cur_y, ICON_HISTORY_FILLED, "Sequences");
    cur_y += 28.0f;
    ui_text(a, ctx, x + 16.0f, cur_y, "Grouped by face count and position.", t.text_muted, 12.0f);
    cur_y += 20.0f;

    ui_separator(a, ctx, x + 14.0f, cur_y, w - 28.0f, color_alpha(t.border, 0x80));
    cur_y += 12.0f;

    section_header(a, ctx, t, x, cur_y, ICON_USER_FILLED, "Assign identities");
    cur_y += 28.0f;
    ui_text(a, ctx, x + 16.0f, cur_y, "Load a video to analyze faces.", t.text_muted, 12.0f);
    cur_y += 22.0f;

    ui_separator(a, ctx, x + 14.0f, cur_y, w - 28.0f, color_alpha(t.border, 0x80));
    cur_y += 12.0f;

    section_header(a, ctx, t, x, cur_y, ICON_BOX_FILLED, "Studio (per-frame retouch)");
    cur_y += 30.0f;
    ui_checkbox(a, ctx, x + 16.0f, cur_y, "Paint retouch on this frame", &state->vid_paint_retouch, &t);
    cur_y += 36.0f;

    ui_separator(a, ctx, x + 14.0f, cur_y, w - 28.0f, color_alpha(t.border, 0x80));
    cur_y += 12.0f;

    section_header(a, ctx, t, x, cur_y, ICON_SLIDERS_FILLED, "Process & cache");
    cur_y += 30.0f;

    ui_checkbox(a, ctx, x + 16.0f, cur_y, "Low quality", &state->vid_low_quality, &t);
    ui_checkbox(a, ctx, x + 150.0f, cur_y, "Use cache", &state->vid_use_cache, &t);
    cur_y += 26.0f;
    ui_checkbox(a, ctx, x + 16.0f, cur_y, "Restore mouths", &state->vid_restore_mouths, &t);
    ui_checkbox(a, ctx, x + 150.0f, cur_y, "Skip unassigned", &state->vid_skip_unassigned, &t);
    cur_y += 30.0f;

    ui_text(a, ctx, x + 16.0f, cur_y, "Fast 640 px pass \xC2\xB7 enhancer off", t.text_dim, 12.0f);
    cur_y += 20.0f;

    const float btn2_w = (w - 28.0f) * 0.5f;
    ui_button_icon_text(a, ctx, Rect{x + 12.0f, cur_y, btn2_w, 32.0f}, 0, "Process frame",
                        t.bg_control, 6.0f, 0.0f, 12.5f, true, tone_control_text(t));
    ui_button_icon_text(a, ctx, Rect{x + 16.0f + btn2_w, cur_y, btn2_w, 32.0f}, 0, "Process sequence",
                        t.bg_control, 6.0f, 0.0f, 12.5f, true, tone_control_text(t));
    cur_y += 38.0f;

    ui_button_icon_text(a, ctx, Rect{x + 12.0f, cur_y, w - 24.0f, 36.0f}, ICON_PLAY_FILLED, "Process current chunk",
                        t.bg_control_hover, 6.0f, 16.0f, 13.0f, true, tone_control_text(t));
    cur_y += 44.0f;

    ui_separator(a, ctx, x + 14.0f, cur_y, w - 28.0f, color_alpha(t.border, 0x80));
    cur_y += 12.0f;

    section_header(a, ctx, t, x, cur_y, ICON_SAVE, "Export");
    cur_y += 30.0f;
    ui_checkbox(a, ctx, x + 16.0f, cur_y, "Audio", &state->vid_audio, &t);
    ui_checkbox(a, ctx, x + 120.0f, cur_y, "Split chunks", &state->vid_split_chunks, &t);
    cur_y += 30.0f;

    ui_button_icon_text(a, ctx, Rect{x + 12.0f, cur_y, btn2_w, 34.0f}, 0, "Export range",
                        t.bg_control, 6.0f, 0.0f, 13.0f, true, tone_control_text(t));
    ui_button_icon_text(a, ctx, Rect{x + 16.0f + btn2_w, cur_y, btn2_w, 34.0f}, 0, "Export all",
                        t.bg_control, 6.0f, 0.0f, 13.0f, true, tone_control_text(t));
    cur_y += 40.0f;

    ui_text(a, ctx, x + 16.0f, cur_y, "Export always runs at the source resolution.", t.text_dim, 11.5f);

    ui_clip(a, ctx, Rect{0, 0, ctx->screen_w, ctx->screen_h});
}

void video_center_panel(Arena* a, UIContext* ctx, UIState* state, const Layout& L) {
    const Theme& t = *ctx->theme;
    const float x = L.canvas_x;
    const float y = 124.0f;
    const float w = L.canvas_w;
    const float h = L.h - L.status_h - y;

    ui_rect(a, ctx, Rect{x, y, w, h}, Color{0x0e, 0x10, 0x14, 0xff}, 8.0f);
    ui_rect_border(a, ctx, Rect{x, y, w, h}, t.border, 8.0f, 1.0f);

    const float transport_h = 82.0f;
    const float view_h = h - transport_h;

    const float frame_h = std::min(view_h - 20.0f, 620.0f);
    const float frame_w = frame_h * 0.72f;
    const float frame_x = x + (w - frame_w) * 0.5f;
    const float frame_y = y + (view_h - frame_h) * 0.5f;

    ui_rect(a, ctx, Rect{frame_x, frame_y, frame_w, frame_h}, Color{0x1e, 0x22, 0x2b, 0xff}, 4.0f);
    ui_icon_center(a, ctx, Rect{frame_x, frame_y, frame_w, frame_h}, ICON_PLAY,
                   Color{0x3a, 0x40, 0x4c, 0xff}, 64.0f);

    const Rect trans_r{x, y + view_h, w, transport_h};
    ui_rect(a, ctx, trans_r, t.bg_raised, 0.0f);
    ui_separator(a, ctx, trans_r.x, trans_r.y, trans_r.w, t.border);

    const float row1_y = trans_r.y + 8.0f;
    const float btn_w = 34.0f;
    const float btn_h = 28.0f;

    const bool compact = w < 240.0f;
    const char* play_lbl = state->is_playing ? "Pause" : "Play";
    const uint32_t play_icon = state->is_playing ? 0u : static_cast<uint32_t>(ICON_PLAY_FILLED);
    const Color play_bg = state->is_playing ? t.accent_dim : t.bg_control;
    const Color play_fg = state->is_playing ? t.on_accent : tone_control_text(t);
    if (compact) {
        const float pw = 74.0f;
        if (ui_button_icon_text(a, ctx, Rect{x + (w - pw) * 0.5f, row1_y, pw, btn_h},
                                play_icon, play_lbl, play_bg, 5.0f, 14.0f, 12.5f, true, play_fg)) {
            state->is_playing = !state->is_playing;
        }
    } else {
        if (ui_button_icon_text(a, ctx, Rect{x + 14.0f, row1_y, btn_w, btn_h}, 0, "|<",
                                t.bg_control, 5.0f, 0.0f, 12.0f, true, tone_control_text(t))) {
            state->current_frame = state->in_frame;
        }
        if (ui_button_icon_text(a, ctx, Rect{x + 52.0f, row1_y, btn_w, btn_h}, 0, "<",
                                t.bg_control, 5.0f, 0.0f, 12.0f, true, tone_control_text(t))) {
            state->current_frame = std::max(0, state->current_frame - 1);
        }
        if (ui_button_icon_text(a, ctx, Rect{x + 90.0f, row1_y, 62.0f, btn_h}, play_icon,
                                play_lbl, play_bg, 5.0f, 14.0f, 12.5f, true, play_fg)) {
            state->is_playing = !state->is_playing;
        }
        if (ui_button_icon_text(a, ctx, Rect{x + 156.0f, row1_y, btn_w, btn_h}, 0, ">",
                                t.bg_control, 5.0f, 0.0f, 12.0f, true, tone_control_text(t))) {
            state->current_frame = std::min(state->total_frames - 1, state->current_frame + 1);
        }
        if (ui_button_icon_text(a, ctx, Rect{x + 194.0f, row1_y, btn_w, btn_h}, 0, ">|",
                                t.bg_control, 5.0f, 0.0f, 12.0f, true, tone_control_text(t))) {
            state->current_frame = state->out_frame;
        }
    }

    const bool show_timecode = w >= 520.0f;
    const bool show_all_boxes = w >= 810.0f;
    const bool show_some_boxes = w >= 640.0f;
    if (show_some_boxes) {
        float cb_x = x + 242.0f;
        ui_checkbox(a, ctx, cb_x, row1_y + 3.0f, "Sequence", &state->opt_sequence, &t);
        cb_x += 88.0f;
        ui_checkbox(a, ctx, cb_x, row1_y + 3.0f, "Loop", &state->opt_loop, &t);
        if (show_all_boxes) {
            cb_x += 66.0f;
            ui_checkbox(a, ctx, cb_x, row1_y + 3.0f, "Original", &state->opt_original, &t);
            cb_x += 80.0f;
            ui_checkbox(a, ctx, cb_x, row1_y + 3.0f, "Boxes", &state->opt_boxes, &t);
        }
    }

    if (show_timecode) {
        const float cur_sec = static_cast<float>(state->current_frame) / state->fps;
        const int tc_m = static_cast<int>(cur_sec) / 60;
        const int tc_s = static_cast<int>(cur_sec) % 60;
        const int tc_ms = static_cast<int>((cur_sec - static_cast<int>(cur_sec)) * 1000.0f);
        ui_text_right(a, ctx, Rect{x + w - 260.0f, row1_y + 4.0f, 246.0f, 20.0f},
                      arena_fmt(a, "+%d:+%02d.+%03d \xC2\xB7 frame %03d \xC2\xB7 000/001",
                                tc_m, tc_s, tc_ms, state->current_frame),
                      t.text, 12.5f, 1);
    }

    const Rect track_r{x + 14.0f, trans_r.y + 40.0f, w - 28.0f, 18.0f};
    ui_rect(a, ctx, track_r, Color{0x14, 0x16, 0x1c, 0xff}, 4.0f);
    ui_rect_border(a, ctx, track_r, t.border, 4.0f, 1.0f);

    const float in_x = track_r.x + (static_cast<float>(state->in_frame) / state->total_frames) * track_r.w;
    const float out_x = track_r.x + (static_cast<float>(state->out_frame) / state->total_frames) * track_r.w;
    const float cache_w = std::max(2.0f, out_x - in_x);
    ui_rect(a, ctx, Rect{in_x, track_r.y + 3.0f, cache_w, 5.0f}, Color{0x2b, 0x5b, 0xa3, 0xff}, 2.0f);
    ui_rect(a, ctx, Rect{in_x, track_r.y + 10.0f, cache_w, 5.0f}, Color{0x23, 0x4a, 0x85, 0xff}, 2.0f);

    ui_rect(a, ctx, Rect{in_x, track_r.y, 2.0f, track_r.h}, Color{0x38, 0xbd, 0xf8, 0xff});
    ui_rect(a, ctx, Rect{in_x, track_r.y, 6.0f, 2.0f}, Color{0x38, 0xbd, 0xf8, 0xff});
    ui_rect(a, ctx, Rect{in_x, track_r.y + track_r.h - 2.0f, 6.0f, 2.0f}, Color{0x38, 0xbd, 0xf8, 0xff});

    ui_rect(a, ctx, Rect{out_x - 1.0f, track_r.y, 2.0f, track_r.h}, Color{0xfb, 0x92, 0x3c, 0xff});
    ui_rect(a, ctx, Rect{out_x - 5.0f, track_r.y, 6.0f, 2.0f}, Color{0xfb, 0x92, 0x3c, 0xff});
    ui_rect(a, ctx, Rect{out_x - 5.0f, track_r.y + track_r.h - 2.0f, 6.0f, 2.0f}, Color{0xfb, 0x92, 0x3c, 0xff});

    const float play_x = track_r.x + (static_cast<float>(state->current_frame) / state->total_frames) * track_r.w;
    ui_rect(a, ctx, Rect{play_x - 1.0f, track_r.y - 2.0f, 2.5f, track_r.h + 4.0f}, Color{0xfa, 0xcc, 0x15, 0xff});
    ui_rect(a, ctx, Rect{play_x - 3.5f, track_r.y - 3.0f, 7.0f, 4.0f}, Color{0xfa, 0xcc, 0x15, 0xff}, 1.5f);

    const uint64_t track_id = ui_hash_str("timeline_track");
    const UIInteraction track_in = ui_process_interaction(ctx, track_id, track_r, UIBehavior{ .draggable = true });
    if (track_in.dragging || track_in.clicked) {
        const float norm = (ctx->mouse_x - track_r.x) / track_r.w;
        state->current_frame = std::clamp(static_cast<int>(norm * state->total_frames), 0, state->total_frames - 1);
    }

    ui_text(a, ctx, x + 16.0f, trans_r.y + 64.0f, "No video loaded", t.text_dim, 11.5f);
}

void video_right_sidebar(Arena* a, UIContext* ctx, UIState* state, const Layout& L) {
    if (!L.show_right) return;
    const Theme& t = *ctx->theme;
    const float rx = L.right_x;
    const float w = L.right_w;
    const float view_y = 124.0f;
    const float view_bottom = ctx->screen_h - L.status_h;

    ui_rect(a, ctx, Rect{rx, view_y, w, view_bottom - view_y}, t.bg_panel);
    ui_separator(a, ctx, rx, view_y, 1.0f, t.border);

    float cur_y = view_y + 12.0f;

    section_header(a, ctx, t, rx, cur_y, ICON_USER_FILLED, "Identity library");
    cur_y += 32.0f;

    const Rect lib_card{rx + 12.0f, cur_y, w - 24.0f, 62.0f};
    ui_rect(a, ctx, lib_card, t.bg_raised, 8.0f);
    ui_rect_border(a, ctx, lib_card, t.border, 8.0f, 1.0f);

    const Rect av_r{lib_card.x + 8.0f, lib_card.y + 8.0f, 46.0f, 46.0f};
    ui_rect(a, ctx, av_r, t.accent_dim, 6.0f);
    ui_icon(a, ctx, av_r.x + 13.0f, av_r.y + 13.0f, ICON_USER_FILLED, t.on_accent, 20.0f);

    ui_text_bold(a, ctx, lib_card.x + 62.0f, lib_card.y + 12.0f, "No video loaded", t.text, 14.0f);
    ui_text(a, ctx, lib_card.x + 62.0f, lib_card.y + 32.0f, "Video workspace pending", t.text_muted, 12.0f, 2);
    cur_y += 74.0f;

    ui_separator(a, ctx, rx + 14.0f, cur_y, w - 28.0f, color_alpha(t.border, 0x80));
    cur_y += 12.0f;

    section_header(a, ctx, t, rx, cur_y, ICON_CLOCK, "Cache");
    cur_y += 30.0f;
    ui_text(a, ctx, rx + 14.0f, cur_y, "No cached frames.", t.text_muted, 12.5f);
    cur_y += 18.0f;
    cur_y += 24.0f;

    ui_button_icon_text(a, ctx, Rect{rx + 12.0f, cur_y, w - 24.0f, 30.0f}, 0, "Forget this frame's cache",
                        t.bg_control, 5.0f, 0.0f, 12.0f, false, tone_control_text(t));
    cur_y += 36.0f;
    ui_button_icon_text(a, ctx, Rect{rx + 12.0f, cur_y, w - 24.0f, 30.0f}, 0, "Save project",
                        t.bg_control, 5.0f, 0.0f, 12.0f, false, tone_control_text(t));
    cur_y += 36.0f;

    ui_text_edit(a, ctx, Rect{rx + 12.0f, cur_y, w - 24.0f, 28.0f}, state->video_path,
                 sizeof(state->video_path), &t, "video path", 0x0f02);
    cur_y += 42.0f;

    ui_separator(a, ctx, rx + 14.0f, cur_y, w - 28.0f, color_alpha(t.border, 0x80));
    cur_y += 12.0f;

    section_header(a, ctx, t, rx, cur_y, ICON_INFO, "Shortcuts");
    cur_y += 30.0f;

    auto shortcut_row = [&](const char* key, const char* desc) {
        ui_rect(a, ctx, Rect{rx + 14.0f, cur_y, 44.0f, 20.0f}, t.bg_raised, 4.0f);
        ui_rect_border(a, ctx, Rect{rx + 14.0f, cur_y, 44.0f, 20.0f}, t.border, 4.0f, 1.0f);
        ui_text_center(a, ctx, Rect{rx + 14.0f, cur_y, 44.0f, 20.0f}, key, t.accent, 11.0f, 1);
        ui_text(a, ctx, rx + 66.0f, cur_y + 2.0f, desc, t.text_muted, 12.0f);
        cur_y += 26.0f;
    };

    shortcut_row("Space", "Play / Pause");
    shortcut_row("< / >", "Step frame");
    shortcut_row("Shift", "Jump 10 frames");
}

// ---------------------------------------------------------------------------
// 6. Status Bar & Size Grip
// ---------------------------------------------------------------------------
void status_bar(Arena* a, UIContext* ctx, UIState* state) {
    const Theme& t = *ctx->theme;
    const float W = ctx->screen_w;
    const float H = ctx->screen_h;
    const float bar_h = 26.0f;
    const Rect bar{0.0f, H - bar_h, W, bar_h};

    ui_rect(a, ctx, bar, t.bg_header);
    ui_separator(a, ctx, 0.0f, bar.y, W, color_alpha(t.border, 0x90));

    const char* mode_badge = state->mode == AppMode::Photo ? "PHOTO MODE" : "VIDEO MODE";
    const float badge_w = text_width(mode_badge, 10.5f, 1) + 16.0f;
    const bool show_badge = W >= 700.0f;
    const bool show_right = W >= 940.0f;

    float msg_max = W - 26.0f - 10.0f;
    if (show_badge) msg_max = std::min(msg_max, (W - badge_w) * 0.5f - 40.0f);
    if (show_right) msg_max = std::min(msg_max, W - 250.0f - 40.0f);

    app::App* app = app_of(state);
    const bool busy = app && app->busy();
    const bool err = app && app->statusIsError();
    const Color dot = busy ? t.accent : (err ? t.danger : Color{0x34, 0xd3, 0x99, 0xff});
    ui_rect(a, ctx, Rect{12.0f, bar.y + 9.0f, 8.0f, 8.0f}, dot, 4.0f);

    const char* msg = app ? app->statusText().c_str() : "Ready";
    if (busy && app) {
        msg = arena_fmt(a, "%s \xC2\xB7 %s \xC2\xB7 %d%%", app->statusText().c_str(),
                        app::stageText(app->stage()), app->progressPermille() / 10);
    }
    ui_text(a, ctx, 26.0f, bar.y + 6.0f,
            ui_text_ellipsized(a, msg, 12.0f, 0, msg_max),
            err ? t.danger : t.text_muted, 12.0f);

    if (show_badge) {
        const Rect badge_r{(W - badge_w) * 0.5f, bar.y + 4.0f, badge_w, 18.0f};
        ui_rect(a, ctx, badge_r, color_alpha(t.bg_control, 0xb0), 4.0f);
        ui_rect_border(a, ctx, badge_r, color_alpha(t.border, 0x70), 4.0f, 1.0f);
        ui_text_center(a, ctx, badge_r, mode_badge, t.accent, 10.5f, 1);
    }

    if (show_right) {
        const char* right = (app && app->base.valid())
                                ? arena_fmt(a, "%dx%d \xC2\xB7 F2 Theme",
                                            app->base.width, app->base.height)
                                : "F2 Theme";
        ui_text(a, ctx, W - 250.0f, bar.y + 5.0f, right, t.text_dim, 11.5f);
    }

    const Rect grip_area{W - 20.0f, H - 20.0f, 20.0f, 20.0f};
    state->size_grip_hovered = ui_hovered(ctx, grip_area);

    const uint64_t grip_id = ui_hash_str("window_size_grip");
    const UIInteraction grip_in = ui_process_interaction(ctx, grip_id, grip_area,
                                                        UIBehavior{ .trigger = UIClickTrigger::OnDown, .draggable = true });
    state->size_grip_drag = grip_in.dragging;

    const Color grip_col = (state->size_grip_hovered || state->size_grip_drag)
                               ? t.accent
                               : color_alpha(t.text_muted, 0x90);

    const float gx = W - 4.0f;
    const float gy = H - 4.0f;
    ui_rect(a, ctx, Rect{gx - 10.0f, gy - 2.0f, 2.0f, 2.0f}, grip_col);
    ui_rect(a, ctx, Rect{gx - 6.0f,  gy - 2.0f, 2.0f, 2.0f}, grip_col);
    ui_rect(a, ctx, Rect{gx - 2.0f,  gy - 2.0f, 2.0f, 2.0f}, grip_col);
    ui_rect(a, ctx, Rect{gx - 6.0f,  gy - 6.0f, 2.0f, 2.0f}, grip_col);
    ui_rect(a, ctx, Rect{gx - 2.0f,  gy - 6.0f, 2.0f, 2.0f}, grip_col);
    ui_rect(a, ctx, Rect{gx - 2.0f,  gy - 10.0f, 2.0f, 2.0f}, grip_col);
}

// ---------------------------------------------------------------------------
// 7. Modals: Settings & About
// ---------------------------------------------------------------------------
void settings_modal(Arena* a, UIContext* ctx, UIState* state) {
    const Theme& t = *ctx->theme;
    const float W = ctx->screen_w;
    const float H = ctx->screen_h;

    ctx->in_popup_layer = true;

    ui_rect(a, ctx, Rect{0, 0, W, H}, Color{0, 0, 0, 0x85});

    const float modal_w = 440.0f;
    const float modal_h = 340.0f;
    const Rect modal{(W - modal_w) * 0.5f, (H - modal_h) * 0.5f, modal_w, modal_h};

    ui_rect(a, ctx, modal, t.bg_panel, 10.0f);
    ui_rect_border(a, ctx, modal, t.border_strong, 10.0f, 1.0f);

    ui_icon(a, ctx, modal.x + 16.0f, modal.y + 16.0f, ICON_SETTINGS_FILLED, t.accent, 20.0f);
    ui_text_bold(a, ctx, modal.x + 44.0f, modal.y + 17.0f, "Settings", t.text, 16.0f);

    if (ui_icon_button(a, ctx, Rect{modal.x + modal.w - 38.0f, modal.y + 12.0f, 28.0f, 28.0f},
                       ICON_X, t.text_muted, t.danger, 6.0f, 16.0f)) {
        state->show_settings = false;
    }

    ui_separator(a, ctx, modal.x + 12.0f, modal.y + 48.0f, modal.w - 24.0f, t.border);

    float row_y = modal.y + 64.0f;

    ui_text(a, ctx, modal.x + 20.0f, row_y + 4.0f, "Application theme", t.text, 14.0f, 2);
    const char* theme_lbl = state->light_theme ? "Light theme" : "Dark theme";
    if (ui_button_icon_text(a, ctx, Rect{modal.x + modal.w - 140.0f, row_y, 120.0f, 32.0f},
                            0, theme_lbl, t.bg_control, 6.0f, 0.0f, 13.0f, true, tone_control_text(t))) {
        state->light_theme = !state->light_theme;
    }
    row_y += 46.0f;

    ui_text(a, ctx, modal.x + 20.0f, row_y + 4.0f, "Compute device", t.text, 14.0f, 2);
    static const char* kDevices[] = {"Auto (GPU first)", "CUDA", "CPU"};
    int dev_idx = state->app ? state->app->provider : 0;
    const Rect dev_combo{modal.x + modal.w - 190.0f, row_y, 170.0f, 32.0f};
    if (ui_combo(a, ctx, dev_combo, kDevices, 3, &dev_idx, &t)) {
        state->provider_index = dev_idx;
        if (state->app) {
            state->app->provider = static_cast<uint8_t>(dev_idx);
            state->app->markConfigDirty();
        }
    }
    row_y += 46.0f;

    ui_text(a, ctx, modal.x + 20.0f, row_y + 4.0f, "Models folder", t.text, 14.0f, 2);
    const UITextEditResult me = ui_text_edit(a, ctx, Rect{modal.x + modal.w - 224.0f, row_y, 204.0f, 32.0f},
                                             state->models_dir, sizeof(state->models_dir), &t,
                                             "models", 0x0f03);
    if (state->app && (me == UITextEditResult::Changed || me == UITextEditResult::Submitted)) {
        state->app->model_dir = state->models_dir;
        state->app->markConfigDirty();
    }
    row_y += 42.0f;

    bool unload = state->app ? state->app->unload_after_run : false;
    ui_checkbox(a, ctx, modal.x + 20.0f, row_y, "Unload models between runs", &unload, &t);
    if (state->app && unload != state->app->unload_after_run) {
        state->app->unload_after_run = unload;
        state->app->markConfigDirty();
    }
    row_y += 30.0f;

    bool check_updates = state->app ? state->app->check_updates : true;
    ui_checkbox(a, ctx, modal.x + 20.0f, row_y, "Check GitHub releases", &check_updates, &t);
    if (state->app && check_updates != state->app->check_updates) {
        state->app->check_updates = check_updates;
        state->app->markConfigDirty();
    }
    row_y += 34.0f;

    if (state->app) {
        ui_text(a, ctx, modal.x + 20.0f, row_y,
                arena_fmt(a, "Active engine: %s",
                          state->app->providerName().empty() ? "not loaded"
                                                             : state->app->providerName().c_str()),
                t.text_muted, 12.5f, 2);
    }

    if (ui_button_icon_text(a, ctx, Rect{modal.x + modal.w - 100.0f, modal.y + modal.h - 44.0f, 82.0f, 32.0f},
                            0, "Done", t.accent, 6.0f, 0.0f, 13.0f, true, t.on_accent)) {
        if (state->app) state->app->saveConfig();
        state->show_settings = false;
    }

    ctx->in_popup_layer = false;
}

void about_modal(Arena* a, UIContext* ctx, UIState* state) {
    const Theme& t = *ctx->theme;
    const float W = ctx->screen_w;
    const float H = ctx->screen_h;

    ctx->in_popup_layer = true;

    ui_rect(a, ctx, Rect{0, 0, W, H}, Color{0, 0, 0, 0x85});

    const float modal_w = 460.0f;
    const float modal_h = 300.0f;
    const Rect modal{(W - modal_w) * 0.5f, (H - modal_h) * 0.5f, modal_w, modal_h};

    ui_rect(a, ctx, modal, t.bg_panel, 10.0f);
    ui_rect_border(a, ctx, modal, t.border_strong, 10.0f, 1.0f);

    if (ui_icon_button(a, ctx, Rect{modal.x + modal.w - 38.0f, modal.y + 12.0f, 28.0f, 28.0f},
                       ICON_X, t.text_muted, t.danger, 6.0f, 16.0f)) {
        state->show_about = false;
    }

    ui_icon_gradient(a, ctx, modal.x + 24.0f, modal.y + 24.0f, ICON_LOGO,
                     Color{0x41, 0x76, 0xee, 0xff}, Color{0xb3, 0x2e, 0xf2, 0xff}, 42.0f);
    ui_text_bold(a, ctx, modal.x + 80.0f, modal.y + 24.0f, "FaceForge", t.text, 18.0f);
    ui_text(a, ctx, modal.x + 80.0f, modal.y + 48.0f, "restore \xC2\xB7 swap \xC2\xB7 enhance",
            t.accent, 13.0f, 2);

    ui_separator(a, ctx, modal.x + 20.0f, modal.y + 82.0f, modal.w - 40.0f, t.border);

    float ty = modal.y + 100.0f;
    ui_text(a, ctx, modal.x + 24.0f, ty, "Native C++ UI on the FaceForge core (C ABI)", t.text, 13.5f);
    ty += 24.0f;
    ui_text(a, ctx, modal.x + 24.0f, ty, "ONNX Runtime/OpenCV pipeline, unchanged core", t.text, 13.5f);
    ty += 24.0f;
    ui_text(a, ctx, modal.x + 24.0f, ty, "Immediate-mode widgets, arena allocated", t.text, 13.5f);
    ty += 24.0f;
    ui_text(a, ctx, modal.x + 24.0f, ty, "https://github.com/vhanla/faceforge", t.accent, 13.0f, 2);

    if (ui_button_icon_text(a, ctx, Rect{modal.x + modal.w - 100.0f, modal.y + modal.h - 44.0f, 82.0f, 32.0f},
                            0, "OK", t.accent, 6.0f, 0.0f, 13.0f, true, t.on_accent)) {
        state->show_about = false;
    }

    ctx->in_popup_layer = false;
}

} // namespace

void faceforge_draw(Arena* frame, UIContext* ui, UIState* state) {
    state->window_action = WindowAction::None;

    if (ui->theme == nullptr) ui->theme = &theme_faceforge();
    if (ui->screen_w <= 0.0f) ui->screen_w = kDesignW;
    if (ui->screen_h <= 0.0f) ui->screen_h = kDesignH;

    // Adopt a new default output path after opening a target, but never while
    // the user is typing in the field.
    if (state->sync_output_path) {
        state->sync_output_path = false;
        if (state->app) {
            std::snprintf(state->output_path, sizeof(state->output_path), "%s",
                          state->app->save_path.c_str());
        }
    }

    const Theme& t = *ui->theme;
    const float W = ui->screen_w;
    const float H = ui->screen_h;
    ui_rect(frame, ui, Rect{0, 0, W, H}, t.bg_app);

    const Layout L = compute_layout(ui, state);

    if (state->mode == AppMode::Video && state->is_playing) {
        state->play_accum += ui->dt * state->fps;
        while (state->play_accum >= 1.0f) {
            state->play_accum -= 1.0f;
            state->current_frame++;
            if (state->current_frame > state->out_frame) {
                if (state->opt_loop) {
                    state->current_frame = state->in_frame;
                } else {
                    state->current_frame = state->out_frame;
                    state->is_playing = false;
                    break;
                }
            }
        }
    }

    if (ui->scroll_delta != 0.0f && ui->mouse_y >= 124.0f && ui->mouse_y < H - L.status_h) {
        if (ui->mouse_x >= L.left_x && ui->mouse_x < L.left_x + L.left_w) {
            if (state->mode == AppMode::Photo) {
                state->left_scroll -= ui->scroll_delta * 42.0f;
            } else {
                state->video_left_scroll -= ui->scroll_delta * 42.0f;
            }
        }
    }

    nav_sidebar(frame, ui, state, L.rail_w);
    header(frame, ui, state);
    toolbar(frame, ui, state, L.rail_w);

    if (state->mode == AppMode::Photo) {
        photo_left_sidebar(frame, ui, state, L);
        photo_center_panel(frame, ui, state, L);
        photo_right_sidebar(frame, ui, state, L);
    } else {
        video_left_sidebar(frame, ui, state, L);
        video_center_panel(frame, ui, state, L);
        video_right_sidebar(frame, ui, state, L);
    }

    status_bar(frame, ui, state);

    if (state->show_settings) settings_modal(frame, ui, state);
    if (state->show_about) about_modal(frame, ui, state);

    ui_clip(frame, ui, Rect{0, 0, W, H});
    ui_rect_rounded_inverse(frame, ui, Rect{0.0f, 0.0f, W, H}, t.bg_window, 6.0f);
    ui_rect_border(frame, ui, Rect{0.0f, 0.0f, W, H},
                   color_lighten(t.border, 4), 6.0f, 1.0f);
}
