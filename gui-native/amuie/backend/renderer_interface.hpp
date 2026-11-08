#pragma once
#include "core/types.hpp"
#include <algorithm>

// The only virtual boundary in the engine. Widgets/core never include a
// graphics API; each backend translates RenderCommands into its own pipeline.
class IRendererBackend {
public:
    virtual ~IRendererBackend() = default;

    virtual void init() = 0;
    virtual void begin_frame(int screen_w, int screen_h) = 0;
    virtual void set_viewport(int x, int y, int w, int h, float scale) = 0;
    // Per-axis variant: lets a responsive layout stretch horizontally while the
    // vertical axis keeps a fixed DPI ratio. Backends that cannot support
    // non-uniform scaling may fall back to `set_viewport` with min(sx, sy).
    virtual void set_viewport_2d(int x, int y, int w, int h,
                                 float scale_x, float scale_y) {
        set_viewport(x, y, w, h, std::min(scale_x, scale_y));
    }
    virtual void set_clear_color(Color c) = 0;

    virtual void draw_rect_rounded(Rect r, Color c, float radius) = 0;
    // Fills the region of `r` that lies outside its rounded outline. Used for
    // shaped windows: one call over the window rect clears the four corner tips.
    virtual void draw_rect_rounded_inverse(Rect r, Color c, float radius) = 0;
    virtual void draw_rect_border(Rect r, Color c, float radius, float width) = 0;
    virtual void draw_text(const char* text, float x, float y, Color c) {
        draw_text_sized(text, x, y, 14.0f, c, 0);
    }
    virtual void draw_text_sized(const char* text, float x, float y, float size,
                                 Color c, uint8_t weight = 0) = 0;
    // Horizontal gradient variant; a == 0 falls back to a solid fill.
    virtual void draw_text_sized_gradient(const char* text, float x, float y,
                                          float size, Color c0, Color c1,
                                          uint8_t weight = 0) {
        (void)c1;  // Backends without gradient support render the start color.
        draw_text_sized(text, x, y, size, c0, weight);
    }

    // Texture registry for images and video frames. Texture ids are opaque
    // backend handles; the core and widgets never see a GPU object. `bgr` is
    // 8-bit interleaved BGR (the faceforge native pixel layout), `stride` in
    // bytes. Returns 0 on failure. Upload is a cold path (once per image),
    // never per frame.
    virtual uint64_t create_texture_bgr(const uint8_t* bgr, int w, int h,
                                        int stride) {
        (void)bgr; (void)w; (void)h; (void)stride;
        return 0;
    }
    // Same registry for straight-alpha RGBA overlays (e.g. the red retouch mask
    // preview). The fragment shader samples texture alpha, so a regular
    // SRC_ALPHA blend shows it over the image underneath.
    virtual uint64_t create_texture_rgba(const uint8_t* rgba, int w, int h,
                                         int stride) {
        (void)rgba; (void)w; (void)h; (void)stride;
        return 0;
    }
    virtual void destroy_texture(uint64_t id) { (void)id; }
    virtual void draw_texture(uint64_t id, Rect dst, Rect uv, Color tint) {
        (void)id; (void)uv;  // Backends without textures render a flat tint.
        draw_rect_rounded(dst, tint, 0.0f);
    }

    virtual void set_clip(Rect r) = 0;
    virtual void end_frame() = 0;
};
