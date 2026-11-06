#define GLFW_INCLUDE_NONE
#define GL_GLEXT_PROTOTYPES
#include <GLFW/glfw3.h>
#include <GL/glcorearb.h>

#include "backend/opengl3_backend.hpp"
#include "core/font.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char* kVertexShader = R"GLSL(
#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_local;
layout(location = 2) in vec4 a_fill;
layout(location = 3) in vec4 a_border;
layout(location = 4) in vec4 a_geom;   // width, height, radius, border_width
layout(location = 5) in vec2 a_uv;
layout(location = 6) in float a_mode;  // 0 = shape, 1 = glyph

uniform vec2 u_view_size;   // logical content size in px (framebuffer/scale)

out vec4 v_fill;
out vec4 v_border;
out vec4 v_geom;
out vec2 v_local;
out vec2 v_uv;
out float v_mode;

void main() {
    // Map logical px -> NDC relative to the viewport rect. glViewport already
    // applies the per-axis device scale and origin, so this is resolution-
    // independent and supports non-uniform (responsive) scaling.
    vec2 ndc = vec2(
        a_pos.x / u_view_size.x * 2.0 - 1.0,
        1.0 - a_pos.y / u_view_size.y * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_fill = a_fill;
    v_border = a_border;
    v_geom = a_geom;
    v_local = a_local * a_geom.xy;
    v_uv = a_uv;
    v_mode = a_mode;
}
)GLSL";

const char* kFragmentShader = R"GLSL(
#version 330 core
in vec4 v_fill;
in vec4 v_border;
in vec4 v_geom;
in vec2 v_local;
in vec2 v_uv;
in float v_mode;

uniform sampler2D u_atlas;
uniform sampler2D u_tex;   // texture registry (images / video frames)
uniform vec2 u_scale;  // device px per logical px, per axis

out vec4 FragColor;

float sd_round_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    if (v_mode > 3.5) {
        // Textures: sampled RGB multiplied by the tint (v_fill.rgb).
        vec4 tex = texture(u_tex, v_uv);
        FragColor = vec4(tex.rgb * v_fill.rgb, tex.a * v_fill.a);
        return;
    }

    if (v_mode > 2.5) {
        // Icons: pre-antialiased alpha from the atlas.
        float a = texture(u_atlas, v_uv).r;
        FragColor = vec4(v_fill.rgb, v_fill.a * a);
        return;
    }

    if (v_mode > 0.5 && v_mode < 1.5) {
        // Text: 8-bit signed distance field (0.5 == glyph contour). The screen
        // derivative keeps the edge one device pixel wide at any output size, so
        // downscaled text stays crisp instead of turning blocky.
        float d = texture(u_atlas, v_uv).r;
        float w = max(fwidth(d) * 0.5, 1.0 / 4096.0);
        float a = smoothstep(0.5 - w, 0.5 + w, d);
        FragColor = vec4(v_fill.rgb, v_fill.a * a);
        return;
    }

    vec2 half_size = v_geom.xy * 0.5;
    vec2 p = v_local - half_size;
    float radius = min(v_geom.z, min(half_size.x, half_size.y));
    // Anisotropic AA: work in device-pixel space so corners stay round and the
    // edge falloff is one pixel wide even when sx != sy (responsive layouts).
    float min_scale = min(u_scale.x, u_scale.y);
    vec2 dev_p = p * u_scale;
    vec2 dev_half = half_size * u_scale;
    float dev_radius = radius * min_scale;
    float d_dev = sd_round_box(dev_p, dev_half, dev_radius);
    float outer = 1.0 - smoothstep(-0.5, 0.5, d_dev);

    if (v_mode > 1.5) {
        // Inverse fill: only the area outside the rounded outline.
        FragColor = vec4(v_fill.rgb, v_fill.a * (1.0 - outer));
        return;
    }

    if (v_geom.w > 0.0) {
        float bw = min(v_geom.w, min(half_size.x, half_size.y));
        vec2 inner_half = max(half_size - vec2(bw), vec2(0.0));
        float inner_radius = max(radius - bw, 0.0);
        float din_dev = sd_round_box(dev_p, inner_half * u_scale,
                                     inner_radius * min_scale);
        float inner = 1.0 - smoothstep(-0.5, 0.5, din_dev);
        float ring = clamp(outer - inner, 0.0, 1.0);
        float fa = v_fill.a * inner;
        float ba = v_border.a * ring;
        float a = fa + ba * (1.0 - fa);
        vec3 rgb = (v_fill.rgb * fa + v_border.rgb * ba) / max(fa + ba, 0.00001);
        FragColor = vec4(rgb, a);
    } else {
        FragColor = vec4(v_fill.rgb, v_fill.a * outer);
    }
}
)GLSL";

GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        std::fprintf(stderr, "shader compile failed:\n%s\n", log);
    }
    return shader;
}

GLuint link_program(const char* vs, const char* fs) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei len = 0;
        glGetProgramInfoLog(p, sizeof(log), &len, log);
        std::fprintf(stderr, "program link failed:\n%s\n", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

} // namespace

struct OpenGL3Backend::Impl {
    struct Vertex {
        float x, y;
        float lx, ly;
        float fr, fg, fb, fa;
        float br, bg, bb, ba;
        float gw, gh, radius, border_w;
        float u, v;
        float mode;
    };

    static constexpr int kMaxVertices = 65536;

    // Texture registry: a fixed table so the hot path never allocates or
    // searches a container. Uploads happen once per image, not per frame.
    struct TexEntry {
        uint64_t id;
        GLuint tex;
        int w, h;
    };
    static constexpr int kMaxTextures = 256;

    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint program = 0;
    GLuint atlas = 0;
    GLint u_view_size = -1;
    GLint u_scale = -1;
    GLint u_atlas = -1;
    GLint u_tex = -1;

    TexEntry textures[kMaxTextures] = {};
    uint64_t next_tex_id = 1;

    int fb_w = 0, fb_h = 0;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    float scale_x = 1.0f;  // device px per logical px (horizontal)
    float scale_y = 1.0f;  // device px per logical px (vertical)
    Color clear_color{0x15, 0x17, 0x1c, 0xff};

    Vertex* vertices = nullptr;
    int count = 0;

    void flush() {
        if (count == 0) return;
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(count * sizeof(Vertex)), vertices);
        glDrawArrays(GL_TRIANGLES, 0, count);
        count = 0;
    }

    void add_vertex(float x, float y, float lx, float ly,
                    Color fill, Color border, float gw, float gh,
                    float radius, float bw, float u, float v, float mode) {
        Vertex& o = vertices[count++];
        o.x = x; o.y = y;
        o.lx = lx; o.ly = ly;
        o.fr = fill.r / 255.0f; o.fg = fill.g / 255.0f;
        o.fb = fill.b / 255.0f; o.fa = fill.a / 255.0f;
        o.br = border.r / 255.0f; o.bg = border.g / 255.0f;
        o.bb = border.b / 255.0f; o.ba = border.a / 255.0f;
        o.gw = gw; o.gh = gh;
        o.radius = radius; o.border_w = bw;
        o.u = u; o.v = v;
        o.mode = mode;
    }

    void add_quad(float x, float y, float w, float h,
                  Color fill, Color border, float radius, float bw,
                  float u0, float v0, float u1, float v1, float mode) {
        if (w <= 0.0f || h <= 0.0f) return;
        if (count + 6 > kMaxVertices) flush();

        add_vertex(x,     y,     0, 0, fill, border, w, h, radius, bw, u0, v0, mode);
        add_vertex(x + w, y,     1, 0, fill, border, w, h, radius, bw, u1, v0, mode);
        add_vertex(x + w, y + h, 1, 1, fill, border, w, h, radius, bw, u1, v1, mode);

        add_vertex(x,     y,     0, 0, fill, border, w, h, radius, bw, u0, v0, mode);
        add_vertex(x + w, y + h, 1, 1, fill, border, w, h, radius, bw, u1, v1, mode);
        add_vertex(x,     y + h, 0, 1, fill, border, w, h, radius, bw, u0, v1, mode);
    }

    // Like add_quad but with a left-to-right fill gradient (glyph tinting).
    // `mode` selects the sampling path (1 = SDF text, 3 = alpha icon).
    void add_quad_grad(float x, float y, float w, float h, Color left, Color right,
                       float u0, float v0, float u1, float v1, float mode = 1.0f) {
        if (w <= 0.0f || h <= 0.0f) return;
        if (count + 6 > kMaxVertices) flush();
        const Color none{0, 0, 0, 0};

        add_vertex(x,     y,     0, 0, left,  none, w, h, 0.0f, 0.0f, u0, v0, mode);
        add_vertex(x + w, y,     1, 0, right, none, w, h, 0.0f, 0.0f, u1, v0, mode);
        add_vertex(x + w, y + h, 1, 1, right, none, w, h, 0.0f, 0.0f, u1, v1, mode);

        add_vertex(x,     y,     0, 0, left,  none, w, h, 0.0f, 0.0f, u0, v0, mode);
        add_vertex(x + w, y + h, 1, 1, right, none, w, h, 0.0f, 0.0f, u1, v1, mode);
        add_vertex(x,     y + h, 0, 1, left,  none, w, h, 0.0f, 0.0f, u0, v1, mode);
    }
};

OpenGL3Backend::~OpenGL3Backend() {
    if (!impl_) return;
    for (int i = 0; i < Impl::kMaxTextures; ++i) {
        if (impl_->textures[i].tex) glDeleteTextures(1, &impl_->textures[i].tex);
    }
    if (impl_->vbo) glDeleteBuffers(1, &impl_->vbo);
    if (impl_->vao) glDeleteVertexArrays(1, &impl_->vao);
    if (impl_->atlas) glDeleteTextures(1, &impl_->atlas);
    if (impl_->program) glDeleteProgram(impl_->program);
    delete[] impl_->vertices;
    delete impl_;
}

void OpenGL3Backend::init() {
    impl_ = new Impl();
    impl_->vertices = new Impl::Vertex[Impl::kMaxVertices];
    impl_->program = link_program(kVertexShader, kFragmentShader);
    impl_->u_view_size = glGetUniformLocation(impl_->program, "u_view_size");
    impl_->u_scale = glGetUniformLocation(impl_->program, "u_scale");
    impl_->u_atlas = glGetUniformLocation(impl_->program, "u_atlas");
    impl_->u_tex = glGetUniformLocation(impl_->program, "u_tex");

    glUseProgram(impl_->program);
    glUniform1i(impl_->u_atlas, 0);
    glUniform1i(impl_->u_tex, 1);

    glGenVertexArrays(1, &impl_->vao);
    glBindVertexArray(impl_->vao);
    glGenBuffers(1, &impl_->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, impl_->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(Impl::kMaxVertices * sizeof(Impl::Vertex)),
                 nullptr, GL_DYNAMIC_DRAW);

    const GLsizei stride = sizeof(Impl::Vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(8));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(16));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(32));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(48));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(64));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(72));

    glGenTextures(1, &impl_->atlas);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl_->atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasWidth, kAtlasHeight, 0,
                 GL_RED, GL_UNSIGNED_BYTE, kAtlasPixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

void OpenGL3Backend::begin_frame(int screen_w, int screen_h) {
    impl_->fb_w = screen_w;
    impl_->fb_h = screen_h;
    impl_->count = 0;

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, screen_w, screen_h);
    glClearColor(impl_->clear_color.r / 255.0f, impl_->clear_color.g / 255.0f,
                 impl_->clear_color.b / 255.0f, impl_->clear_color.a / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void OpenGL3Backend::set_viewport(int x, int y, int w, int h, float scale) {
    set_viewport_2d(x, y, w, h, scale, scale);
}

void OpenGL3Backend::set_viewport_2d(int x, int y, int w, int h,
                                     float scale_x, float scale_y) {
    impl_->vx = x;
    impl_->vy = y;
    impl_->vw = w;
    impl_->vh = h;
    impl_->scale_x = scale_x > 0.0f ? scale_x : 1.0f;
    impl_->scale_y = scale_y > 0.0f ? scale_y : 1.0f;

    glViewport(x, y, w, h);
    glUseProgram(impl_->program);
    glUniform2f(impl_->u_scale, impl_->scale_x, impl_->scale_y);
    // Logical content size: the viewport rect divided by the per-axis scale.
    const float logical_w = static_cast<float>(w) / impl_->scale_x;
    const float logical_h = static_cast<float>(h) / impl_->scale_y;
    glUniform2f(impl_->u_view_size, logical_w, logical_h);

    // Default to the full logical content area.
    set_clip(Rect{0.0f, 0.0f, logical_w, logical_h});
}

void OpenGL3Backend::set_clear_color(Color c) { impl_->clear_color = c; }

void OpenGL3Backend::draw_rect_rounded(Rect r, Color c, float radius) {
    impl_->add_quad(r.x, r.y, r.w, r.h, c, Color{0, 0, 0, 0}, radius, 0.0f,
                    0, 0, 0, 0, 0);
}

void OpenGL3Backend::draw_rect_rounded_inverse(Rect r, Color c, float radius) {
    impl_->add_quad(r.x, r.y, r.w, r.h, c, Color{0, 0, 0, 0}, radius, 0.0f,
                    0, 0, 0, 0, 2.0f);
}

void OpenGL3Backend::draw_rect_border(Rect r, Color c, float radius, float width) {
    impl_->add_quad(r.x, r.y, r.w, r.h, Color{0, 0, 0, 0}, c, radius, width,
                    0, 0, 0, 0, 0);
}

void OpenGL3Backend::draw_text_sized(const char* text, float x, float y, float size,
                                     Color c, uint8_t weight) {
    draw_text_impl(text, x, y, size, c, Color{0, 0, 0, 0}, weight);
}

void OpenGL3Backend::draw_text_sized_gradient(const char* text, float x, float y,
                                              float size, Color c0, Color c1,
                                              uint8_t weight) {
    if (c1.a == 0) c1 = c0;
    draw_text_impl(text, x, y, size, c0, c1, weight);
}

void OpenGL3Backend::draw_text_impl(const char* text, float x, float y, float size,
                                    Color c0, Color c1, uint8_t weight) {
    if (!text || !*text) return;
    const bool gradient = c1.a > 0;

    const float font_scale = size / static_cast<float>(kFontBakeSize);
    const float baseline = y + static_cast<float>(kFontAscent) * font_scale;
    const float inv_w = 1.0f / static_cast<float>(kAtlasWidth);
    const float inv_h = 1.0f / static_cast<float>(kAtlasHeight);
    float pen = x;

    const char* p = text;
    while (*p) {
        const uint32_t cp = utf8_next(p);
        if (cp == 0) break;
        const Glyph* g = font_find(cp, weight);
        if (!g) continue;

        if (cp >= 0xE000u) {
            const float icon_scale = size / static_cast<float>(g->bake_px);
            // Icons keep their pre-antialiased alpha (mode 3) and carry an SDF-free
            // padding offset of 0.
            const float gx = pen + static_cast<float>(g->off_x) * icon_scale;
            const float gy = y + static_cast<float>(g->off_y) * icon_scale;
            const float w = static_cast<float>(g->w) * icon_scale;
            const float h = static_cast<float>(g->h) * icon_scale;
            const float u0 = static_cast<float>(g->x) * inv_w;
            const float v0 = static_cast<float>(g->y) * inv_h;
            const float u1 = static_cast<float>(g->x + g->w) * inv_w;
            const float v1 = static_cast<float>(g->y + g->h) * inv_h;
            if (gradient) impl_->add_quad_grad(gx, gy, w, h, c0, c1, u0, v0, u1, v1, 3.0f);
            else impl_->add_quad(gx, gy, w, h, c0, c0, 0.0f, 0.0f, u0, v0, u1, v1, 3.0f);
            continue;
        }

        const float gx = pen + static_cast<float>(g->off_x) * font_scale;
        const float gy = baseline + static_cast<float>(g->off_y) * font_scale;
        const float gw = static_cast<float>(g->w) * font_scale;
        const float gh = static_cast<float>(g->h) * font_scale;
        if (gw > 0.0f && gh > 0.0f) {
            const float u0 = static_cast<float>(g->x) * inv_w;
            const float v0 = static_cast<float>(g->y) * inv_h;
            const float u1 = static_cast<float>(g->x + g->w) * inv_w;
            const float v1 = static_cast<float>(g->y + g->h) * inv_h;
            if (gradient) impl_->add_quad_grad(gx, gy, gw, gh, c0, c1, u0, v0, u1, v1);
            else impl_->add_quad(gx, gy, gw, gh, c0, c0, 0.0f, 0.0f, u0, v0, u1, v1, 1.0f);
        }
        pen += g->advance * font_scale;
    }
}

uint64_t OpenGL3Backend::create_texture_bgr(const uint8_t* bgr, int w, int h,
                                            int stride) {
    if (!impl_ || !bgr || w <= 0 || h <= 0) return 0;
    int slot = -1;
    for (int i = 0; i < Impl::kMaxTextures; ++i) {
        if (impl_->textures[i].id == 0) { slot = i; break; }
    }
    if (slot < 0) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Native faceforge pixels are tightly packed BGR; GL_BGR avoids a swizzle.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride > 0 ? stride / 3 : w);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_BGR, GL_UNSIGNED_BYTE,
                 bgr);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);

    impl_->textures[slot].id = impl_->next_tex_id++;
    impl_->textures[slot].tex = tex;
    impl_->textures[slot].w = w;
    impl_->textures[slot].h = h;
    return impl_->textures[slot].id;
}

void OpenGL3Backend::destroy_texture(uint64_t id) {
    if (!impl_ || id == 0) return;
    for (int i = 0; i < Impl::kMaxTextures; ++i) {
        if (impl_->textures[i].id == id) {
            if (impl_->textures[i].tex) glDeleteTextures(1, &impl_->textures[i].tex);
            impl_->textures[i] = Impl::TexEntry{};
            return;
        }
    }
}

void OpenGL3Backend::draw_texture(uint64_t id, Rect dst, Rect uv, Color tint) {
    if (!impl_) return;
    GLuint tex = 0;
    for (int i = 0; i < Impl::kMaxTextures; ++i) {
        if (impl_->textures[i].id == id) { tex = impl_->textures[i].tex; break; }
    }
    if (!tex) {
        // Unknown handle: keep layout visible with a flat tint.
        impl_->add_quad(dst.x, dst.y, dst.w, dst.h, tint, Color{0, 0, 0, 0},
                        0.0f, 0.0f, 0, 0, 0, 0, 0.0f);
        return;
    }
    // A batch is drawn with a single bound sampler, so flush before switching
    // the texture (images are few and switching is rare).
    impl_->flush();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glActiveTexture(GL_TEXTURE0);
    impl_->add_quad(dst.x, dst.y, dst.w, dst.h, tint, Color{0, 0, 0, 0},
                    0.0f, 0.0f, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h, 4.0f);
}

void OpenGL3Backend::set_clip(Rect r) {
    impl_->flush();

    const float sx = impl_->scale_x;
    const float sy = impl_->scale_y;
    const float logical_h = static_cast<float>(impl_->vh) / sy;
    int x = static_cast<int>(std::lround(impl_->vx + r.x * sx));
    int y = static_cast<int>(std::lround(impl_->vy + (logical_h - (r.y + r.h)) * sy));
    int w = static_cast<int>(std::lround(r.w * sx));
    int h = static_cast<int>(std::lround(r.h * sy));
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
}

void OpenGL3Backend::end_frame() {
    impl_->flush();
    glDisable(GL_SCISSOR_TEST);
}

bool OpenGL3Backend::save_screenshot_ppm(const char* path) {
    if (!impl_ || impl_->fb_w <= 0 || impl_->fb_h <= 0) return false;
    const int w = impl_->fb_w;
    const int h = impl_->fb_h;
    auto* pixels = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(w) * h * 4));
    if (!pixels) return false;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::free(pixels);
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    auto* row = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(w) * 3));
    if (!row) {
        std::fclose(f);
        std::free(pixels);
        return false;
    }
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* src = pixels + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        std::fwrite(row, 1, static_cast<size_t>(w) * 3, f);
    }
    std::fclose(f);
    std::free(row);
    std::free(pixels);
    return true;
}
