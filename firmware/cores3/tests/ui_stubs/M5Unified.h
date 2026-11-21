#pragma once
#include <array>
#include <cassert>
#include <cstdint>

struct TestDisplay {
    std::array<uint16_t, 320 * 240> pixels{};
    unsigned transfers = 0, transactions = 0, completed = 0, transferred_pixels = 0;
    int x = 0, y = 0, width = 0, height = 0;
    void setRotation(int value) { assert(value == 1); }
    void setColorDepth(int value) { assert(value == 16); }
    void startWrite() { ++transactions; }
    void endWrite() { assert(transactions); --transactions; }
    void setAddrWindow(int px, int py, int w, int h) {
        assert(transactions && px >= 0 && py >= 0 && w > 0 && h > 0 && h <= 16);
        assert(px + w <= 320 && py + h <= 240);
        x = px; y = py; width = w; height = h;
    }
    void writePixels(const uint16_t* data, int count, bool swap) {
        assert(swap && count == width * height);
        ++transfers; transferred_pixels += unsigned(count);
        for (int row = 0; row < height; ++row)
            for (int col = 0; col < width; ++col)
                pixels[size_t((y + row) * 320 + x + col)] = data[row * width + col];
    }
    void waitDMA() { ++completed; }
};
struct TestM5 { TestDisplay Display; };
inline TestM5 M5;
