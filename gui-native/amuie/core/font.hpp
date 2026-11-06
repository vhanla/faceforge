#pragma once
#include <cstdint>

// Glyphs are pre-baked into a single 8-bit atlas. Text codepoints store a signed
// distance field (128 == glyph contour) that the shader reconstructs with
// fwidth-based AA, so downscaled text stays crisp at any UI size. Icon glyphs
// (U+E000..) keep pre-antialiased alpha and can use a larger bake_px.
struct Glyph {
    uint32_t codepoint;
    uint16_t x, y, w, h;
    int16_t off_x, off_y;
    uint16_t bake_px;
    uint8_t weight;  // 0 = regular, 1 = semibold
    float advance;
};

extern const uint8_t kAtlasPixels[];
extern const int kAtlasWidth;
extern const int kAtlasHeight;
extern const Glyph kGlyphs[];
extern const int kGlyphCount;
extern const int kFontAscent;
extern const int kFontDescent;
extern const int kFontBakeSize;

const Glyph* font_find(uint32_t codepoint, uint8_t weight = 0);
uint32_t utf8_next(const char*& p);
int utf8_encode(uint32_t codepoint, char* out);

float font_line_height(float size);
float font_ascent(float size);
float text_width(const char* text, float size, uint8_t weight = 0);
