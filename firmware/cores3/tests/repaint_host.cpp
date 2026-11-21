#include "../main/ui/controller.hpp"
#include "../main/ui/repaint.hpp"
#include <M5Unified.h>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace qb;
using namespace qb::ui;
namespace {
class Reference final : public Surface {
public:
    std::vector<uint16_t> pixels = std::vector<uint16_t>(320 * 240);
    Rect clipping = display_bounds;
    void clip(Rect rect) override { clipping = intersection(rect, display_bounds); }
    void fill(Rect rect, uint32_t color) override {
        rect = intersection(rect, clipping);
        for (int y = rect.y; y < rect.y + rect.h; ++y)
            for (int x = rect.x; x < rect.x + rect.w; ++x) pixels[size_t(y * 320 + x)] = rgb565(color);
    }
    void text(int x, int y, const char* value, unsigned style, uint32_t fg, uint32_t bg) override {
        raster_text(x, y, value, style, fg, bg, [&](int px, int py, int width, uint32_t color) {
            fill({px, py, width, 1}, color);
        });
    }
};
class Bands final : public BufferedSurface {
public:
    std::vector<uint16_t> pixels = std::vector<uint16_t>(320 * 240);
    std::vector<Rect> transfers;
    unsigned area = 0;
    void reset_counts() { transfers.clear(); area = 0; }
protected:
    void blit(Rect rect, const uint16_t* data) override {
        assert(rect.x >= 0 && rect.y >= 0 && rect.w > 0 && rect.h > 0 && rect.h <= band_height);
        assert(rect.x + rect.w <= 320 && rect.y + rect.h <= 240);
        transfers.push_back(rect); area += unsigned(rect.w * rect.h);
        for (int row = 0; row < rect.h; ++row)
            for (int col = 0; col < rect.w; ++col)
                pixels[size_t((rect.y + row) * 320 + rect.x + col)] = data[row * rect.w + col];
    }
};
void compositor() {
    assert(rgb565(0xff0000) == 0xf800 && rgb565(0x00ff00) == 0x07e0 && rgb565(0x0000ff) == 0x001f);
    assert(sizeof(Bands) < 11000);
    Bands buffered; Reference reference;
    const Surface::Painter draw = [](Surface& surface) {
        surface.fill(display_bounds, 0x14243a);
        surface.fill({20, 17, 90, 33}, 0xff0000);
        surface.text(24, 24, "Text crosses bands", 2, 0x5ce0c1, 0x14243a);
        surface.clip({0, 32, 300, 37});
        surface.fill({-10, 28, 340, 34}, 0x123456);
        surface.clip(display_bounds);
        surface.text(2, 225, "Last band", 1, 0xffffff, 0x14243a);
    };
    buffered.paint(display_bounds, draw); reference.paint(display_bounds, draw);
    assert(buffered.pixels == reference.pixels && buffered.transfers.size() == band_count);
    buffered.reset_counts();
    buffered.paint({7, 23, 39, 43}, draw); reference.paint({7, 23, 39, 43}, draw);
    assert(buffered.pixels == reference.pixels && buffered.transfers.size() == 3 && buffered.area == 39 * 43);
    buffered.reset_counts();
    buffered.paint({320, 240, 12, 12}, draw);
    assert(buffered.transfers.empty());
    const auto old = buffered.pixels;
    const Rect dirty{7, 23, 39, 43};
    buffered.paint(dirty, [](Surface& surface) {
        surface.clip(display_bounds);
        surface.fill(display_bounds, 0x00ff00);
    });
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x)
            assert(buffered.pixels[size_t(y * 320 + x)] ==
                (dirty.contains(x, y) ? rgb565(0x00ff00) : old[size_t(y * 320 + x)]));
}
struct Tools {
    Bands surface;
    ToolsUi ui;
    local::View view;
    explicit Tools(local::Tool tool) {
        ui.open(tool); view.tool = tool; view.reader_present = true; view.battery = 81;
    }
    void paint() { ++view.revision; ui.paint(surface, view); }
    void equivalent() {
        const auto incremental = surface.pixels;
        Reference full;
        ui.paint(full, view, true);
        assert(incremental == full.pixels);
    }
};
void tools() {
    Tools t(local::Tool::Uhf);
    t.paint(); assert(t.surface.area == 320 * 240); t.equivalent();
    t.surface.reset_counts();
    for (unsigned i = 0; i < 20; ++i) t.paint();
    assert(t.surface.transfers.empty());
    t.view.battery = 80; t.paint();
    const auto battery_area = t.surface.area;
    assert(battery_area > 0 && battery_area <= 320 * 32); t.equivalent();
    t.view.inventory = t.view.busy = true; t.view.tag_count = 4;
    for (auto& tag : t.view.tags) { tag.size = 12; tag.epc.fill(0x30); tag.rssi = -55; tag.reads = 999; }
    t.paint(); t.equivalent(); t.surface.reset_counts();
    t.view.tags[1].reads = 1; t.paint();
    const auto inventory_area = t.surface.area;
    assert(inventory_area > 0 && inventory_area <= 320 * 48); t.equivalent();
    t.surface.reset_counts(); t.view.tags[63].reads = 2; t.paint();
    assert(t.surface.transfers.empty());
    t.view.tag_count = 1; t.paint(); t.equivalent();
    std::snprintf(t.view.message.data(), t.view.message.size(), "Long status message to be replaced");
    t.paint(); t.equivalent();
    std::snprintf(t.view.message.data(), t.view.message.size(), "Ready");
    t.surface.reset_counts(); t.paint();
    assert(t.surface.area > 0 && t.surface.area <= 320 * 32); t.equivalent();
    t.view.inventory = false; t.view.done = 1; t.view.total = 10; t.paint(); t.equivalent();
    t.surface.reset_counts(); t.view.done = 2; t.paint();
    assert(t.surface.area > 0 && t.surface.area <= 320 * 64); t.equivalent();
    t.view.busy = false; t.view.review = true; t.view.review_token = 10;
    t.paint(); t.equivalent(); t.surface.reset_counts();
    t.view.review_token = 11;
    assert(t.ui.touch(200, 200, t.view).command == local::Command::None);
    t.paint(); assert(t.surface.area == 320 * 240); t.equivalent();
    assert(t.ui.touch(200, 200, t.view).review_token == 11);
    t.view.light_theme = true; t.surface.reset_counts(); t.paint();
    assert(t.surface.area == 320 * 240); t.equivalent();
    Tools hf(local::Tool::Hf); hf.paint();
    hf.ui.touch(110, 45, hf.view); hf.paint();
    hf.ui.touch(200, 116, hf.view); hf.paint(); hf.equivalent(); hf.surface.reset_counts();
    InputEditor keyboard; keyboard.open("", "", 12, InputKind::Hex, true);
    const auto& key = keyboard.keys()[0];
    hf.ui.touch(key.x + 2, key.y + 2, hf.view); hf.paint(); hf.equivalent();
    assert(hf.surface.area > 0 && hf.surface.area <= 320 * 48);
    std::printf("Redraw pixels: unchanged revisions=0; battery=%u; one inventory row=%u; full=%u\n",
                battery_area, inventory_area, 320U * 240);
}
void dashboard_and_overlay() {
    Bands buffered; Reference reference;
    Controller actual(buffered), expected(reference);
    UiSnapshot state;
    const auto update = [&] { actual.update(state); expected.update(state); assert(buffered.pixels == reference.pixels); };
    actual.initialize(); expected.initialize(); update();
    buffered.reset_counts(); update(); assert(buffered.transfers.empty());
    state.battery_percent = 77; update(); assert(buffered.area < 320 * 32);
    state.mode = WorkMode::Standalone; update();
    state.local.tool = local::Tool::Hf; ++state.local.revision; update();
    assert(actual.touch(200, 90) == expected.touch(200, 90));
    update(); const auto before = buffered.pixels;
    state.pairing_pending = true; state.pairing_code = 123456; update();
    assert(buffered.pixels != before);
    state.pairing_pending = false; update(); assert(buffered.pixels == before);
    state.transition_pending = true; state.transition_target = WorkMode::Home; update();
    state.transition_pending = false; update(); assert(buffered.pixels == before);
    actual.touch(20, 15); expected.touch(20, 15); update();
    state.light_theme = true; update();
}
void adapter() {
    ui::initialize();
    assert(M5.Display.transfers == band_count && M5.Display.transactions == 0);
    assert(M5.Display.transfers == M5.Display.completed);
    UiSnapshot state; ui::update(state);
    const auto count = M5.Display.transfers;
    ui::update(state); assert(M5.Display.transfers == count);
    assert(M5.Display.pixels[100 * 320] == rgb565(0x060c14));
    assert(M5.Display.pixels[195 * 320 + 20] == rgb565(0x00dfbd));
    assert(M5.Display.transactions == 0);
}
}
int main() {
    compositor(); tools(); dashboard_and_overlay(); adapter();
    std::puts("Buffered compositor, clipping, incremental/full pixels, reviews, overlays and actual LCD adapter passed.");
}
