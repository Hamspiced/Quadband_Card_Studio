#pragma once
#include "typeface.hpp"

namespace qb::ui {
inline const typeface::Glyph& glyph(unsigned char c, unsigned style) {
    if (c < 32 || c > 126) c = '?';
    return (style == 1 ? typeface::small_glyphs : typeface::large_glyphs)[c - 32];
}
inline int text_height(unsigned style) {
    return style == 1 ? typeface::small_height : typeface::large_height;
}
inline int text_width(const char* text, unsigned style) {
    int width = 0;
    while (*text) width += glyph(static_cast<unsigned char>(*text++), style).advance;
    return width;
}
inline uint32_t mix_color(uint32_t front, uint32_t back, unsigned alpha) {
    uint32_t color = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8)
        color |= (((((front >> shift) & 255) * alpha + ((back >> shift) & 255) * (255 - alpha) + 127) / 255) << shift);
    return color;
}
template<class Pixel>
void raster_text(int x, int y, const char* text, unsigned style, uint32_t fg, uint32_t bg, Pixel pixel) {
    const auto* coverage = style == 1 ? typeface::small_pixels : typeface::large_pixels;
    while (*text) {
        const auto& g = glyph(static_cast<unsigned char>(*text++), style);
        for (int row = 0; row < text_height(style); ++row) {
            int column = 0;
            while (column < g.advance) {
                const unsigned alpha = coverage[g.offset + row * g.advance + column];
                const int begin = column++;
                while (column < g.advance && coverage[g.offset + row * g.advance + column] == alpha) ++column;
                if (alpha) pixel(x + begin, y + row, column - begin, mix_color(fg, bg, alpha));
            }
        }
        x += g.advance;
    }
}
}
