#pragma once
#include "backend/renderer_interface.hpp"

class OpenGL3Backend final : public IRendererBackend {
public:
    OpenGL3Backend() = default;
    ~OpenGL3Backend() override;

    void init() override;
    void begin_frame(int screen_w, int screen_h) override;
    void set_viewport(int x, int y, int w, int h, float scale) override;
    void set_viewport_2d(int x, int y, int w, int h,
                         float scale_x, float scale_y) override;
    void set_clear_color(Color c) override;

    void draw_rect_rounded(Rect r, Color c, float radius) override;
    void draw_rect_rounded_inverse(Rect r, Color c, float radius) override;
    void draw_rect_border(Rect r, Color c, float radius, float width) override;
    void draw_text_sized(const char* text, float x, float y, float size, Color c,
                         uint8_t weight) override;
    void draw_text_sized_gradient(const char* text, float x, float y, float size,
                                  Color c0, Color c1, uint8_t weight) override;

    uint64_t create_texture_bgr(const uint8_t* bgr, int w, int h,
                                int stride) override;
    uint64_t create_texture_rgba(const uint8_t* rgba, int w, int h,
                                 int stride) override;
    void destroy_texture(uint64_t id) override;
    void draw_texture(uint64_t id, Rect dst, Rect uv, Color tint) override;

    void set_clip(Rect r) override;
    void end_frame() override;

    // Demo/verification helper (not part of the agnostic renderer contract).
    bool save_screenshot_ppm(const char* path);

private:
    struct Impl;
    Impl* impl_ = nullptr;

    uint64_t create_texture_impl(const uint8_t* pixels, int w, int h, int stride,
                                 unsigned int format);

    void draw_text_impl(const char* text, float x, float y, float size, Color c0,
                        Color c1, uint8_t weight);
};
