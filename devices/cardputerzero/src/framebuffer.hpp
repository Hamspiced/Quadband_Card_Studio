#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace qb::zero {
struct FramebufferLayout {
    unsigned width = 0, height = 0, virtual_width = 0, virtual_height = 0;
    unsigned xoffset = 0, yoffset = 0, stride = 0, bits = 0;
    unsigned red_offset = 0, green_offset = 0, blue_offset = 0;
    unsigned red_length = 0, green_length = 0, blue_length = 0;
    size_t size = 0;
    void validate() const {
        if (width != 320 || height != 170 || virtual_width > 8192 || virtual_height > 8192 ||
            (bits != 16 && bits != 32) || xoffset > virtual_width || yoffset > virtual_height ||
            width > virtual_width - xoffset || height > virtual_height - yoffset ||
            stride < virtual_width * (bits / 8) || !red_length || !green_length || !blue_length ||
            red_length > 8 || green_length > 8 || blue_length > 8 ||
            red_offset >= bits || green_offset >= bits || blue_offset >= bits ||
            red_length > bits - red_offset || green_length > bits - green_offset || blue_length > bits - blue_offset)
            throw std::runtime_error("Unsupported framebuffer geometry/color layout; expected a 320x170 RGB display.");
        const uint32_t r = ((1u << red_length) - 1) << red_offset;
        const uint32_t g = ((1u << green_length) - 1) << green_offset;
        const uint32_t b = ((1u << blue_length) - 1) << blue_offset;
        if ((r & g) || (r & b) || (g & b) ||
            size < size_t(yoffset + height - 1) * stride + size_t(xoffset + width) * (bits / 8))
            throw std::runtime_error("Framebuffer channels overlap or mapped storage is too small.");
    }
    uint32_t convert(uint16_t rgb) const {
        const auto scale = [](unsigned value, unsigned source_max, unsigned length) {
            return (value * ((1u << length) - 1) + source_max / 2) / source_max;
        };
        return (scale(rgb >> 11, 31, red_length) << red_offset) |
            (scale((rgb >> 5) & 63, 63, green_length) << green_offset) |
            (scale(rgb & 31, 31, blue_length) << blue_offset);
    }
};
}
