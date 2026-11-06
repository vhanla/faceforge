#pragma once
// FaceForge native screen, drawn with the vendored AMUIE immediate-mode
// widgets and bound to the UI-agnostic app layer (app.hpp). The screen owns
// only presentation state: theme, scroll offsets, view toggles, the video
// mock and dialog visibility. Everything about the pipeline (engine, faces,
// identities, result, undo) lives in app::App and is read through `app`.
//
// Textures are owned by the caller (the backend), so the screen renders image
// ids that the entry point fills from `app` each frame:
//   base_tex    pristine input image (the "Original" view)
//   result_tex  composed/retouched output (the normal view)
//   face_tex[]  per-face thumbnails
//   identity_tex[] identity thumbnails

#include "app.hpp"

#include "core/arena.hpp"
#include "core/types.hpp"

enum class WindowAction { None, Minimize, Maximize, Close };
enum class AppMode { Photo, Video };

struct UIState {
    app::App* app = nullptr;

    bool light_theme = false;
    AppMode mode = AppMode::Photo;

    // Navigation rail:
    bool sidebar_expanded = false; // false = compact icon rail (~54px), true = expanded (~148px)

    // Photo mode settings
    bool overlay = true;
    bool original = false;
    bool fit = true;
    float zoom = 1.0f;

    float left_scroll = 0.0f;  // vertical scroll of the left sidebar (logical px)
    float studio_scroll = 0.0f;

    // Video mode state (mockup; the real pipeline lands in a later phase)
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
    char video_path[512] = "";

    // Dialogs / Modals
    bool show_settings = false;
    bool show_about = false;

    WindowAction window_action = WindowAction::None;
    bool header_drag = false;
    bool size_grip_hovered = false;
    bool size_grip_drag = false;

    char output_path[512] = "";
    char models_dir[512] = "models";
    int provider_index = 0; // 0 auto, 1 cuda, 2 cpu

    // Per-face context menu target (index, or -1).
    int face_menu_target = -1;

    // Canvas / thumbnail textures, filled by the entry point each frame.
    uint64_t base_tex = 0;
    int base_tex_w = 0;
    int base_tex_h = 0;
    uint64_t result_tex = 0;
    int result_tex_w = 0;
    int result_tex_h = 0;
    uint64_t face_tex[app::kMaxFaces] = {};
    uint64_t identity_tex[app::kMaxIdentities] = {};
    uint32_t identity_gen_seen = 0;

    // Set for one frame by the entry point when it wants the Output field to
    // adopt a new default path (e.g. after opening a target).
    bool sync_output_path = false;
};

void faceforge_draw(Arena* frame, UIContext* ui, UIState* state);
