#include "core/font.hpp"

namespace {
const Glyph* find_in_range(const Glyph* glyphs, int count, uint32_t cp, uint8_t weight) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const Glyph& g = glyphs[mid];
        if (g.codepoint == cp && g.weight == weight) return &g;
        if (g.codepoint < cp || (g.codepoint == cp && g.weight < weight)) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}
} // namespace

const Glyph* font_find(uint32_t codepoint, uint8_t weight) {
    const Glyph* g = find_in_range(kGlyphs, kGlyphCount, codepoint, weight);
    if (!g && weight != 0) g = find_in_range(kGlyphs, kGlyphCount, codepoint, 0);
    if (g) return g;
    return find_in_range(kGlyphs, kGlyphCount, '?', weight);
}

uint32_t utf8_next(const char*& p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == 0) return 0;
    ++p;
    if (c < 0x80) return c;

    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return 0xFFFD;

    for (int i = 0; i < extra; ++i) {
        const unsigned char cc = static_cast<unsigned char>(*p);
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
        ++p;
    }
    return cp;
}

int utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

float font_line_height(float size) {
    return static_cast<float>(kFontAscent + kFontDescent) * size / static_cast<float>(kFontBakeSize);
}

float font_ascent(float size) {
    return static_cast<float>(kFontAscent) * size / static_cast<float>(kFontBakeSize);
}

float text_width(const char* text, float size, uint8_t weight) {
    if (!text) return 0.0f;
    float w = 0.0f;
    const char* p = text;
    while (*p) {
        const uint32_t cp = utf8_next(p);
        if (cp == 0) break;
        const Glyph* g = font_find(cp, weight);
        if (g) w += g->advance;
    }
    return w * size / static_cast<float>(kFontBakeSize);
}
