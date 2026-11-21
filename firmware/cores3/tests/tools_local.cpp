#include "../main/ui/tools.hpp"
#include "../main/ui/controller.hpp"
#include "../main/ui/typography.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace qb;
using namespace qb::local;
using namespace qb::ui;
namespace {
template<size_t N> void put(std::array<char, N>& out, const char* text) {
    std::snprintf(out.data(), out.size(), "%s", text);
}
class Recorder final : public Surface {
public:
    std::vector<std::string> labels;
    std::vector<std::pair<int, std::string>> rows;
    std::vector<uint8_t> pixels = std::vector<uint8_t>(320 * 240 * 3);
    unsigned calls = 0;
    Rect clipping = display_bounds;
    void clip(Rect rect) override {
        assert(rect.x >= 0 && rect.y >= 0 && rect.x + rect.w <= 320 && rect.y + rect.h <= 240);
        clipping = rect;
        ++calls;
    }
    void fill(Rect rect, uint32_t color) override {
        assert(rect.x >= 0 && rect.y >= 0 && rect.w >= 0 && rect.h >= 0);
        assert(rect.x + rect.w <= 320 && rect.y + rect.h <= 240);
        ++calls;
        for (int y = rect.y; y < rect.y + rect.h; ++y)
            for (int x = rect.x; x < rect.x + rect.w; ++x) pixel(x, y, color);
    }
    void pixel(int x, int y, uint32_t color) {
        assert(x >= 0 && x < 320 && y >= 0 && y < 240);
        if (!clipping.contains(x, y)) return;
        const size_t pos = size_t(y * 320 + x) * 3;
        pixels[pos] = uint8_t(color >> 16); pixels[pos + 1] = uint8_t(color >> 8); pixels[pos + 2] = uint8_t(color);
    }
    void text(int x, int y, const char* text, unsigned style, uint32_t fg, uint32_t bg) override {
        assert(x >= 0 && y >= 0 && x + text_width(text, style) <= 320);
        assert(y + text_height(style) <= 240);
        ++calls; labels.emplace_back(text);
        // Band replay may rasterize the same logical line more than once.
        const auto row = std::make_pair(y, std::string(text));
        if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
        raster_text(x, y, text, style, fg, bg, [&](int px, int py, int w, uint32_t color) {
            for (int col = 0; col < w; ++col) pixel(px + col, py, color);
        });
    }
    bool saw(const char* text) const {
        for (const auto& label : labels) if (label.find(text) != std::string::npos) return true;
        return false;
    }
    void ppm(const std::filesystem::path& filename) const {
        std::ofstream stream(filename, std::ios::binary); assert(stream);
        stream << "P6\n320 240\n255\n";
        stream.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
    }
};
struct Fixture {
    View view;
    ToolsUi ui;
    Recorder surface;
    explicit Fixture(Tool tool) {
        view.tool = tool; view.reader_present = true; view.battery = 81;
        ui.open(tool);
    }
    Intent tap(int x, int y) { return ui.touch(x, y, view); }
    void paint(bool force = false) { ui.paint(surface, view, force); }
    void changed() { ++view.revision; paint(); }
    void expect(int x, int y, Command command) { assert(tap(x, y).command == command); }
};
void type(Fixture& f, const char* text, InputKind kind) {
    InputEditor probe; probe.open("", "", 256, kind);
    for (; *text; ++text) {
        bool found = false;
        for (size_t i = 0; i < probe.key_count(); ++i) {
            const auto& key = probe.keys()[i];
            if (key.label[0] != *text || key.label[1]) continue;
            assert(f.tap(key.x + key.w / 2, key.y + key.h / 2).command == Command::None);
            found = true; break;
        }
        assert(found);
    }
}
void static_paint_and_close() {
    Fixture f(Tool::Hf); f.paint(); const unsigned calls = f.surface.calls;
    f.paint(); assert(f.surface.calls == calls);
    f.view.battery = 50; f.paint(); assert(f.surface.calls > calls);
    const unsigned after_battery = f.surface.calls;
    f.view.light_theme = true;
    f.tap(100, 45); f.paint(); assert(f.surface.calls > after_battery);
    assert(f.surface.saw("BAT 50%"));
    f.expect(-1, 20, Command::None); f.expect(320, 20, Command::None);
    f.expect(20, 240, Command::None); f.expect(20, 10, Command::Exit);
    assert(!f.ui.active()); const unsigned closed = f.surface.calls;
    f.paint(true); assert(f.surface.calls == closed);
}
void hf_memory_and_auth() {
    Fixture f(Tool::Hf);
    f.view.hf_family = HfFamily::Classic1K; f.view.hf_ready = true;
    f.view.hf_units = 64; f.view.hf_unit = 1; f.view.hf_unit_known = true; f.view.hf_writable = true;
    put(f.view.identity, "Classic 1K / 04AABBCC");
    put(f.view.hf_data, "00112233445566778899AABBCCDDEEFF");
    put(f.view.hf_value, "FFEEDDCCBBAA99887766554433221100");
    f.paint(); f.expect(20, 170, Command::HfRead);
    auto previous = f.tap(20, 95); assert(previous.command == Command::HfSelectUnit && previous.index == 0);
    auto next = f.tap(280, 95); assert(next.command == Command::HfSelectUnit && next.index == 2);
    f.expect(250, 170, Command::HfPrepareUnit);
    f.expect(130, 170, Command::None); f.paint(); assert(f.surface.saw("Desired memory bytes"));
    auto edited = f.tap(200, 220);
    assert(edited.command == Command::SetField && edited.field == Field::HfValue);
    assert(std::strcmp(edited.text.data(), f.view.hf_value.data()) == 0);
    f.view.hf_writable = false; f.expect(250, 170, Command::None);
    f.tap(110, 45);
    auto key_b = f.tap(210, 80);
    assert(key_b.command == Command::SetField && key_b.field == Field::HfKeyType && std::string(key_b.text.data()) == "97");
    auto factory = f.tap(20, 112);
    assert(factory.field == Field::HfKey && std::string(factory.text.data()) == "FFFFFFFFFFFF");
    auto standard = f.tap(20, 150);
    assert(standard.field == Field::HfStandardKeys && std::string(standard.text.data()) == "1");
    f.expect(210, 112, Command::None); f.paint();
    type(f, "A1B2C3D4E5F6", InputKind::Hex); f.surface.labels.clear(); f.paint();
    assert(f.surface.saw("************") && !f.surface.saw("A1B2C3D4E5F6"));
    auto key = f.tap(200, 220);
    assert(key.command == Command::SetField && key.field == Field::HfKey && std::string(key.text.data()) == "A1B2C3D4E5F6");
    f.tap(210, 112); auto empty = f.tap(200, 220);
    assert(empty.command == Command::SetField && empty.field == Field::HfKey && !empty.text[0]);
    f.tap(20, 185);
    auto mode = f.tap(230, 80);
    assert(mode.command == Command::SetField && mode.field == Field::NtagMode && std::string(mode.text.data()) == "2");
    f.tap(20, 116); type(f, "01020304", InputKind::Hex);
    auto pwd = f.tap(200, 220); assert(pwd.field == Field::NtagPassword && std::string(pwd.text.data()) == "01020304");
    f.tap(200, 116); type(f, "1234", InputKind::Hex);
    auto pack = f.tap(200, 220); assert(pack.field == Field::NtagPack && std::string(pack.text.data()) == "1234");
    f.tap(260, 45); f.expect(20, 160, Command::HfCapture);
    f.expect(200, 160, Command::None);
    f.view.hf_source = f.view.hf_copy_armed = true;
    f.expect(200, 160, Command::HfPrepareCopy);
    f.view.hf_copy_armed = false; f.expect(200, 160, Command::None);
}
void ndef_catalog_and_fields() {
    Fixture f(Tool::Hf); f.view.ndef_available = true; f.view.ndef_queue = 1; f.view.ndef_field_count = 8;
    for (unsigned i = 0; i < 19; ++i) {
        char title[24]; std::snprintf(title, sizeof(title), "Record type %u", i);
        put(f.view.ndef_kinds[i], title);
    }
    for (unsigned i = 0; i < 8; ++i) {
        put(f.view.ndef_fields[i].label, "Record field");
        put(f.view.ndef_fields[i].value, "current");
    }
    f.view.ndef_fields[0].choice_count = 2;
    put(f.view.ndef_fields[0].choices[0], "one"); put(f.view.ndef_fields[0].choices[1], "two");
    put(f.view.ndef_fields[0].value, "one");
    f.view.ndef_fields[1].secret = true; put(f.view.ndef_fields[1].value, "hidden-value");
    for (unsigned kind = 0; kind < 19; ++kind) {
        f.ui.open(Tool::Hf); f.tap(180, 45); f.tap(30, 162);
        for (unsigned page = 0; page < kind / 4; ++page) f.tap(280, 196);
        auto choice = f.tap(20, 78 + int(kind % 4) * 29);
        assert(choice.command == Command::SetField && choice.field == Field::NdefKind);
        assert(std::string(choice.text.data()) == std::to_string(kind));
    }
    f.ui.open(Tool::Hf); f.tap(180, 45); f.tap(30, 162); f.tap(20, 78);
    auto choice = f.tap(30, 92);
    assert(choice.field == Field::NdefField0 && std::string(choice.text.data()) == "two");
    f.surface.labels.clear(); f.paint();
    assert(!f.surface.saw("hidden-value") && f.surface.saw("********"));
    f.tap(30, 132); auto blank = f.tap(200, 220);
    assert(blank.field == Field::NdefField1 && !blank.text[0]);
    for (unsigned page = 0; page < 3; ++page) f.tap(280, 196);
    f.tap(30, 132); auto last = f.tap(200, 220);
    assert(last.field == Field::NdefField7 && std::string(last.text.data()) == "current");
    f.expect(20, 162, Command::HfAddRecord);
    f.expect(260, 162, Command::HfPrepareNdef);
    f.tap(180, 45); f.expect(130, 162, Command::HfClearRecords);
}
void bulk_controls() {
    Fixture f(Tool::Hf); f.view.ndef_queue = 3; f.view.bulk_bytes = 42;
    f.tap(180,45); f.tap(40,134); f.surface.labels.clear(); f.paint(true);
    assert(f.surface.saw("Overwrite: OFF") && f.surface.saw("Format: OFF") && f.surface.saw("Classic1K"));
    auto family = f.tap(40,78);
    assert(family.command == Command::SetField && family.field == Field::BulkFamily && std::string(family.text.data()) == "3");
    auto overwrite = f.tap(40,106); assert(overwrite.field == Field::BulkOverwrite && std::string(overwrite.text.data()) == "1");
    auto formatting = f.tap(180,106); assert(formatting.field == Field::BulkFormatting && std::string(formatting.text.data()) == "1");
    f.expect(40,178,Command::HfArmBulk);
    f.view.ndef_queue = 4; f.expect(40,178,Command::None);
    f.view.ndef_queue = 3; f.view.bulk_active = true; f.view.bulk_phase = 1;
    f.expect(40,78,Command::None); f.expect(40,106,Command::None); f.expect(180,106,Command::None);
    f.expect(40,178,Command::None); f.expect(140,178,Command::HfRead); f.expect(250,178,Command::None);
    f.expect(260,15,Command::Stop);
    f.view.bulk_phase = 2; f.view.hf_ready = true; f.expect(250,178,Command::HfPrepareBulk);
    f.view.bulk_phase = 4; f.expect(140,178,Command::None); f.expect(250,178,Command::None);
}
void uhf_inventory_memory_and_power() {
    Fixture f(Tool::Uhf); f.view.tag_count = 64; f.view.selected_tag = 0;
    for (unsigned i = 0; i < 64; ++i) {
        f.view.tags[i].size = 12; f.view.tags[i].epc.fill(uint8_t(i));
        f.view.tags[i].reads = i + 1; f.view.tags[i].rssi = -55;
    }
    f.expect(20, 194, Command::UhfStart);
    f.view.inventory = f.view.busy = true; f.expect(20, 78, Command::None);
    f.surface.labels.clear(); f.paint(true);
    assert(f.surface.saw("-55dBm") && !f.surface.saw("Working"));
    f.expect(270, 228, Command::None);
    f.expect(20, 194, Command::Stop);
    f.tap(260, 45);
    f.expect(20, 180, Command::None); f.expect(140, 180, Command::None); f.expect(250, 180, Command::None);
    f.tap(140, 45); f.expect(150, 182, Command::None); f.expect(260, 15, Command::Stop);
    f.view.inventory = f.view.busy = false; f.tap(40, 45);
    for (unsigned page = 0; page < 15; ++page) f.tap(280, 194);
    auto chosen = f.tap(20, 162);
    assert(chosen.command == Command::UhfSelectTag && chosen.index == 63);
    f.expect(150, 182, Command::UhfRead);
    f.view.uhf_memory = true; f.view.uhf_count = 32;
    for (unsigned i = 0; i < 256; ++i) f.view.uhf_data[i] = char("0123456789ABCDEF"[i % 16]);
    put(f.view.uhf_value, "AABB"); f.tap(240, 180);
    f.expect(200, 162, Command::UhfPrepareUser);
    f.tap(280, 196); f.tap(280, 196); f.paint(); assert(f.surface.saw("3 / 3"));
    f.tap(20, 162); auto edited = f.tap(200, 220);
    assert(edited.command == Command::SetField && edited.field == Field::UhfValue);
    f.view.uhf_bank = 2; f.expect(200, 162, Command::None);
    f.tap(150, 45); f.tap(30, 180);
    auto bank = f.tap(260, 80); assert(bank.field == Field::UhfBank && std::string(bank.text.data()) == "3");
    f.tap(20, 148); type(f, "AABBCCDD", InputKind::Hex);
    auto password = f.tap(200, 220); assert(password.field == Field::UhfPassword && std::string(password.text.data()) == "AABBCCDD");
    f.tap(20, 148); auto empty = f.tap(200, 220); assert(empty.field == Field::UhfPassword && !empty.text[0]);
    put(f.view.uhf_epc, "1122"); f.expect(20, 185, Command::UhfPrepareEpc);
    f.tap(260, 45); f.expect(20, 180, Command::UhfReadPower); f.expect(240, 180, Command::None);
    f.view.uhf_power_known = true; f.expect(240, 180, Command::UhfPreparePower);
    f.tap(140, 180); auto power = f.tap(200, 220);
    assert(power.field == Field::UhfPower && std::string(power.text.data()) == "18");
    f.view.tags[0].size = 62;
    std::string expected_epc;
    for (unsigned i = 0; i < 62; ++i) {
        f.view.tags[0].epc[i] = uint8_t(i); char byte[3];
        std::snprintf(byte, sizeof(byte), "%02X", i); expected_epc += byte;
    }
    f.tap(150, 45); f.expect(150, 182, Command::None);
    f.surface.rows.clear(); f.paint(true); assert(f.surface.saw("inventory-only"));
    std::string shown;
    for (const auto& row : f.surface.rows) if (row.first >= 66 && row.first < 132) shown += row.second;
    assert(shown == expected_epc);
}
void lf_capabilities_and_removal() {
    Fixture f(Tool::Lf); f.view.lf_present = false; f.view.lf_scan_mask = f.view.lf_write_mask = 255;
    f.expect(20, 150, Command::LfScan); f.expect(200, 150, Command::None);
    f.view.lf_present = true; f.expect(200, 150, Command::LfCapture);
    f.view.lf_card_valid = true;
    f.tap(150, 45); f.expect(200, 132, Command::LfTestId);
    put(f.view.lf_value, "0102030405");
    f.expect(20, 192, Command::None); f.view.lf_write_armed = true;
    f.expect(20, 192, Command::LfPrepareWrite);
    for (unsigned format : {2U, 7U}) {
        f.view.lf_format = format; f.expect(20, 192, Command::None);
    }
    f.view.lf_format = 0; f.view.lf_write_mask = 0; f.expect(20, 192, Command::None);
    f.view.lf_write_mask = 255; f.view.lf_scan_mask = 0; f.expect(20, 192, Command::None);
    f.view.lf_scan_mask = 255; f.view.lf_present = false; f.tap(260, 45);
    for (unsigned i = 0; i < 4; ++i) {
        auto format = f.tap(20, 78 + int(i) * 29);
        assert(format.field == Field::LfFormat && std::string(format.text.data()) == std::to_string(i));
    }
    f.tap(280, 196);
    auto last = f.tap(20, 165); assert(last.field == Field::LfFormat && std::string(last.text.data()) == "7");
    f.view.lf_scan_mask = 0; f.expect(20, 78, Command::None);
    f.view.lf_scan_mask = 255; f.view.lf_format = 2; f.tap(40, 45);
    f.tap(30, 190); auto hint = f.tap(200, 220); assert(hint.field == Field::LfHint);
    f.view.lf_format = 5; f.tap(150, 45); put(f.view.lf_value, "12345678");
    f.tap(20, 132); auto ascii = f.tap(200, 220);
    assert(ascii.field == Field::LfId && std::string(ascii.text.data()) == "12345678");
}
void modal_priority_and_stale_state() {
    Fixture f(Tool::Hf); f.view.review = true; f.view.review_token = 123;
    put(f.view.review_title, "Review write"); put(f.view.review_lines[0], "No automatic retries");
    f.paint();
    for (int y = -1; y <= 240; ++y) for (int x = -1; x <= 320; ++x) {
        auto actual = f.tap(x, y);
        const bool cancel = Rect{12, 184, 140, 44}.contains(x, y);
        const bool confirm = Rect{168, 184, 140, 44}.contains(x, y);
        assert(actual.command == (cancel ? Command::CancelReview : confirm ? Command::Confirm : Command::None));
        if (cancel || confirm) assert(actual.review_token == 123);
    }
    f.view.review_token = 456; f.expect(200, 200, Command::None);
    f.paint(); auto current = f.tap(200, 200); assert(current.review_token == 456);
    f.view.review_token = 0; f.expect(200, 200, Command::None);
    f.view.review = false; f.view.busy = true; f.view.done = std::numeric_limits<unsigned>::max(); f.view.total = 1;
    f.paint(true); f.expect(200, 200, Command::Stop); f.expect(200, 160, Command::None);
    f.view.busy = false; f.tap(110, 45); f.tap(200, 116);
    type(f, "ABC", InputKind::Hex);
    f.view.review = true; f.view.review_token = 9; f.paint(true);
    f.expect(10, 10, Command::None);
    f.view.review = false; f.expect(200, 220, Command::None);
    f.tap(200, 116); type(f, "ABC", InputKind::Hex); f.view.tool = Tool::Lf;
    f.expect(200, 220, Command::None); f.paint(true); assert(f.surface.saw("Waiting for tool"));
}
void malformed_projection_bounds() {
    for (bool light : {false, true}) {
        Fixture f(Tool::Hf);
        f.view.light_theme = light; f.view.identity.fill('W'); f.view.message.fill('W');
        f.view.hf_ready = f.view.hf_writable = f.view.hf_unit_known = true;
        f.view.hf_unit = f.view.hf_units = std::numeric_limits<unsigned>::max();
        f.view.hf_data.fill('W'); f.view.hf_value.fill('W'); f.paint(true);
        f.tap(130, 170); f.paint(true); f.tap(20, 220);
        for (auto& kind : f.view.ndef_kinds) kind.fill('W');
        f.view.ndef_field_count = 255; f.view.ndef_count = 0xffffffff; f.view.ndef_queue = 0xffffffff;
        for (auto& field : f.view.ndef_fields) {
            field.label.fill('W'); field.value.fill('W'); field.choice_count = 255;
            for (auto& choice : field.choices) choice.fill('W');
        }
        f.tap(180, 45); f.paint(true); f.tap(30, 162); f.paint(true);
        f.tap(20, 78); f.paint(true); f.tap(20, 92); f.paint(true);
        f.ui.open(Tool::Uhf); f.view.tool = Tool::Uhf;
        f.view.tag_count = 0xffffffff; f.view.selected_tag = 0xffffffff;
        for (auto& tag : f.view.tags) { tag.epc.fill(255); tag.size = 255; tag.reads = 0xffffffff; }
        f.view.uhf_data.fill('W'); f.paint(true);
        f.tap(20, 78); f.paint(true);
        f.view.selected_tag = 0; f.view.uhf_memory = true; f.paint(true);
        f.tap(230, 185); f.paint(true);
        f.ui.open(Tool::Lf); f.view.tool = Tool::Lf;
        f.view.lf_value.fill('W'); f.view.lf_actual.fill('W'); f.view.lf_format = 0xffffffff;
        f.paint(true); f.tap(150, 45); f.paint(true); f.tap(20, 132); f.paint(true);
        f.tap(20, 220); f.view.review = true; f.view.review_token = 1;
        f.view.review_title.fill('W'); for (auto& line : f.view.review_lines) line.fill('W');
        f.paint(true); f.expect(200, 200, Command::Confirm);
    }
}
void render_examples(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    Fixture f(Tool::Hf); f.view.hf_ready = f.view.hf_unit_known = f.view.hf_writable = true;
    f.view.hf_units = 64; f.view.hf_unit = 4;
    put(f.view.identity, "Classic 1K / 04A1B2C3"); put(f.view.hf_data, "00112233445566778899AABBCCDDEEFF");
    put(f.view.hf_value, "00112233445566778899AABBCCDDEEFF");
    f.paint(true); f.surface.ppm(root / "hf-memory.ppm");
    f.tap(110, 45); f.paint(true); f.surface.ppm(root / "hf-auth.ppm");
    f.tap(200, 116); type(f, "A1B2C3D4E5F6", InputKind::Hex);
    f.paint(true); f.surface.ppm(root / "masked-keyboard.ppm"); f.tap(20, 220);
    put(f.view.ndef_kinds[0], "Wi-Fi"); f.view.ndef_field_count = 3;
    put(f.view.ndef_fields[0].label, "Network SSID"); put(f.view.ndef_fields[0].value, "Guest network");
    put(f.view.ndef_fields[1].label, "Security"); put(f.view.ndef_fields[1].value, "WPA2");
    put(f.view.ndef_fields[2].label, "Password"); f.view.ndef_fields[2].secret = true;
    f.view.ndef_queue = 2; f.view.ndef_available = true;
    f.tap(180, 45); f.tap(20, 162); f.tap(20, 78);
    f.paint(true); f.surface.ppm(root / "nfc-record.ppm");
    f.tap(280, 196); f.paint(true); f.surface.ppm(root / "nfc-secret.ppm");
    f.view.review = true; f.view.review_token = 7;
    put(f.view.review_title, "Review Classic edit"); put(f.view.review_lines[0], "Target: 04A1B2C3 / block 4");
    put(f.view.review_lines[1], "16 bytes will be changed");
    put(f.view.review_lines[2], "Expected bytes rechecked first"); put(f.view.review_lines[3], "Do not remove until verified");
    f.paint(true); f.surface.ppm(root / "write-review.ppm");
    Fixture uhf(Tool::Uhf); uhf.view.tag_count = 4;
    for (unsigned i = 0; i < 4; ++i) {
        uhf.view.tags[i].size = 12; uhf.view.tags[i].epc.fill(uint8_t(0x30 + i));
        uhf.view.tags[i].rssi = int8_t(-45 - i); uhf.view.tags[i].reads = 10 + i;
    }
    uhf.paint(true); uhf.surface.ppm(root / "uhf-tags.ppm");
    uhf.view.selected_tag = 0; uhf.view.uhf_memory = true; uhf.view.uhf_count = 32;
    for (unsigned i = 0; i < 128; ++i) uhf.view.uhf_data[i] = "A1B2"[i % 4];
    put(uhf.view.uhf_value, "A1B2");
    uhf.tap(20, 78); uhf.paint(true); uhf.surface.ppm(root / "uhf-memory.ppm");
    uhf.tap(240, 185); uhf.paint(true); uhf.surface.ppm(root / "uhf-data.ppm");
    uhf.tap(240, 45); uhf.view.uhf_power_known = true;
    uhf.paint(true); uhf.surface.ppm(root / "uhf-power.ppm");
    Fixture lf(Tool::Lf); lf.view.lf_present = true; lf.view.lf_scan_mask = lf.view.lf_write_mask = 255;
    lf.view.lf_write_armed = true; put(lf.view.lf_value, "0102030405"); lf.tap(150, 45);
    lf.paint(true); lf.surface.ppm(root / "lf-id.ppm");
    lf.view.light_theme = true; lf.paint(); lf.surface.ppm(root / "lf-id-light.ppm");
    lf.tap(260, 45); lf.paint(true); lf.surface.ppm(root / "lf-formats-light.ppm");
    lf.view.busy = true; lf.view.done = 4; lf.view.total = 16;
    put(lf.view.message, "Reading and verifying. Stop at the next safe command boundary.");
    lf.paint(true); lf.surface.ppm(root / "busy-light.ppm");
}
}
int main(int argc, char** argv) {
    static_paint_and_close(); hf_memory_and_auth(); ndef_catalog_and_fields(); bulk_controls();
    uhf_inventory_memory_and_power(); lf_capabilities_and_removal(); modal_priority_and_stale_state();
    malformed_projection_bounds();
    if (argc == 2) render_examples(argv[1]);
    std::puts("Standalone tools UI tests passed");
}
