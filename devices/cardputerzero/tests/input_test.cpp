#include "input.hpp"
#include "framebuffer.hpp"
#include <cassert>
#include <iostream>

using namespace qb::zero;
int main() {
    KeyTranslator translator;
    assert(!translator.evdev(42, 1));
    auto upper = translator.evdev(30, 1); assert(upper && upper->character == 'A');
    assert(!translator.evdev(42, 0));
    auto released = translator.evdev(30, 0); assert(released && !released->pressed && released->character == 'A');
    translator.evdev(29, 1);
    assert(translator.evdev(46, 1)->key == Key::Stop);
    translator.evdev(29, 0);
    assert(translator.evdev(46, 0)->key == Key::Stop);
    assert(translator.evdev(28, 2)->repeat);
    assert(translator.evdev(103, 1)->key == Key::Up);
    for (unsigned code = 0; code < 2000; ++code) {
        for (int value = -1; value < 4; ++value) {
            const auto event = translator.evdev(code, value);
            if (event && event->key == Key::Character) assert(event->character >= 32 && event->character <= 126);
        }
    }
    translator.reset();
    assert(translator.evdev(30, 1)->character == 'a');
    FramebufferLayout layout{320,170,320,170,0,0,640,16,11,5,0,5,6,5,640*170};
    layout.validate(); assert(layout.convert(0xf800) == 0xf800 && layout.convert(0x07e0) == 0x07e0);
    layout = {320,170,400,200,8,2,1600,32,16,8,0,8,8,8,1600*200};
    layout.validate(); assert(layout.convert(0xf800) == 0xff0000 && layout.convert(0x001f) == 0x0000ff);
    for (unsigned failure = 0; failure < 5; ++failure) {
        auto bad = layout;
        if (failure == 0) bad.stride = 1;
        if (failure == 1) bad.size = 1;
        if (failure == 2) bad.red_offset = 31;
        if (failure == 3) bad.green_offset = bad.red_offset;
        if (failure == 4) bad.width = 640;
        bool rejected = false;
        try { bad.validate(); } catch (...) { rejected = true; }
        assert(rejected);
    }
    std::cout << "Input rollover, modifier releases/repeats, bounded mapping and framebuffer layouts passed.\n";
}
