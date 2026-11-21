#include "controller.hpp"
#include "typography.hpp"
#include "repaint.hpp"
#include <M5Unified.h>

namespace qb::ui {
namespace {
class DisplaySurface final : public BufferedSurface {
protected:
    void blit(Rect rect, const uint16_t* pixels) override {
        M5.Display.setAddrWindow(rect.x, rect.y, rect.w, rect.h);
        M5.Display.writePixels(pixels, rect.w * rect.h, true);
        M5.Display.waitDMA();
    }
};
DisplaySurface display;
Controller controller(display);
}
void initialize() {
    M5.Display.setRotation(1);
    M5.Display.setColorDepth(16);
    M5.Display.startWrite();
    controller.initialize();
    M5.Display.endWrite();
}
void update(const UiSnapshot& state) {
    M5.Display.startWrite();
    controller.update(state);
    M5.Display.endWrite();
}
UiAction touch(int x, int y) {
    M5.Display.startWrite();
    const auto action = controller.touch(x, y);
    M5.Display.endWrite();
    return action;
}
local::Intent take_local_intent() { return controller.take_local_intent(); }
}
