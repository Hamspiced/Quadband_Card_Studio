#include "runtime.hpp"
#include "ui.hpp"
#include "lvgl.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace qb;
using namespace qb::zero;
namespace {
std::array<uint8_t, 320 * 170 * 4> frame{};
std::array<uint8_t, 320 * 20 * 4> buffer{};
unsigned pixel_bytes = 2;
std::string suffix;
void flush(lv_display_t* display, const lv_area_t* area, uint8_t* bytes) {
    const int width = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            for (unsigned b = 0; b < pixel_bytes; ++b)
                frame[size_t(y * 320 + x) * pixel_bytes + b] =
                    bytes[size_t((y - area->y1) * width + x - area->x1) * pixel_bytes + b];
    lv_display_flush_ready(display);
}
lv_obj_t* find(lv_obj_t* root, const std::string& text) {
    if (lv_obj_check_type(root, &lv_label_class) && text == lv_label_get_text(root)) return root;
    for (uint32_t n = 0; n < lv_obj_get_child_count(root); ++n)
        if (auto* value = find(lv_obj_get_child(root, int32_t(n)), text)) return value;
    return nullptr;
}
void screenshot(const char* name) {
    lv_refr_now(nullptr);
    const auto folder = std::filesystem::path(QB_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(folder);
    const std::string base = std::string(name) + suffix;
    FILE* raw = std::fopen((folder / (base + (pixel_bytes == 2 ? ".rgb565" : ".argb8888"))).c_str(), "wb");
    assert(raw);
    assert(std::fwrite(frame.data(), 1, 320 * 170 * pixel_bytes, raw) == 320 * 170 * pixel_bytes);
    assert(std::fclose(raw) == 0);
    FILE* output = std::fopen((folder / (base + ".ppm")).c_str(), "wb"); assert(output);
    std::fprintf(output, "P6\n320 170\n255\n");
    for (unsigned i = 0; i < 320 * 170; ++i) {
        uint8_t rgb[3];
        if (pixel_bytes == 2) {
            const uint16_t value = uint16_t(frame[i * 2]) | uint16_t(frame[i * 2 + 1]) << 8;
            rgb[0] = uint8_t((value >> 11) * 255 / 31);
            rgb[1] = uint8_t(((value >> 5) & 63) * 255 / 63);
            rgb[2] = uint8_t((value & 31) * 255 / 31);
        } else {
            rgb[0] = frame[i * 4 + 2]; rgb[1] = frame[i * 4 + 1]; rgb[2] = frame[i * 4];
        }
        assert(std::fwrite(rgb, 1, 3, output) == 3);
    }
    assert(std::fclose(output) == 0);
}
void workflow(bool light) {
    Runtime runtime(true); assert(runtime.connect({}) && runtime.wait_idle(2000));
    ApplicationUi ui(lv_screen_active(), {
        [&](const local::Intent& intent) { return runtime.submit(intent); },
        [&](const ReaderConfig& config) { return runtime.connect(config); },
        [&] { runtime.stop(); }, [] {}, [&](const ReaderConfig& config) { return runtime.detect(config); }
    });
    const auto update = [&] {
        auto snapshot = runtime.snapshot(); snapshot.view.light_theme = light;
        ui.update(snapshot); lv_refr_now(nullptr);
    };
    const auto tap = [&](Key key, char c = 0) {
        update(); ui.key({key, c, true}); ui.key({key, c, false});
        assert(runtime.wait_idle(2000)); update();
    };
    const auto shortcut = [&](char c) { tap(Key::Character, c); };
    const auto choose = [&](const std::string& title) {
        update(); auto* label = find(lv_screen_active(), title);
        if (!label) std::fprintf(stderr, "Missing integration control: %s\n", title.c_str());
        assert(label);
        auto* button = lv_obj_get_parent(label);
        auto* group = static_cast<lv_group_t*>(lv_obj_get_group(button)); assert(group);
        for (unsigned n = 0; lv_group_get_focused(group) != button && n < 128; ++n) tap(Key::Tab);
        assert(lv_group_get_focused(group) == button); tap(Key::Enter);
    };
    update(); screenshot("home");
    shortcut('1'); shortcut('R');
    assert(runtime.snapshot().view.hf_ready); screenshot("hf-memory");
    shortcut('U'); choose("Go to unit"); tap(Key::Home); tap(Key::Delete); shortcut('4'); tap(Key::Enter);
    shortcut('M'); shortcut('E'); tap(Key::Home);
    for (unsigned i = 0; i < 32; ++i) tap(Key::Delete);
    for (char c : std::string("01020304")) shortcut(c);
    tap(Key::Enter); shortcut('W');
    assert(runtime.snapshot().view.review); screenshot("write-review");
    tap(Key::Escape); assert(!runtime.snapshot().view.review);
    shortcut('N'); screenshot("ndef");
    tap(Key::Escape); shortcut('A'); choose("Supplied key: ************");
    for (char c : std::string("A1B2C3D4E5F6")) shortcut(c);
    screenshot("masked-keyboard"); tap(Key::Escape);
    shortcut('2'); shortcut('R');
    for (unsigned i = 0; i < 100 && !runtime.snapshot().view.tag_count; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    update(); assert(runtime.snapshot().view.inventory && runtime.snapshot().view.tag_count == 1);
    screenshot("uhf-live"); shortcut('R');
    assert(!runtime.snapshot().view.inventory && runtime.snapshot().view.tag_count == 1);
    screenshot("uhf-results");
    // Primary Start is focused after Stop. The next focusable control is the tag.
    tap(Key::Home); tap(Key::Tab); tap(Key::Enter);
    assert(runtime.snapshot().view.selected_tag == 0);
    assert(find(lv_screen_active(), "UHF Memory"));
    shortcut('R'); assert(runtime.snapshot().view.uhf_memory); screenshot("uhf-memory");
    shortcut('3'); shortcut('R');
    assert(runtime.snapshot().view.lf_present); screenshot("lf-scan");
    shortcut('1'); shortcut('N');
    local::Intent field; field.command = local::Command::SetField;
    field.field = local::Field::NdefField0;
    const std::string draft = "Bulk demo";
    std::copy(draft.begin(),draft.end(),field.text.begin());
    assert(runtime.submit(field) && runtime.wait_idle(2000)); update();
    choose("Add record to RAM");
    assert(runtime.snapshot().view.ndef_queue == 1);
    shortcut('B'); choose("Exact target: Classic1K");
    assert(runtime.snapshot().view.bulk_family == local::HfFamily::Ntag213);
    screenshot("bulk-draft");
    choose("Form >"); choose("Form >");
    choose("Arm frozen job");
    assert(runtime.snapshot().view.bulk_active && runtime.snapshot().view.bulk_phase == 1);
    choose("< Form");
    screenshot("bulk-removal");
    choose("Form >");
    choose("Stop job"); assert(!runtime.snapshot().view.bulk_active);
}
}
int main() {
    lv_init();
    for (bool argb : {false, true}) {
        pixel_bytes = argb ? 4 : 2;
        auto* display = lv_display_create(320, 170);
        lv_display_set_color_format(display, argb ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display, buffer.data(), nullptr, 320 * 20 * pixel_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, flush);
        for (bool light : {false, true}) {
            suffix = std::string(light ? "-light" : "-dark") + (argb ? "-argb8888" : "-rgb565");
            workflow(light);
        }
        lv_display_delete(display);
    }
    lv_deinit();
    std::puts("Worker + keyboard workflows and 44 native RGB565/ARGB8888 Light/Dark renders passed.");
}
