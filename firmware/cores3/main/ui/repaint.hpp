#pragma once
#include "surface.hpp"
#include "typography.hpp"
#include <array>
#include <cstddef>

namespace qb::ui {
inline constexpr int band_height = 16;
inline constexpr size_t band_count = 240 / band_height;
inline uint16_t rgb565(uint32_t color) {
    return uint16_t(((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) | ((color >> 3) & 0x001f));
}

class BufferedSurface : public Surface {
public:
    void paint(Rect dirty, const Painter& draw) override {
        dirty = intersection(dirty, display_bounds);
        if (!dirty.w || !dirty.h) return;
        for (int y = dirty.y; y < dirty.y + dirty.h; y += band_height) {
            band_ = {dirty.x, y, dirty.w, std::min(band_height, dirty.y + dirty.h - y)};
            clipping_ = band_;
            std::fill_n(pixels_.begin(), band_.w * band_.h, uint16_t(0));
            draw(*this);
            blit(band_, pixels_.data());
        }
        band_ = clipping_ = {0, 0, 0, 0};
        // Do not retain card bytes or keyboard text in the scratch buffer.
        volatile uint16_t* pixels = pixels_.data();
        for (size_t i = 0; i < pixels_.size(); ++i) pixels[i] = 0;
    }
    void clip(Rect rect) override { clipping_ = intersection(rect, band_); }
    void fill(Rect rect, uint32_t color) override {
        rect = intersection(rect, clipping_);
        if (!rect.w || !rect.h) return;
        for (int y = rect.y; y < rect.y + rect.h; ++y)
            std::fill_n(pixels_.begin() + (y - band_.y) * band_.w + rect.x - band_.x, rect.w, rgb565(color));
    }
    void text(int x, int y, const char* value, unsigned style, uint32_t color, uint32_t background) override {
        const Rect visible = intersection({x, y, text_width(value, style), text_height(style)}, clipping_);
        if (!visible.w || !visible.h) return;
        raster_text(x, y, value, style, color, background, [&](int px, int py, int width, uint32_t rgb) {
            fill({px, py, width, 1}, rgb);
        });
    }
protected:
    // Implementations must finish consuming this RGB565 buffer before returning.
    virtual void blit(Rect, const uint16_t*) = 0;
private:
    std::array<uint16_t, 320 * band_height> pixels_{};
    Rect band_{}, clipping_{};
};

class SceneDamage {
public:
    void reset() { valid_ = false; }
    void paint(Surface& surface, const Surface::Painter& draw, bool force) {
        Fingerprints next;
        draw(next);
        for (size_t band = 0; band < band_count; ++band) {
            if (force || !valid_ || next.values[band] != previous_[band])
                surface.paint({0, int(band) * band_height, 320, band_height}, draw);
        }
        previous_ = next.values;
        valid_ = true;
    }
private:
    class Fingerprints final : public Surface {
    public:
        std::array<uint64_t, band_count> values;
        Fingerprints() { values.fill(14695981039346656037ULL); }
        void clip(Rect rect) override { clipping_ = intersection(rect, display_bounds); }
        void fill(Rect rect, uint32_t color) override { add(rect, 0, color, 0, 0, ""); }
        void text(int x, int y, const char* text, unsigned style, uint32_t color, uint32_t bg) override {
            add({x, y, text_width(text, style), text_height(style)}, 1, color, bg, style, text);
        }
    private:
        Rect clipping_ = display_bounds;
        void add(Rect rect, unsigned kind, uint32_t color, uint32_t bg, unsigned style, const char* text) {
            const Rect visible = intersection(rect, clipping_);
            if (!visible.w || !visible.h) return;
            for (int band = visible.y / band_height; band <= (visible.y + visible.h - 1) / band_height; ++band) {
                auto& hash = values[size_t(band)];
                const auto byte = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ULL; };
                for (uint32_t value : {uint32_t(rect.x), uint32_t(rect.y), uint32_t(rect.w), uint32_t(rect.h),
                                       uint32_t(visible.x), uint32_t(visible.y), uint32_t(visible.w), uint32_t(visible.h),
                                       uint32_t(kind), color, bg, uint32_t(style)}) {
                    for (unsigned shift = 0; shift < 32; shift += 8) byte(uint8_t(value >> shift));
                }
                for (const char* cursor = text; *cursor; ++cursor) byte(uint8_t(*cursor));
                byte(0);
            }
        }
    };
    std::array<uint64_t, band_count> previous_{};
    bool valid_ = false;
};
}
