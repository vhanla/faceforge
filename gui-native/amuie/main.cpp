#include "backend/opengl3_backend.hpp"
#include "core/arena.hpp"
#include "core/theme.hpp"
#include "core/types.hpp"
#include "demo/faceforge.hpp"
#include "platform/input.hpp"
#include "platform/window.hpp"
#include "widgets/ui_widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kDesignW = 1532;
constexpr int kDesignH = 1027;

struct Options {
    const char* screenshot = nullptr;
    int frames = 3;
    bool light = false;
    bool video = false;
    int width = kDesignW;
    int height = kDesignH;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            o.screenshot = argv[++i];
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            o.frames = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                o.width = w;
                o.height = h;
            }
        } else if (std::strcmp(argv[i], "--light") == 0) {
            o.light = true;
        } else if (std::strcmp(argv[i], "--video") == 0) {
            o.video = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: %s [--screenshot out.ppm] [--frames N] "
                        "[--size WxH] [--light] [--video]\n", argv[0]);
            std::exit(0);
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_args(argc, argv);

    platform::WindowDesc desc;
    desc.width = options.width;
    desc.height = options.height;
    desc.title = "MUIE Window";
    desc.undecorated = true;
    desc.transparent = false;
    desc.resizable = true;
    desc.visible = true;
    desc.center = true;

    platform::Window* window = platform::create(desc);
    if (!window) return 1;

    std::printf("platform: %s\n", platform::platform_name());

    Arena frame_arena = Arena::create(8 * 1024 * 1024);
    Arena state_arena = Arena::create(64 * 1024);
    DemoState* demo = state_arena.make<DemoState>();
    if (!frame_arena.buffer || !state_arena.buffer || !demo) {
        std::fprintf(stderr, "arena allocation failed\n");
        platform::destroy(window);
        return 1;
    }
    demo->light_theme = options.light;
    if (options.video) {
        demo->mode = AppMode::Video;
    }

    OpenGL3Backend backend;
    backend.init();

    UIContext ui;
    bool prev_mouse_down = false;
    bool prev_mouse_right_down = false;
    bool prev_f2_down = false;
    bool prev_space_down = false;
    bool prev_esc_down = false;
    double prev_time = platform::time_seconds();
    int frame = 0;

    while (!platform::should_close(window)) {
        platform::poll_events();
        platform::feed_ui_input(window, &ui);

        const bool esc_down = platform::key_pressed(window, platform::Key::Escape);
        if (esc_down && !prev_esc_down) {
            if (ui.active_popup_id != 0) {
                ui.active_popup_id = 0;
                ui.active_popup_rect = Rect{};
            } else if (demo->show_settings || demo->show_about) {
                demo->show_settings = false;
                demo->show_about = false;
            } else {
                platform::set_should_close(window);
            }
        }
        prev_esc_down = esc_down;

        const bool f2_down = platform::key_pressed(window, platform::Key::F2);
        if (f2_down && !prev_f2_down) {
            demo->light_theme = !demo->light_theme;
        }
        prev_f2_down = f2_down;

        const bool space_down = platform::key_pressed(window, platform::Key::Space);
        if (space_down && !prev_space_down) {
            if (demo->mode == AppMode::Video) {
                demo->is_playing = !demo->is_playing;
            }
        }
        prev_space_down = space_down;

        int fb_w = 0, fb_h = 0;
        int win_w = 0, win_h = 0;
        platform::get_framebuffer_size(window, &fb_w, &fb_h);
        platform::get_window_size(window, &win_w, &win_h);
        if (fb_w <= 0 || fb_h <= 0) continue;

        // Responsive model: 1 logical px == 1 window px, and the backend
        // multiplies by the display DPI so HiDPI stays crisp. Layout reflows to
        // the actual window size instead of letterboxing a fixed design.
        const double dpi_x = win_w > 0 ? static_cast<double>(fb_w) / win_w : 1.0;
        const double dpi_y = win_h > 0 ? static_cast<double>(fb_h) / win_h : 1.0;
        const float scale_x = static_cast<float>(dpi_x);
        const float scale_y = static_cast<float>(dpi_y);
        const float logical_w = static_cast<float>(fb_w) / scale_x;
        const float logical_h = static_cast<float>(fb_h) / scale_y;

        double mx = 0.0, my = 0.0;
        platform::get_cursor(window, &mx, &my);
        const double scroll = platform::scroll_poll(window);

        const bool mouse_down = platform::mouse_down(window);
        const bool mouse_clicked = mouse_down && !prev_mouse_down;
        const bool mouse_released = !mouse_down && prev_mouse_down;
        prev_mouse_down = mouse_down;

        const bool mouse_right_down = platform::mouse_right_down(window);
        const bool mouse_right_clicked = mouse_right_down && !prev_mouse_right_down;
        const bool mouse_right_released = !mouse_right_down && prev_mouse_right_down;
        prev_mouse_right_down = mouse_right_down;

        const double now = platform::time_seconds();
        const float dt = static_cast<float>(std::clamp(now - prev_time, 0.0, 0.05));
        prev_time = now;

        // Cursor coords are already in window (logical) pixels. For headless
        // captures, park the cursor offscreen so hover states don't pollute the
        // comparison with the reference.
        if (options.screenshot) {
            ui.mouse_x = -1.0f;
            ui.mouse_y = -1.0f;
        } else {
            ui.mouse_x = static_cast<float>(mx);
            ui.mouse_y = static_cast<float>(my);
        }
        ui.mouse_left_down = mouse_down;
        ui.mouse_left_clicked = mouse_clicked;
        ui.mouse_left_released = mouse_released;
        ui.mouse_right_down = mouse_right_down;
        ui.mouse_right_clicked = mouse_right_clicked;
        ui.mouse_right_released = mouse_right_released;

        ui.theme = demo->light_theme ? &theme_light() : &theme_faceforge();
        ui.time_seconds = static_cast<float>(now);
        ui.dt = dt;
        ui.scroll_delta = static_cast<float>(scroll);
        ui.screen_w = logical_w;
        ui.screen_h = logical_h;
        ui.scale_x = scale_x;
        ui.scale_y = scale_y;
        ui.first_command = nullptr;
        ui.last_command = nullptr;
        ui.popup_first_command = nullptr;
        ui.popup_last_command = nullptr;
        ui.in_popup_layer = false;
        ui.id_stack_depth = 0;
        ui.popup_just_opened = false;

        // If a popup is open and a click occurs outside its rect, dismiss it and consume click
        if (ui.active_popup_id != 0 && (ui.mouse_left_clicked || ui.mouse_right_clicked)) {
            const bool inside = ui_hovered(&ui, ui.active_popup_rect);
            if (!inside) {
                ui.active_popup_id = 0;
                ui.active_popup_rect = Rect{};
                ui.mouse_left_clicked = false;
                ui.mouse_right_clicked = false;
                ui.active_id = UI_UNCLAIMED_ID;
            }
        }

        demo_draw(&frame_arena, &ui, demo);
        ui_end_frame(&frame_arena, &ui);

        // Text widgets may have requested a clipboard write (copy/cut).
        if (ui.input.clipboard_out) {
            platform::clipboard_set_text(window, ui.input.clipboard_out);
        }

        switch (demo->window_action) {
            case WindowAction::Minimize: platform::minimize(window); break;
            case WindowAction::Maximize: platform::toggle_maximize(window); break;
            case WindowAction::Close: platform::set_should_close(window); break;
            case WindowAction::None: break;
        }
        platform::drag_update(window,
            demo->header_drag && mouse_down && demo->window_action == WindowAction::None);
        platform::resize_update(window,
            demo->size_grip_drag && mouse_down && demo->window_action == WindowAction::None);
        platform::set_cursor_resize(window,
            demo->size_grip_hovered || demo->size_grip_drag);

        const Theme& theme = *ui.theme;
        backend.set_clear_color(theme.bg_window);
        backend.begin_frame(fb_w, fb_h);
        backend.set_viewport_2d(0, 0, fb_w, fb_h, scale_x, scale_y);

        for (RenderCommand* cmd = ui.first_command; cmd; cmd = cmd->next) {
            switch (cmd->type) {
                case CMD_DRAW_RECT:
                case CMD_DRAW_RECT_ROUNDED:
                    backend.draw_rect_rounded(cmd->bounds, cmd->color, cmd->corner_radius);
                    break;
                case CMD_DRAW_RECT_BORDER:
                    backend.draw_rect_border(cmd->bounds, cmd->border_color,
                                             cmd->corner_radius, cmd->border_width);
                    break;
                case CMD_DRAW_RECT_INVERSE:
                    backend.draw_rect_rounded_inverse(cmd->bounds, cmd->color,
                                                      cmd->corner_radius);
                    break;
                case CMD_DRAW_TEXT:
                    if (cmd->color2.a > 0) {
                        backend.draw_text_sized_gradient(cmd->text, cmd->bounds.x,
                                                         cmd->bounds.y, cmd->text_size,
                                                         cmd->color, cmd->color2,
                                                         cmd->text_weight);
                    } else {
                        backend.draw_text_sized(cmd->text, cmd->bounds.x, cmd->bounds.y,
                                                cmd->text_size, cmd->color,
                                                cmd->text_weight);
                    }
                    break;
                case CMD_SET_CLIP:
                    backend.set_clip(cmd->bounds);
                    break;
                case CMD_DRAW_TEXTURE:
                    backend.draw_texture(cmd->texture_id, cmd->bounds, cmd->uv,
                                         cmd->color);
                    break;
            }
        }

        backend.end_frame();

        if (options.screenshot && frame + 1 >= options.frames) {
            if (backend.save_screenshot_ppm(options.screenshot)) {
                std::printf("screenshot: %s\n", options.screenshot);
            } else {
                std::fprintf(stderr, "failed to write screenshot\n");
            }
            platform::swap_buffers(window);
            break;
        }

        platform::swap_buffers(window);
        frame_arena.reset();
        ++frame;
    }

    frame_arena.destroy();
    state_arena.destroy();
    platform::destroy(window);
    return 0;
}
