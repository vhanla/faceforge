#pragma once

namespace platform {

struct Window;

} // namespace platform

struct UIContext;

namespace platform {

// Drains the platform's keyboard/text/clipboard/drag-drop queues into
// `ctx->input` for the current frame. Call once, after poll_events() and before
// the UI pass; the per-frame fields are reset first. `window` may be null
// (headless tests / no window), in which case only the reset happens, leaving
// the persistent fields (focused_id, edits) untouched.
void feed_ui_input(Window* window, UIContext* ctx);

} // namespace platform
