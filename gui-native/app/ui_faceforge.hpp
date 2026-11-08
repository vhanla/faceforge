#pragma once
// FaceForge native screen, drawn with the vendored AMUIE immediate-mode
// widgets and bound to the UI-agnostic app layer (app.hpp). The screen owns
// only presentation state: theme, scroll offsets, view toggles, dialog
// visibility and the video workspace pointer. Everything about the pipeline
// (engine, faces, identities, result, undo) lives in app::App and is read
// through `app`; the video pipeline lives in app::Video and is read through
// `video`.
//
// Textures are owned by the caller (the backend), so the screen renders image
// ids that the entry point fills from `app`/`video` each frame:
//   base_tex    pristine input image (the "Original" view)
//   result_tex  composed/retouched output (the normal view)
//   mask_tex    Studio retouch mask preview (red overlay; base resolution)
//   face_tex[]  per-face thumbnails
//   identity_tex[] identity thumbnails
//   vid_cur_tex / vid_res_tex / vid_face_tex[]  video frame, processed frame
//                                               and sequence face crops

#include "app.hpp"

#include "core/arena.hpp"
#include "core/types.hpp"

namespace app {
class Video;
}

enum class WindowAction { None, Minimize, Maximize, Close };
enum class AppMode { Photo, Video };

struct UIState {
    app::App* app = nullptr;

    bool light_theme = false;
    // Acrylic background: translucent surfaces over a compositor-blurred
    // desktop (set by the entry point from the window/platform options).
    bool acrylic = false;
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

    // Video workspace: the pipeline state lives in app::Video (owned by the
    // entry point); the screen only keeps presentation state.
    app::Video* video = nullptr;
    float video_left_scroll = 0.0f;
    char video_path[512] = "";

    // Video canvas textures, filled by the entry point each frame.
    uint64_t vid_cur_tex = 0;
    int vid_cur_tex_w = 0;
    int vid_cur_tex_h = 0;
    uint64_t vid_res_tex = 0;
    int vid_res_tex_w = 0;
    int vid_res_tex_h = 0;
    uint64_t vid_face_tex[8] = {};

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

    // Studio retouch mask overlay (straight-alpha RGBA, base resolution), owned by
    // the entry point. `mask_gen` from app::App drives uploads so the preview stays
    // in sync with the mask on the UI thread.
    uint64_t mask_tex = 0;
    int mask_tex_w = 0;
    int mask_tex_h = 0;
    uint32_t mask_gen_seen = 0;
    uint64_t face_tex[app::kMaxFaces] = {};
    uint64_t identity_tex[app::kMaxIdentities] = {};
    uint32_t identity_gen_seen = 0;

    // Set for one frame by the entry point when it wants the Output field to
    // adopt a new default path (e.g. after opening a target).
    bool sync_output_path = false;
};

void faceforge_draw(Arena* frame, UIContext* ui, UIState* state);
