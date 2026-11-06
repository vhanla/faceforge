#pragma once
#include <cstdint>

// Keyboard / text / clipboard / drag-and-drop input, filled once per frame by
// the app (see platform/feed_ui_input) and read by widgets. The platform layer
// owns the event queues; this header stays free of GLFW and platform types so
// widgets and the core never depend on a windowing system.
//
// Persistent fields (`focused_id`, `edits`) survive across frames; the app does
// not clear them. Per-frame fields are reset by feed_ui_input before the pass.

constexpr int kMaxEditFields = 24;
constexpr int kMaxInputChars = 32;
constexpr int kMaxDroppedFiles = 8;

// Per editable widget, keyed by a stable id. The caret and selection are byte
// offsets into the app-owned UTF-8 buffer; `scroll` keeps the caret visible.
struct UITextEditState {
    uint64_t id = 0;       // 0 = free slot
    int32_t caret = 0;
    int32_t sel = 0;       // selection anchor; == caret when no selection
    float scroll = 0.0f;
    uint8_t focused_prev = 0;
};

struct UIInput {
    // Typed Unicode codepoints this frame (already filtered to printable).
    uint32_t characters[kMaxInputChars] = {};
    int32_t char_count = 0;

    // Editing keys, as press counts (repeats included) this frame.
    int32_t key_backspace = 0;
    int32_t key_delete = 0;
    int32_t key_left = 0;
    int32_t key_right = 0;
    int32_t key_home = 0;
    int32_t key_end = 0;
    int32_t key_enter = 0;
    int32_t key_escape = 0;
    int32_t key_tab = 0;

    // Modifier / shortcut state. Shortcut keys are only set while Ctrl is held.
    uint8_t key_shift = 0;
    uint8_t key_ctrl = 0;
    uint8_t key_a = 0;
    uint8_t key_c = 0;
    uint8_t key_v = 0;
    uint8_t key_x = 0;

    // Clipboard text for paste (owned by the platform, valid this frame) and a
    // frame-arena request the widget sets on copy/cut; the app writes it back
    // after the UI pass and must reset it before the next pass.
    const char* clipboard = nullptr;
    const char* clipboard_out = nullptr;

    // Files dropped on the window since the last frame. Paths are owned by the
    // platform and valid for this frame only.
    const char* dropped_files[kMaxDroppedFiles] = {};
    int32_t dropped_count = 0;

    // Keyboard focus and per-field edit state (persistent).
    uint64_t focused_id = 0;
    UITextEditState edits[kMaxEditFields] = {};
};
