#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>

namespace qb::ui {
struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
inline Rect intersection(Rect a, Rect b) {
    const int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0, std::min(a.y + a.h, b.y + b.h) - y)};
}
inline constexpr Rect display_bounds{0, 0, 320, 240};

// Paint callbacks replay a complete scene; the surface limits its visible damage.
class Surface {
public:
    using Painter = std::function<void(Surface&)>;
    virtual ~Surface() = default;
    virtual void clip(Rect) = 0;
    virtual void fill(Rect, uint32_t rgb) = 0;
    virtual void text(int x, int y, const char*, unsigned style, uint32_t rgb, uint32_t background) = 0;
    virtual void paint(Rect dirty, const Painter& draw) {
        dirty = intersection(dirty, display_bounds);
        if (!dirty.w || !dirty.h) return;
        clip(dirty);
        draw(*this);
        clip(display_bounds);
    }
};
}
