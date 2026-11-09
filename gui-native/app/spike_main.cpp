// FaceForge native UI entry point.
//
// Drives the real application: it builds app::App (the UI-agnostic pipeline:
// engine, worker thread, identity library, undo/redo) and the FaceForge screen
// (app/ui_faceforge.*), which is rendered with the vendored AMUIE widgets on
// top of the unchanged C core (core/include/faceforge.h).
//
// The only work this file does beyond the frame loop is texture lifetime: the
// UI draws image ids, so the entry point uploads / destroys the base, result,
// face-thumbnail, identity-thumbnail and video-frame textures as the app's
// dirty flags and generations change.

#include "app.hpp"
#include "config.hpp"
#include "ui_faceforge.hpp"
#include "video.hpp"

#include "backend/opengl3_backend.hpp"
#include "core/arena.hpp"
#include "core/theme.hpp"
#include "core/types.hpp"
#include "platform/input.hpp"
#include "platform/window.hpp"
#include "widgets/ui_widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

namespace {

constexpr int kDesignW = 1532;
constexpr int kDesignH = 1027;

struct Options {
    const char* screenshot = nullptr;
    std::string target = "assets/target_t1.jpg";
    bool target_set = false;
    std::vector<std::string> identities;
    std::string save;
    std::string config;
    std::string provider;
    std::string models;
    std::string temp_dir;
    std::string export_dir;
    bool no_config = false;
    bool no_update_check = false;
    bool check_updates = false;
    bool update_explicit = false;
    int frames = 3;
    bool light = false;
    bool acrylic = false;
    bool acrylic_set = false;
    bool video = false;
    std::string video_path;
    bool wait_idle = false;
    int width = kDesignW;
    int height = kDesignH;
};

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --target PATH       target image to open (default assets/target_t1.jpg)\n"
        "  --identity PATH     identity image; repeatable, or a ';'-separated list\n"
        "  --models DIR        models directory (default models)\n"
        "  --temp-dir DIR      temporary files directory\n"
        "  --export-dir DIR    default export directory\n"
        "  --provider NAME     auto | cuda | cpu\n"
        "  --config PATH       settings file\n"
        "  --no-config         ignore the settings file\n"
        "  --no-update-check   disable the update check\n"
        "  --check-updates     enable the update check\n"
        "  --save PATH         save path used by the Output field\n"
        "  --video [PATH]      start in the video workspace, optionally opening PATH\n"
        "  --light             start with the light theme\n"
        "  --acrylic           translucent surfaces over a blurred desktop (default\n"
        "                      when the platform supports it)\n"
        "  --no-acrylic        keep the window opaque\n"
        "  --screenshot OUT    render a few frames, write a PPM and exit\n"
        "  --frames N          frames to render before the screenshot (default 3)\n"
        "  --wait              screenshot only once the pipeline goes idle\n"
        "  --size WxH          initial window size\n",
        argv0);
}

Options parse_args(int argc, char** argv) {
    Options o;
    auto value = [&](int& i) -> const char* {
        if (i + 1 < argc) return argv[++i];
        return nullptr;
    };
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--screenshot") == 0) {
            if (const char* v = value(i)) o.screenshot = v;
        } else if (std::strcmp(a, "--target") == 0 || std::strcmp(a, "--image") == 0) {
            if (const char* v = value(i)) {
                o.target = v;
                o.target_set = true;
            }
        } else if (std::strcmp(a, "--identity") == 0) {
            if (const char* v = value(i)) {
                // Accept both a repeated flag and a ';'-separated list.
                std::string rest = v;
                size_t start = 0;
                while (start <= rest.size()) {
                    const size_t semi = rest.find(';', start);
                    const size_t end = semi == std::string::npos ? rest.size() : semi;
                    if (end > start) o.identities.push_back(rest.substr(start, end - start));
                    if (semi == std::string::npos) break;
                    start = semi + 1;
                }
            }
        } else if (std::strcmp(a, "--models") == 0 || std::strcmp(a, "--models-dir") == 0) {
            if (const char* v = value(i)) o.models = v;
        } else if (std::strcmp(a, "--temp-dir") == 0) {
            if (const char* v = value(i)) o.temp_dir = v;
        } else if (std::strcmp(a, "--export-dir") == 0) {
            if (const char* v = value(i)) o.export_dir = v;
        } else if (std::strcmp(a, "--provider") == 0 || std::strcmp(a, "--device") == 0) {
            if (const char* v = value(i)) o.provider = v;
        } else if (std::strcmp(a, "--config") == 0) {
            if (const char* v = value(i)) o.config = v;
        } else if (std::strcmp(a, "--save") == 0) {
            if (const char* v = value(i)) o.save = v;
        } else if (std::strcmp(a, "--no-config") == 0) {
            o.no_config = true;
        } else if (std::strcmp(a, "--no-update-check") == 0) {
            o.no_update_check = true;
        } else if (std::strcmp(a, "--check-updates") == 0) {
            o.check_updates = true;
            o.update_explicit = true;
        } else if (std::strcmp(a, "--frames") == 0) {
            if (const char* v = value(i)) o.frames = std::max(1, std::atoi(v));
        } else if (std::strcmp(a, "--size") == 0) {
            int w = 0, h = 0;
            if (const char* v = value(i)) {
                if (std::sscanf(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    o.width = w;
                    o.height = h;
                }
            }
        } else if (std::strcmp(a, "--light") == 0) {
            o.light = true;
        } else if (std::strcmp(a, "--acrylic") == 0) {
            o.acrylic = true;
            o.acrylic_set = true;
        } else if (std::strcmp(a, "--no-acrylic") == 0) {
            o.acrylic = false;
            o.acrylic_set = true;
        } else if (std::strcmp(a, "--video") == 0) {
            o.video = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.video_path = argv[++i];
        } else if (std::strcmp(a, "--wait") == 0) {
            o.wait_idle = true;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        }
    }
    return o;
}

void apply_env(app::App& app, const Options& o) {
    if (const char* v = std::getenv("FACEFORGE_MODELS")) app.model_dir = v;
    if (const char* v = std::getenv("FACEFORGE_TEMP_DIR")) app.temp_dir = v;
    if (const char* v = std::getenv("FACEFORGE_EXPORT_DIR")) app.export_dir = v;
    if (const char* v = std::getenv("FACEFORGE_PROVIDER")) app.provider = config::providerFromName(v);
    if (std::getenv("FACEFORGE_SEQUENTIAL")) app.unload_after_run = true;
    if (const char* v = std::getenv("FACEFORGE_CHECK_UPDATES")) {
        app.check_updates = !(std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0 ||
                              std::strcmp(v, "off") == 0);
    }
    if (const char* v = std::getenv("FACEFORGE_UPDATE_INTERVAL_HOURS")) {
        const long h = std::strtol(v, nullptr, 10);
        if (h > 0) app.update_interval_hours = static_cast<uint32_t>(std::min<long>(h, 24 * 365));
    }
    (void)o;
}

void apply_cli(app::App& app, const Options& o) {
    if (!o.models.empty()) app.model_dir = o.models;
    if (!o.temp_dir.empty()) app.temp_dir = o.temp_dir;
    if (!o.export_dir.empty()) app.export_dir = o.export_dir;
    if (!o.provider.empty()) app.provider = config::providerFromName(o.provider);
    if (o.no_update_check) app.check_updates = false;
    if (o.update_explicit) app.check_updates = o.check_updates;
    if (!o.save.empty()) {
        app.save_path = o.save;
        app.save_explicit = true;
    }
}

bool is_video_path(const char* path) {
    if (!path) return false;
    const char* dot = std::strrchr(path, '.');
    if (!dot) return false;
    std::string ext;
    for (const char* p = dot + 1; *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        ext.push_back(c);
    }
    return ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "webm" || ext == "avi" ||
           ext == "m4v" || ext == "mpg" || ext == "mpeg";
}

// ---- texture lifetime ------------------------------------------------------

// Uploads (or destroys, when the image is gone) a full-resolution image.
void upload_image(OpenGL3Backend& backend, uint64_t& tex, int& tw, int& th,
                  const ff::Image* img) {
    if (tex) {
        backend.destroy_texture(tex);
        tex = 0;
    }
    tw = th = 0;
    if (!img || !img->valid()) return;
    tex = backend.create_texture_bgr(img->data, img->width, img->height, img->stride);
    tw = img->width;
    th = img->height;
}

// Uploads a thumbnail into `tex`, recreating it (thumbnails never change size).
void upload_thumb(OpenGL3Backend& backend, uint64_t& tex, const ff::Image& img) {
    if (!img.valid()) {
        if (tex) {
            backend.destroy_texture(tex);
            tex = 0;
        }
        return;
    }
    if (tex) backend.destroy_texture(tex);
    tex = backend.create_texture_bgr(img.data, img.width, img.height, img.stride);
}

void sync_textures(OpenGL3Backend& backend, app::App& app, UIState& st) {
    if (app.base_dirty) {
        app.base_dirty = false;
        upload_image(backend, st.base_tex, st.base_tex_w, st.base_tex_h,
                     app.base.valid() ? &app.base : nullptr);
    }
    if (app.result_dirty) {
        app.result_dirty = false;
        // In Studio mode the canvas shows the result with the retouch mask
        // blended in, so upload the display image instead of the raw result.
        const ff::Image* shown = app.studio ? app.displayImage() : &app.result;
        upload_image(backend, st.result_tex, st.result_tex_w, st.result_tex_h,
                     shown && shown->valid() ? shown : nullptr);
    }

    // Studio retouch mask: straight-alpha RGBA at base resolution, driven by
    // App::mask_gen so it stays in sync with UI-thread mask edits.
    if (app.studio && app.studio_show_mask && app.mask_gen != st.mask_gen_seen) {
        st.mask_gen_seen = app.mask_gen;
        int pw = 0, ph = 0;
        const uint8_t* rgba = app.maskPreviewData(&pw, &ph);
        if (rgba && pw > 0 && ph > 0) {
            if (st.mask_tex) backend.destroy_texture(st.mask_tex);
            st.mask_tex = backend.create_texture_rgba(rgba, pw, ph, 4 * pw);
            st.mask_tex_w = pw;
            st.mask_tex_h = ph;
        }
    }

    // Face thumbnails are rebuilt by App::poll() on this thread, so reading the
    // dirty flags here (after poll) cannot race the worker.
    for (int i = 0; i < app::kMaxFaces; ++i) {
        if (i < app.face_count && app.face_thumb[i].dirty) {
            app.face_thumb[i].dirty = false;
            upload_thumb(backend, st.face_tex[i], app.face_thumb[i].img);
        } else if (i >= app.face_count && st.face_tex[i]) {
            backend.destroy_texture(st.face_tex[i]);
            st.face_tex[i] = 0;
        }
    }

    // Identity thumbnails live behind App::eachIdentityThumb's lock.
    const app::IdentitiesSnapshot ids = app.identitiesSnapshot();
    if (ids.gen != st.identity_gen_seen) {
        st.identity_gen_seen = ids.gen;
        int seen = 0;
        app.eachIdentityThumb([&](int i, const ff::Image& img) {
            if (i < 0 || i >= app::kMaxIdentities) return;
            upload_thumb(backend, st.identity_tex[i], img);
            seen = i + 1;
        });
        for (int i = seen; i < app::kMaxIdentities; ++i) {
            if (st.identity_tex[i]) {
                backend.destroy_texture(st.identity_tex[i]);
                st.identity_tex[i] = 0;
            }
        }
    }
}

void sync_video_textures(OpenGL3Backend& backend, app::Video& video, UIState& st) {
    if (video.cur_dirty) {
        video.cur_dirty = false;
        upload_image(backend, st.vid_cur_tex, st.vid_cur_tex_w, st.vid_cur_tex_h,
                     video.cur_img.valid() ? &video.cur_img : nullptr);
    }
    if (video.res_dirty) {
        video.res_dirty = false;
        upload_image(backend, st.vid_res_tex, st.vid_res_tex_w, st.vid_res_tex_h,
                     video.res_img.valid() ? &video.res_img : nullptr);
    }
    for (int i = 0; i < app::kVideoMaxFaces; ++i) {
        if (video.face_thumbs[i].dirty) {
            video.face_thumbs[i].dirty = false;
            upload_thumb(backend, st.vid_face_tex[i], video.face_thumbs[i].img);
        } else if (!video.face_thumbs[i].img.valid() && st.vid_face_tex[i]) {
            backend.destroy_texture(st.vid_face_tex[i]);
            st.vid_face_tex[i] = 0;
        }
    }
}

void destroy_textures(OpenGL3Backend& backend, UIState& st) {
    if (st.base_tex) backend.destroy_texture(st.base_tex);
    if (st.result_tex) backend.destroy_texture(st.result_tex);
    if (st.mask_tex) backend.destroy_texture(st.mask_tex);
    if (st.vid_cur_tex) backend.destroy_texture(st.vid_cur_tex);
    if (st.vid_res_tex) backend.destroy_texture(st.vid_res_tex);
    for (int i = 0; i < app::kVideoMaxFaces; ++i) {
        if (st.vid_face_tex[i]) backend.destroy_texture(st.vid_face_tex[i]);
    }
    for (int i = 0; i < app::kMaxFaces; ++i) {
        if (st.face_tex[i]) backend.destroy_texture(st.face_tex[i]);
    }
    for (int i = 0; i < app::kMaxIdentities; ++i) {
        if (st.identity_tex[i]) backend.destroy_texture(st.identity_tex[i]);
    }
}

int dispatch_commands(OpenGL3Backend& backend, UIContext& ui) {
    int drawn = 0;
    for (RenderCommand* cmd = ui.first_command; cmd; cmd = cmd->next) {
        ++drawn;
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
    return drawn;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_args(argc, argv);

    // The settings file is read here as well as by the app layer: the window is
    // created before the app exists, but acrylic has to be decided at creation
    // time (changing it later takes an application restart).
    const bool test_mode = std::getenv("FACEFORGE_SELFTEST") != nullptr ||
                           std::getenv("FACEFORGE_VIDEOTEST") != nullptr;
    const char* env_config = std::getenv("FACEFORGE_CONFIG");
    const bool explicit_config = !options.config.empty() || env_config != nullptr;
    const bool config_enabled = !options.no_config && (!test_mode || explicit_config);
    const std::string config_path =
        !options.config.empty() ? options.config
        : env_config            ? env_config
                                : config::defaultPath();

    // Precedence: CLI > environment > settings file > platform default.
    bool acrylic_wanted = true;
    if (config_enabled) {
        config::Values boot;
        if (config::load(config_path, boot)) acrylic_wanted = boot.acrylic;
    }
    if (const char* v = std::getenv("FACEFORGE_ACRYLIC")) {
        acrylic_wanted = !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
    }
    if (options.acrylic_set) acrylic_wanted = options.acrylic;
    const bool acrylic = acrylic_wanted && platform::acrylic_available();

    platform::WindowDesc desc;
    desc.width = options.width;
    desc.height = options.height;
    desc.title = "FaceForge";
    desc.undecorated = true;
    desc.transparent = acrylic;
    desc.acrylic = acrylic;
    desc.resizable = true;
    desc.visible = true;
    desc.center = true;

    platform::Window* window = platform::create(desc);
    if (!window) return 1;

    // If the window did not get an alpha-capable framebuffer, drawing the
    // translucent theme would just darken the UI against black.
    const bool acrylic_on = acrylic && platform::framebuffer_transparent(window);

    std::printf("platform: %s\n", platform::platform_name());

    Arena frame_arena = Arena::create(8 * 1024 * 1024);
    Arena state_arena = Arena::create(64 * 1024);
    UIState* state = state_arena.make<UIState>();
    if (!frame_arena.buffer || !state_arena.buffer || !state) {
        std::fprintf(stderr, "arena allocation failed\n");
        platform::destroy(window);
        return 1;
    }

    app::App* app = new app::App();
    app::Video* video = new app::Video(app);

    // ---- configuration precedence: CLI > environment > file > defaults ----
    if (config_enabled) app->loadConfig(config_path);
    apply_env(*app, options);
    apply_cli(*app, options);
    // Keep the Settings checkbox in sync with the acrylic preference used for
    // this session (CLI/env overrides included).
    app->acrylic = acrylic_wanted;

    state->app = app;
    state->video = video;
    state->light_theme = options.light;
    state->acrylic = acrylic_on;
    state->mode = options.video ? AppMode::Video : AppMode::Photo;
    if (std::getenv("FACEFORGE_SETTINGS")) state->show_settings = true;
    std::snprintf(state->models_dir, sizeof(state->models_dir), "%s", app->model_dir.c_str());

    if (std::getenv("FACEFORGE_SELFTEST")) {
        // Scripted runs render at full resolution.
        app->fast_preview = false;
    }

    // Target and identity can also come from the environment, matching the
    // Zig GUI's scripted E2E entry points.
    std::string target = options.target;
    if (!options.target_set) {
        if (const char* v = std::getenv("FACEFORGE_TARGET")) target = v;
    }
    std::vector<std::string> identities = options.identities;
    if (identities.empty()) {
        if (const char* v = std::getenv("FACEFORGE_IDENTITY")) {
            const std::string list = v;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t semi = list.find(';', start);
                const size_t end = semi == std::string::npos ? list.size() : semi;
                if (end > start) identities.push_back(list.substr(start, end - start));
                if (semi == std::string::npos) break;
                start = semi + 1;
            }
        }
    }

    // Match the Zig GUI: the bundled portrait is the default identity unless a
    // run supplies its own list.
    if (identities.empty()) identities.push_back("assets/source_lena.jpg");

    if (!target.empty()) {
        app->openTarget(target);
        state->sync_output_path = true;
    }
    for (const std::string& p : identities) app->addIdentityFile(p);

    // Scripted Studio entry point: open the retouch workspace with a mask ready
    // so screenshots can exercise the panel and canvas overlay. An optional
    // stroke paints across the middle so the red overlay is visible.
    if (std::getenv("FACEFORGE_STUDIO")) {
        app->studio = true;
        app->studioEnsureMask();
        if (std::getenv("FACEFORGE_STUDIO_PAINT") && app->base.valid()) {
            const float w = static_cast<float>(app->base.width);
            const float h = static_cast<float>(app->base.height);
            app->studio_erase = true;
            app->studio_size = std::max(24.0f, w * 0.08f);
            app->studio_hard = 0.7f;
            app->studio_opac = 0.9f;
            app->studioBegin(w * 0.35f, h * 0.5f);
            for (int i = 1; i <= 12; ++i) {
                app->studioTo(w * (0.35f + 0.03f * static_cast<float>(i)), h * 0.5f);
            }
            app->studioEnd();
        }
    }

    // Video mode: the autotest owns opening (+ analysis/export/playback), a
    // path from the CLI or FACEFORGE_VIDEO opens the workspace directly.
    if (const char* p = std::getenv("FACEFORGE_VIDEOTEST")) {
        state->mode = AppMode::Video;
        video->setAutotest(p);
    } else {
        std::string open_video;
        if (!options.video_path.empty()) {
            open_video = options.video_path;
        } else if (const char* p = std::getenv("FACEFORGE_VIDEO")) {
            open_video = p;
        }
        if (!open_video.empty()) {
            state->mode = AppMode::Video;
            video->openPath(open_video);
            std::snprintf(state->video_path, sizeof(state->video_path), "%s",
                          open_video.c_str());
        }
    }

    OpenGL3Backend backend;
    backend.init();

    UIContext ui;
    bool prev_mouse_down = false;
    bool prev_mouse_right_down = false;
    bool prev_f2_down = false;
    bool prev_space_down = false;
    bool prev_esc_down = false;
    bool prev_left_down = false;
    bool prev_right_down = false;
    bool prev_home_down = false;
    bool prev_end_down = false;
    bool prev_i_down = false;
    bool prev_o_down = false;
    bool prev_c_down = false;
    bool prev_v_down = false;
    double prev_time = platform::time_seconds();
    double ui_ms_total = 0.0;
    int ui_frames = 0;
    int frame = 0;

    // Clipboard handoff: remember the generation last pushed to the OS so copy
    // requests are forwarded exactly once.
    uint32_t clipboard_gen_seen = app->clipboard_gen;

    // Canvas drag-out state: armed on a left press inside the published result
    // rectangle and started once the pointer moves past a small threshold.
    bool drag_out_armed = false;
    float drag_out_x = 0.0f;
    float drag_out_y = 0.0f;

    while (!platform::should_close(window)) {
        platform::poll_events();
        platform::feed_ui_input(window, &ui);

        const bool esc_down = platform::key_pressed(window, platform::Key::Escape);
        if (esc_down && !prev_esc_down) {
            if (ui.active_popup_id != 0) {
                ui.active_popup_id = 0;
                ui.active_popup_rect = Rect{};
            } else if (state->show_settings || state->show_about) {
                state->show_settings = false;
                state->show_about = false;
            } else {
                platform::set_should_close(window);
            }
        }
        prev_esc_down = esc_down;

        const bool f2_down = platform::key_pressed(window, platform::Key::F2);
        if (f2_down && !prev_f2_down) state->light_theme = !state->light_theme;
        prev_f2_down = f2_down;

        auto edge = [&](int key, bool& prev) {
            const bool down = platform::key_raw_down(window, key);
            const bool hit = down && !prev;
            prev = down;
            return hit;
        };
        const bool space_down = platform::key_raw_down(window, GLFW_KEY_SPACE);
        const bool space_hit = space_down && !prev_space_down;
        prev_space_down = space_down;
        const bool left_hit = edge(GLFW_KEY_LEFT, prev_left_down);
        const bool right_hit = edge(GLFW_KEY_RIGHT, prev_right_down);
        const bool home_hit = edge(GLFW_KEY_HOME, prev_home_down);
        const bool end_hit = edge(GLFW_KEY_END, prev_end_down);
        const bool i_hit = edge(GLFW_KEY_I, prev_i_down);
        const bool o_hit = edge(GLFW_KEY_O, prev_o_down);

        // Clipboard shortcuts (only when no text field owns the keyboard):
        // Ctrl+C copies the result image, Ctrl+V requests an image paste.
        const bool c_hit = edge(GLFW_KEY_C, prev_c_down);
        const bool v_hit = edge(GLFW_KEY_V, prev_v_down);
        if (ui.input.focused_id == 0 && ui.input.key_ctrl) {
            if (c_hit && state->mode == AppMode::Photo) {
                app->copyResult();
            }
            if (v_hit) {
                state->paste_request =
                    (state->mode == AppMode::Photo && !app->base.valid()) ? 1 : 2;
            }
        }

        if (state->mode == AppMode::Video && video->isOpen() && ui.input.focused_id == 0) {
            const int64_t step = ui.input.key_shift ? 10 : 1;
            if (space_hit) video->togglePlayback();
            if (left_hit) video->stepBy(-step);
            if (right_hit) video->stepBy(step);
            if (home_hit) video->seekTo(video->in_frame);
            if (end_hit) video->seekTo(video->out_frame);
            if (i_hit) {
                video->in_frame = video->cur;
                if (video->out_frame < video->in_frame) video->out_frame = video->in_frame;
            }
            if (o_hit) {
                video->out_frame = video->cur;
                if (video->in_frame > video->out_frame) video->in_frame = video->out_frame;
            }
        }

        // Drag-and-drop: a video (or any drop in the video workspace) opens the
        // video editor; otherwise the first image opens the target and later
        // ones join the identity library.
        if (ui.input.dropped_count > 0) {
            const char* first = ui.input.dropped_files[0];
            if (first && *first && (state->mode == AppMode::Video || is_video_path(first))) {
                state->mode = AppMode::Video;
                video->openPath(first);
                std::snprintf(state->video_path, sizeof(state->video_path), "%s", first);
            } else if (!app->busy()) {
                for (int i = 0; i < ui.input.dropped_count; ++i) {
                    const char* path = ui.input.dropped_files[i];
                    if (!path || !*path) continue;
                    if (!app->base.valid()) {
                        app->openTarget(path);
                        state->sync_output_path = true;
                    } else {
                        app->addIdentityFile(path);
                    }
                }
            }
        }

        // Advance the workers and publish anything they finished this frame.
        app->poll();
        video->poll();
        sync_textures(backend, *app, *state);
        sync_video_textures(backend, *video, *state);
        if (video->testDone()) platform::set_should_close(window);

        int fb_w = 0, fb_h = 0;
        int win_w = 0, win_h = 0;
        platform::get_framebuffer_size(window, &fb_w, &fb_h);
        platform::get_window_size(window, &win_w, &win_h);
        if (fb_w <= 0 || fb_h <= 0) continue;

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

        const bool mouse_middle_down = platform::mouse_middle_down(window);

        const double now = platform::time_seconds();
        const float dt = static_cast<float>(std::clamp(now - prev_time, 0.0, 0.05));
        prev_time = now;

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
        ui.mouse_middle_down = mouse_middle_down;

        ui.theme = state->acrylic
                       ? (state->light_theme ? &acrylic_light_theme() : &acrylic_dark_theme())
                       : (state->light_theme ? &theme_light() : &theme_faceforge());
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

        const double ui_start = platform::time_seconds();
        faceforge_draw(&frame_arena, &ui, state);
        ui_end_frame(&frame_arena, &ui);

        // Text widgets may have requested a clipboard write (copy/cut).
        if (ui.input.clipboard_out) {
            platform::clipboard_set_text(window, ui.input.clipboard_out);
        }

        // Paste requests (toolbar, context menus or Ctrl+V): 1 = target image,
        // 2 = identity image, 3 = video (unsupported).
        if (state->paste_request != 0) {
            const int req = state->paste_request;
            state->paste_request = 0;
            std::vector<uint8_t> png;
            if (req == 3) {
                app->setStatus("Clipboard video paste is not supported yet", true);
            } else if (!platform::clipboard_get_image_png(window, png) || png.empty()) {
                app->setStatus("Clipboard has no image", true);
            } else {
                std::string err;
                ff::Image img = ff::Image::decode(png.data(), static_cast<int32_t>(png.size()), &err);
                if (!img.valid()) {
                    app->setStatus("Cannot decode clipboard image", true);
                } else if (req == 1) {
                    app->openTargetImage(std::move(img), "");
                    state->sync_output_path = true;
                } else {
                    app->addIdentityImage(std::move(img), "pasted");
                }
            }
        }

        // Push the result to the OS clipboard when the app produced a new payload.
        if (app->clipboard_gen != clipboard_gen_seen) {
            clipboard_gen_seen = app->clipboard_gen;
            if (!app->clipboard_png.empty()) {
                platform::clipboard_set_image_png(window, app->clipboard_png.data(),
                                                  app->clipboard_png.size());
            }
        }

        // Canvas drag-out: arm on a left press inside the published result rect,
        // then start the native drag once the pointer moves a few pixels. The
        // call blocks until the button is released. Studio disables it via
        // canvas_drag_ready, and it only applies in Photo mode.
        if (mouse_clicked && state->mode == AppMode::Photo && state->canvas_drag_ready &&
            ui.mouse_x >= state->canvas_drag_rect.x &&
            ui.mouse_x <= state->canvas_drag_rect.x + state->canvas_drag_rect.w &&
            ui.mouse_y >= state->canvas_drag_rect.y &&
            ui.mouse_y <= state->canvas_drag_rect.y + state->canvas_drag_rect.h) {
            drag_out_armed = true;
            drag_out_x = ui.mouse_x;
            drag_out_y = ui.mouse_y;
        }
        if (!mouse_down) {
            drag_out_armed = false;
        } else if (drag_out_armed) {
            const float ddx = ui.mouse_x - drag_out_x;
            const float ddy = ui.mouse_y - drag_out_y;
            if (ddx * ddx + ddy * ddy > 36.0f) {
                drag_out_armed = false;
                std::vector<uint8_t> png;
                if (app->encodeDisplayPng(png) && !png.empty()) {
                    if (platform::drag_source_file(window, png.data(), png.size())) {
                        app->setStatus("Result dropped", false);
                    } else {
                        app->setStatus("Drag-out is not available on this desktop", true);
                    }
                }
            }
        }

        switch (state->window_action) {
            case WindowAction::Minimize: platform::minimize(window); break;
            case WindowAction::Maximize: platform::toggle_maximize(window); break;
            case WindowAction::Close: platform::set_should_close(window); break;
            case WindowAction::None: break;
        }
        platform::drag_update(window,
            state->header_drag && mouse_down && state->window_action == WindowAction::None);
        platform::resize_update(window,
            state->size_grip_drag && mouse_down && state->window_action == WindowAction::None);
        platform::set_cursor_resize(window,
            state->size_grip_hovered || state->size_grip_drag);

        const Theme& theme = *ui.theme;
        backend.set_clear_color(theme.bg_window);
        backend.begin_frame(fb_w, fb_h);
        backend.set_viewport_2d(0, 0, fb_w, fb_h, scale_x, scale_y);
        dispatch_commands(backend, ui);
        backend.end_frame();

        ui_ms_total += (platform::time_seconds() - ui_start) * 1000.0;
        ++ui_frames;

        const bool idle_ok = !options.wait_idle || (!app->busy() && !video->active());
        if (options.screenshot && frame + 1 >= options.frames && idle_ok) {
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

    std::printf("ui cpu cost: %.2f ms/frame over %d frames\n",
                ui_frames > 0 ? ui_ms_total / ui_frames : 0.0, ui_frames);

    if (app->config_dirty) app->saveConfig();
    video->close();
    destroy_textures(backend, *state);
    delete video;
    delete app;
    frame_arena.destroy();
    state_arena.destroy();
    platform::destroy(window);
    return 0;
}
