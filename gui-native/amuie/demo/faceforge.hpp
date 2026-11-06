#pragma once
#include "core/arena.hpp"
#include "core/types.hpp"

enum class WindowAction { None, Minimize, Maximize, Close };
enum class AppMode { Photo, Video };

struct DemoState {
    bool light_theme = false;
    AppMode mode = AppMode::Photo;

    // Navigation rail:
    bool sidebar_expanded = false; // false = compact icon rail (~54px), true = expanded (~148px)

    // Photo mode settings (from sampleui.png)
    bool overlay = true;
    bool original = false;
    bool fit = true;
    float zoom = 1.0f;

    bool restore_mouths = true;
    bool fast_preview = true;
    bool unload_models = false;

    int enhancer_index = 0; // 0 = "GFPGAN 1.4" (matches sampleui.png)

    bool stats_preview = true;

    float left_scroll = 0.0f;  // vertical scroll of the left sidebar (logical px)

    // Video mode state (from mockup)
    float video_left_scroll = 0.0f;
    float video_right_scroll = 0.0f;
    int current_frame = 28;
    int total_frames = 121;
    int in_frame = 0;
    int out_frame = 120;
    float fps = 24.0f;
    bool is_playing = false;
    float play_accum = 0.0f;

    bool opt_sequence = true;
    bool opt_loop = true;
    bool opt_original = false;
    bool opt_boxes = true;

    bool vid_low_quality = true;
    bool vid_use_cache = true;
    bool vid_restore_mouths = true;
    bool vid_skip_unassigned = false;
    bool vid_paint_retouch = false;
    bool vid_audio = true;
    bool vid_split_chunks = false;

    int selected_sequence = 0;
    int identity_model_index = 0;
    char video_path[128] = "/home/vhanla/Videos/2c893f20-1e44-4f97-8d14.mp4";

    // Dialogs / Modals
    bool show_settings = false;
    bool show_about = false;

    WindowAction window_action = WindowAction::None;
    bool header_drag = false;
    bool size_grip_hovered = false;
    bool size_grip_drag = false;

    char output_path[96] = "bench/results/gui_result.png";

    // Optional canvas image. When set, the photo canvas draws this registered
    // texture (contain-fit) instead of the empty state. Used by the FaceForge
    // spike to prove the texture path and the native C ABI end to end.
    uint64_t canvas_tex = 0;
    int canvas_tex_w = 0;
    int canvas_tex_h = 0;
};

void demo_draw(Arena* frame, UIContext* ui, DemoState* state);
