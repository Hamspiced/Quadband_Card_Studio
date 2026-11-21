#include "ui.hpp"
#include "lvgl.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace qb;
using namespace qb::zero;
using local::Command;
using local::Field;
using local::Tool;

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "%s:%d check failed: %s\n", __FILE__, __LINE__, #expr); std::abort(); \
} } while (false)

namespace {
unsigned flushes = 0;
std::array<uint8_t, 320 * 170 * 4> pixels{};
void flush(lv_display_t* display, const lv_area_t*, uint8_t*) {
    ++flushes; lv_display_flush_ready(display);
}
template<size_t N> void put(std::array<char, N>& a, const std::string& s) {
    a.fill(0); std::copy_n(s.data(), std::min(N - 1, s.size()), a.data());
}
lv_obj_t* find_label(lv_obj_t* parent, const std::string& s, bool prefix = false) {
    if (lv_obj_check_type(parent, &lv_label_class)) {
        std::string actual(lv_label_get_text(parent));
        if ((!prefix && actual == s) || (prefix && actual.find(s) == 0)) return parent;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (auto* found = find_label(lv_obj_get_child(parent, int32_t(i)), s, prefix)) return found;
    return nullptr;
}
lv_obj_t* find_type(lv_obj_t* parent, const lv_obj_class_t* type) {
    if (lv_obj_check_type(parent, type)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (auto* found = find_type(lv_obj_get_child(parent, int32_t(i)), type)) return found;
    return nullptr;
}
bool visible_secret(lv_obj_t* parent, const std::string& secret) {
    if (lv_obj_check_type(parent, &lv_label_class) &&
        std::strstr(lv_label_get_text(parent), secret.c_str())) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (visible_secret(lv_obj_get_child(parent, int32_t(i)), secret)) return true;
    return false;
}
struct Fixture {
    Snapshot s;
    lv_obj_t* root = lv_screen_active();
    std::vector<local::Intent> intents;
    std::vector<ReaderConfig> connections;
    std::vector<ReaderConfig> detections;
    unsigned stops = 0, quits = 0;
    std::unique_ptr<ApplicationUi> ui;
    Fixture() {
        s.connected = true; s.view.reader_present = true; s.simulated = true;
        s.view.hf_ready = s.view.hf_unit_known = s.view.hf_writable = true;
        s.view.hf_units = 256; s.view.hf_unit_size = 16; s.view.hf_unit = 4;
        s.view.ndef_available = true; s.view.ndef_capacity = 1024;
        s.view.lf_scan_mask = 255; s.view.lf_write_mask = 0x7b;
        for (unsigned i = 0; i < 19; ++i) put(s.view.ndef_kinds[i], "Kind " + std::to_string(i));
        Callbacks cb;
        cb.submit = [this](const local::Intent& i) { intents.push_back(i); return true; };
        cb.connect = [this](const ReaderConfig& c) { connections.push_back(c); return true; };
        cb.stop = [this] { ++stops; };
        cb.quit = [this] { ++quits; };
        cb.detect = [this](const ReaderConfig& c) { detections.push_back(c); return true; };
        ui = std::make_unique<ApplicationUi>(root, std::move(cb)); ui->update(s);
    }
    ~Fixture() { ui.reset(); CHECK(lv_obj_get_child_count(root) == 0); }
    void press(Key k, char c = 0, bool repeat = false, bool shift = false) {
        ui->key({k, c, true, repeat, shift});
    }
    void release(Key k, char c = 0) { ui->key({k, c, false, false, false}); }
    void tap(Key k, char c = 0, bool shift = false) { press(k, c, false, shift); release(k, c); }
    void publish() { ++s.view.revision; ui->update(s); }
    void tool(Tool t) { s.view.tool = t; publish(); }
    lv_obj_t* button(const std::string& title) {
        auto* label = find_label(root, title);
        for (unsigned n = 0; !label && n < 12; ++n) {
            auto* prev = find_label(root, "< Form");
            if (!prev || lv_obj_has_state(lv_obj_get_parent(prev), LV_STATE_DISABLED)) break;
            focus("< Form"); tap(Key::Enter); label = find_label(root, title);
        }
        for (unsigned n = 0; !label && n < 12; ++n) {
            auto* next = find_label(root, "Form >");
            if (!next || lv_obj_has_state(lv_obj_get_parent(next), LV_STATE_DISABLED)) break;
            focus("Form >"); tap(Key::Enter); label = find_label(root, title);
        }
        if (!label) {
            std::fprintf(stderr, "Missing button: %s\n", title.c_str());
            const auto dump = [](auto&& self, lv_obj_t* node) -> void {
                if (lv_obj_check_type(node, &lv_label_class)) std::fprintf(stderr, "  %s\n", lv_label_get_text(node));
                for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) self(self, lv_obj_get_child(node, int32_t(i)));
            };
            dump(dump, root);
        }
        CHECK(label);
        auto* obj = lv_obj_get_parent(label); CHECK(lv_obj_check_type(obj, &lv_button_class)); return obj;
    }
    void focus(const std::string& title) {
        auto* obj = button(title); CHECK(!lv_obj_has_state(obj, LV_STATE_DISABLED));
        auto* group = static_cast<lv_group_t*>(lv_obj_get_group(obj)); CHECK(group);
        // Navigate actual native focus with Tab; never send pixel/touch coordinates.
        for (unsigned n = 0; lv_group_get_focused(group) != obj && n < 128; ++n) tap(Key::Tab);
        CHECK(lv_group_get_focused(group) == obj);
    }
    void choose(const std::string& title) { focus(title); tap(Key::Enter); }
    void type(const std::string& value) { for (char c : value) tap(Key::Character, c); }
    void replace(const std::string& value) {
        auto* area = find_type(root, &lv_textarea_class); CHECK(area);
        const size_t length = std::strlen(lv_textarea_get_text(area));
        tap(Key::Home);
        for (size_t i = 0; i < length; ++i) tap(Key::Delete);
        type(value); tap(Key::Enter);
    }
    void command(Command c) { CHECK(!intents.empty()); CHECK(intents.back().command == c); }
    void set(Field field, const std::string& value) {
        command(Command::SetField); CHECK(intents.back().field == field);
        CHECK(std::string(intents.back().text.data()) == value); publish();
    }
};

void navigation_and_connections() {
    Fixture f;
    auto* shell = lv_obj_get_child(f.root, 0);
    lv_obj_update_layout(shell);
    CHECK(lv_obj_get_width(shell) == 320 && lv_obj_get_height(shell) == 170);
    f.tap(Key::Character, 'S'); f.choose("Connections"); CHECK(f.intents.empty());
    CHECK(find_label(f.root, "Right Grove = UHF UART. Hat Grove = HF I2C."));
    f.choose("HF I2C path: "); f.type("/dev/i2c-1"); f.tap(Key::Enter);
    f.choose("Connect configured readers"); CHECK(f.connections.empty());
    f.choose("HF I2C connector confirmed: NO");
    f.choose("UHF serial path: "); f.type("/dev/ttyUSB9"); f.tap(Key::Enter);
    f.choose("Ultra USB serial: "); f.type("/dev/ttyACM4"); f.tap(Key::Enter);
    f.choose("HF address (0x28): 40"); f.replace("0x28");
    f.choose("UHF baud: 115200"); f.replace("230400");
    CHECK(f.intents.empty() && f.connections.empty());
    f.choose("Connect configured readers"); CHECK(f.connections.size() == 1);
    CHECK(f.connections[0].hf_i2c == "/dev/i2c-1");
    CHECK(f.connections[0].hf_address == 0x28 && f.connections[0].grove_i2c_confirmed);
    CHECK(f.connections[0].uhf_serial == "/dev/ttyUSB9" && f.connections[0].uhf_baud == 230400);
    CHECK(f.connections[0].ultra_serial == "/dev/ttyACM4");
    f.publish(); f.tap(Key::Escape); f.choose("[1] HF / NFC  SIM"); f.command(Command::OpenHf);
    CHECK(f.intents.size() == 1); // Opening a tool is never an RF read.
    f.tool(Tool::Hf); f.choose("[U] Unit"); CHECK(f.intents.size() == 1);
    f.choose("[R] Read"); f.command(Command::HfRead); f.publish();
    f.choose("Go to unit"); f.replace("255"); f.set(Field::HfUnit, "255");
    f.tap(Key::Escape); f.tap(Key::Escape); f.command(Command::Exit);
}
void secrets_and_text() {
    Fixture f; f.tool(Tool::Hf); f.choose("[A] Auth");
    f.choose("Supplied key: ************"); f.type("A1B2C3D4E5F6");
    auto* area = find_type(f.root, &lv_textarea_class); CHECK(area);
    CHECK(lv_textarea_get_password_mode(area));
    CHECK(lv_textarea_get_password_show_time(area) == 0);
    CHECK(!visible_secret(f.root, "A1B2"));
    CHECK(std::string(lv_label_get_text(lv_textarea_get_label(area))) == "************");
    f.tap(Key::Enter); f.set(Field::HfKey, "A1B2C3D4E5F6");
    CHECK(!visible_secret(f.root, "A1B2"));
    f.choose("Supplied key: ************");
    CHECK(std::string(lv_textarea_get_text(find_type(f.root, &lv_textarea_class))).empty());
    f.press(Key::Character, 'A'); f.press(Key::Character, 'B');
    f.release(Key::Character, 'A'); f.release(Key::Character, 'B');
    CHECK(std::string(lv_textarea_get_text(find_type(f.root, &lv_textarea_class))) == "AB");
    f.type("FFFFFF"); f.tap(Key::Escape); CHECK(f.intents.size() == 1);
    f.tap(Key::Escape); f.choose("[T] NTAG"); f.choose("Password: ********");
    f.type("01234567ZZ"); f.tap(Key::Enter); f.set(Field::NtagPassword, "01234567");
    f.choose("PACK: ****"); f.type("ABCD"); f.tap(Key::Enter); f.set(Field::NtagPack, "ABCD");
}
void bulk_ui() {
    Fixture f; f.tool(Tool::Hf); f.s.view.ndef_queue = 3; f.s.view.bulk_bytes = 42; f.publish();
    f.choose("[N] NDEF"); f.choose("[B] Bulk 1-3 records");
    f.choose("Exact target: Classic1K"); f.set(Field::BulkFamily,"3");
    f.s.view.bulk_family = local::HfFamily::Ntag213; f.publish();
    f.choose("Overwrite: OFF"); f.set(Field::BulkOverwrite,"1");
    f.choose("Canonical format: OFF"); f.set(Field::BulkFormatting,"1");
    f.s.view.ndef_queue = 4; f.publish();
    CHECK(lv_obj_has_state(f.button("Arm frozen job"),LV_STATE_DISABLED));
    f.s.view.ndef_queue = 3; f.publish(); f.choose("Arm frozen job"); f.command(Command::HfArmBulk);
    f.s.view.bulk_active = true; f.s.view.bulk_phase = 1; f.publish();
    CHECK(lv_obj_has_state(f.button("Exact target: NTAG213"),LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(f.button("Overwrite: OFF"),LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(f.button("Canonical format: OFF"),LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(f.button("Review bulk target"),LV_STATE_DISABLED));
    const auto before = f.intents.size();
    for (unsigned i = 0; i < 10; ++i) f.publish();
    CHECK(f.intents.size() == before);
    f.choose("[R] Read / removal"); f.command(Command::HfRead); f.publish();
    f.s.view.bulk_phase = 2; f.publish();
    f.choose("Review bulk target"); f.command(Command::HfPrepareBulk);
    f.s.view.review = true; f.s.view.review_token = 71;
    put(f.s.view.review_title,"Bulk NFC target"); f.publish();
    auto* cancel = f.button("Cancel (default)");
    CHECK(lv_group_get_focused(static_cast<lv_group_t*>(lv_obj_get_group(cancel))) == cancel);
    f.tap(Key::Escape); f.command(Command::CancelReview);
    f.s.view.review = false; f.s.view.bulk_phase = 1; f.publish();
    f.choose("Stop job"); CHECK(f.stops == 1);
}
void ndef_pagination() {
    Fixture f; f.tool(Tool::Hf); f.choose("[N] NDEF"); f.choose("[K] Kinds");
    for (unsigned page = 0; page < 5; ++page) {
        for (unsigned i = page * 4; i < std::min(19U, page * 4 + 4); ++i)
            CHECK(find_label(f.root, std::to_string(i + 1) + ". Kind " + std::to_string(i)));
        if (page < 4) f.choose("Next >");
    }
    f.choose("19. Kind 18"); f.set(Field::NdefKind, "18"); CHECK(f.intents.size() == 1);
    f.s.view.ndef_kind = 18; f.s.view.ndef_field_count = 8;
    for (unsigned i = 0; i < 8; ++i) {
        put(f.s.view.ndef_fields[i].label, "Field" + std::to_string(i));
        put(f.s.view.ndef_fields[i].value, "Value" + std::to_string(i));
    }
    f.s.view.ndef_fields[1].secret = true;
    put(f.s.view.ndef_fields[1].value, "MUST_NOT_APPEAR");
    f.publish();
    CHECK(!visible_secret(f.root, "MUST_NOT_APPEAR"));
    for (unsigned page = 0; page < 4; ++page) {
        const unsigned i = page * 2;
        f.choose("Field" + std::to_string(i) + ": Value" + std::to_string(i));
        f.replace("ASCII +/?&=42");
        f.set(Field(unsigned(Field::NdefField0) + i), "ASCII +/?&=42");
        if (page < 3) f.choose("Next >");
    }
    f.choose("Add record to RAM"); f.command(Command::HfAddRecord); f.publish();
    f.tap(Key::Escape); f.choose("[N] NDEF");
    f.s.view.ndef_count = 5; f.s.view.ndef_queue = 3;
    for (unsigned i = 0; i < 8; ++i) put(f.s.view.ndef_records[i], (i < 5 ? "Read " : "+ Draft ") + std::to_string(i));
    f.publish(); f.choose("[V] View");
    f.choose("Next >"); f.choose("Next >"); CHECK(find_label(f.root, "+ Draft 7"));
}
void uhf_and_lf() {
    Fixture f; f.tool(Tool::Uhf);
    CHECK(f.intents.empty()); f.choose("[R] Start"); f.command(Command::UhfStart); f.publish();
    f.s.view.tag_count = 64; f.s.view.tags[63].size = 4; f.s.view.tags[63].epc[0] = 0xab; f.publish();
    for (unsigned p = 0; p < 21; ++p) f.choose("Next >");
    f.choose("64. AB000000  0dBm x0"); f.command(Command::UhfSelectTag);
    CHECK(f.intents.back().index == 63); f.s.view.selected_tag = 63; f.publish();

    f.tap(Key::Character, 'A'); f.choose("Access password: ********"); f.type("1234ABCD"); f.tap(Key::Enter);
    f.set(Field::UhfPassword, "1234ABCD");
    f.tap(Key::Character, 'M'); f.tap(Key::Character, 'G'); f.choose("Count 1-64: 1"); f.replace("64"); f.set(Field::UhfCount, "64");
    f.choose("[R] Read memory"); f.command(Command::UhfRead); f.publish();
    f.s.view.uhf_memory = true; f.s.view.uhf_count = 32;
    put(f.s.view.uhf_data, std::string(256, 'A')); f.publish();
    f.tap(Key::Character, 'M'); f.choose("[D] Data");
    for (unsigned p = 0; p < 3; ++p) f.choose("Next >");
    CHECK(find_label(f.root, "Page 4/4"));
    f.choose("Edit User bytes (1-32 words)"); f.replace(std::string(128, 'B'));
    f.set(Field::UhfValue, std::string(128, 'B'));
    put(f.s.view.uhf_value, std::string(128, 'B')); f.publish();
    f.choose("Review User write"); f.command(Command::UhfPrepareUser); f.publish();
    f.tool(Tool::Lf); f.choose("[F] Formats");
    f.choose("Next >"); CHECK(find_label(f.root, "PAC / Stanley [R/W]"));
    f.choose("Next >"); f.choose("Auto scan (read only) [read only]");
    f.set(Field::LfFormat, "8");
    f.s.view.lf_format = 8; f.publish(); f.tap(Key::Escape);
    f.choose("[R] Scan"); f.command(Command::LfScan); f.publish();
    f.choose("[I] ID/write"); f.choose("Default test ID 0102030405");
    f.command(Command::LfTestId); f.s.view.lf_format = 0; f.publish();
    f.choose("Edit custom ID"); f.replace("0102030405"); f.set(Field::LfId, "0102030405");
    CHECK(lv_obj_has_state(f.button("Review T5577 write"), LV_STATE_DISABLED));
    f.s.view.lf_card_valid = true; f.s.view.lf_write_armed = true; f.publish();
    f.choose("Review T5577 write"); f.command(Command::LfPrepareWrite);
}
void live_inventory_updates() {
    Fixture f; f.tool(Tool::Uhf);
    f.choose("[M] Memory"); f.tap(Key::Character, 'A'); f.choose("Access password: ********");
    f.type("ABCD");
    f.s.view.inventory = f.s.view.busy = true; f.publish();
    CHECK(!find_type(f.root, &lv_textarea_class));
    CHECK(find_label(f.root, "Waiting for tags; inventory is active."));
    CHECK(find_label(f.root, "R/Esc stop | Tab inspect | Ctrl+C stop"));
    CHECK(!find_label(f.root, "Access password: ********"));
    CHECK(!find_label(f.root, "[R] Read memory"));
    f.s.view.tag_count = 65; // Defensive projection clamp: array capacity remains 64.
    for (unsigned i = 0; i < 64; ++i) {
        f.s.view.tags[i].size = 2;
        f.s.view.tags[i].epc[0] = uint8_t(i);
        f.s.view.tags[i].epc[1] = 0xab;
        f.s.view.tags[i].rssi = -45;
        f.s.view.tags[i].reads = i + 1;
    }
    f.publish();
    CHECK(find_label(f.root, "UHF live | 64/64 tags"));
    f.choose("1. 00AB\nRSSI -45 dBm | reads 1");
    CHECK(f.intents.empty() && f.connections.empty());
    lv_refr_now(nullptr);
    auto* shell = lv_obj_get_child(f.root, 0);
    const unsigned before = flushes;
    for (unsigned i = 0; i < 100; ++i) { f.publish(); lv_refr_now(nullptr); }
    CHECK(lv_obj_get_child(f.root, 0) == shell && flushes == before);
    // Hidden tag changes are not visible changes; do not flicker the current page.
    f.s.view.tags[63].reads = 999; f.publish(); lv_refr_now(nullptr);
    CHECK(lv_obj_get_child(f.root, 0) == shell && flushes == before);
    f.s.view.tags[0].rssi = -38; f.s.view.tags[0].reads = 7;
    f.publish(); lv_refr_now(nullptr);
    CHECK(flushes > before);
    CHECK(find_label(f.root, "1. 00AB\nRSSI -38 dBm | reads 7"));
    CHECK(!find_label(f.root, "1. 00AB\nRSSI -45 dBm | reads 1"));
    f.choose("Next >"); CHECK(find_label(f.root, "4. 03AB\nRSSI -45 dBm | reads 4"));
    for (unsigned i = 0; i < 20; ++i) f.choose("Next >");
    CHECK(find_label(f.root, "Page 22/22"));
    f.choose("64. 3FAB\nRSSI -45 dBm | reads 999");
    CHECK(lv_obj_has_state(f.button("Next >"), LV_STATE_DISABLED));
    f.choose("< Prev"); CHECK(find_label(f.root, "Page 21/22"));
    f.s.view.tags[60].size = 62;
    f.s.view.tags[60].epc.fill(0xff); f.publish();
    const std::string longest = "61. " + std::string(124, 'F') + "\nRSSI -45 dBm | reads 61";
    auto* longest_row = f.button(longest); lv_obj_update_layout(longest_row);
    CHECK(lv_obj_get_height(longest_row) > 23);
    f.choose(longest);
    f.s.pending = true; f.publish(); // Queued Stop still permits only local inspection.
    f.choose("< Prev");
    f.type("anything"); f.tap(Key::Backspace); f.tap(Key::Delete);
    CHECK(f.intents.empty() && f.connections.empty());
    f.choose("[R] Stop"); f.tap(Key::Stop); f.tap(Key::Escape);
    CHECK(f.stops == 3);
    f.choose("Exit application"); CHECK(f.quits == 1);
    CHECK(f.intents.empty() && f.connections.empty());
}
void review_replay_and_busy() {
    Fixture f; f.tool(Tool::Hf);
    f.press(Key::Character, 'A'); // Held key while the first review arrives.
    f.s.view.review = true; f.s.view.review_token = 41;
    put(f.s.view.review_title, "Write HF unit");
    for (unsigned i = 0; i < 4; ++i) put(f.s.view.review_lines[i], "Review line " + std::to_string(i));
    f.publish();
    auto* cancel = f.button("Cancel (default)");
    auto* group = static_cast<lv_group_t*>(lv_obj_get_group(cancel));
    CHECK(lv_group_get_focused(group) == cancel);
    f.focus("Confirm once"); f.press(Key::Enter, 0, true); CHECK(f.intents.empty());
    f.press(Key::Enter); CHECK(f.intents.empty());
    f.release(Key::Character, 'A');
    f.release(Key::Enter); f.tap(Key::Enter); f.command(Command::Confirm);
    CHECK(f.intents.back().review_token == 41 && f.intents.size() == 1);
    f.publish();
    CHECK(lv_obj_has_state(f.button("Confirm once"), LV_STATE_DISABLED));
    f.press(Key::Enter, 0, true); CHECK(f.intents.size() == 1); // consumed token cannot replay
    f.s.view.review_token = 42; f.publish();
    CHECK(lv_group_get_focused(static_cast<lv_group_t*>(lv_obj_get_group(f.button("Cancel (default)")))) ==
          f.button("Cancel (default)"));
    // A newly displayed token resets Cancel and requires release + a new activation.
    f.press(Key::Enter, 0, true); CHECK(f.intents.size() == 1);
    f.focus("Confirm once"); f.release(Key::Enter); f.tap(Key::Enter);
    f.command(Command::Confirm); CHECK(f.intents.back().review_token == 42 && f.intents.size() == 2);
    f.s.view.review = false; f.s.pending = true; f.publish();
    CHECK(!find_type(f.root, &lv_textarea_class));
    CHECK(!find_label(f.root, "[U] Unit"));
    f.type("ABC"); CHECK(f.intents.size() == 2);
    f.tap(Key::Stop); CHECK(f.stops == 1);
    f.tap(Key::Escape); CHECK(f.stops == 2);
    f.choose("Exit application"); CHECK(f.quits == 1);
    f.s.pending = false; f.s.view.busy = true; f.publish();
    f.choose("[R] Stop"); CHECK(f.stops == 3);
    f.s.view.busy = false; f.s.view.status = WRITE_UNVERIFIED; f.publish();
    CHECK(find_label(f.root, "WRITE UNVERIFIED", true));
    f.s.cleanup_failed = true; f.publish(); CHECK(find_label(f.root, "CLEANUP UNVERIFIED", true));
    f.tap(Key::Quit); CHECK(f.quits == 2);
}
void discovered_defaults_and_rescan() {
    Fixture f;
    f.s.discovery.sequence = 1; f.s.discovery.attempted = f.s.discovery.completed = true;
    f.s.config.uhf_serial = "/dev/serial0"; f.s.config.hf_i2c = "/dev/i2c-1";
    f.s.config.grove_i2c_confirmed = true;
    f.s.discovery.devices[0] = {Detection::Detected,"/dev/serial0","M100",""};
    f.s.discovery.devices[1] = {Detection::Detected,"/dev/i2c-1","RFID2 / WS1850S",""};
    f.s.discovery.devices[2] = {Detection::Detected,"/dev/spidev0.1","CC1101 (identity only)",""};
    f.s.discovery.devices[3] = {Detection::Blocked,"/dev/spidev0.2","","Enable Cap power"};
    f.publish();
    f.choose("[H] Hardware");
    CHECK(find_label(f.root,"Right Grove UHF: Detected"));
    CHECK(find_label(f.root,"RFID2 / WS1850S"));
    CHECK(find_label(f.root,"CC1101 (identity only)"));
    CHECK(find_label(f.root,"Cap NFC: Power/routing needed"));
    CHECK(f.intents.empty() && f.connections.empty());
    f.tap(Key::Escape); f.tap(Key::Character, 'S'); f.choose("Connections");
    f.button("UHF serial path: /dev/serial0");
    f.choose("Auto-detect / rescan known hardware");
    CHECK(f.detections.size() == 1 && f.detections[0].hf_i2c == "/dev/i2c-1");
    CHECK(f.intents.empty() && f.connections.empty()); f.publish();
    f.choose("HF I2C path: /dev/i2c-1");
    f.s.discovery.sequence = 2; f.s.config.hf_i2c = "/dev/i2c-8"; f.publish();
    CHECK(find_type(f.root,&lv_textarea_class));
    f.replace("/dev/i2c-3"); f.publish();
    f.button("HF I2C path: /dev/i2c-3");
    CHECK(f.connections.empty() && f.intents.empty());
}
void idle_and_theme() {
    Fixture f;
    lv_refr_now(nullptr);
    auto* shell = lv_obj_get_child(f.root, 0);
    const unsigned before = flushes;
    for (unsigned i = 0; i < 100; ++i) { f.publish(); lv_refr_now(nullptr); }
    CHECK(lv_obj_get_child(f.root, 0) == shell);
    CHECK(flushes == before); // revision-only snapshots must not invalidate/rebuild
    f.tap(Key::Character, 'S'); f.choose("Theme: Dark > Light"); CHECK(find_label(f.root, "Theme: Light > Dark"));
    f.publish(); CHECK(find_label(f.root, "Theme: Light > Dark"));
    auto* label = find_label(f.root, "Connections"); CHECK(label);
    auto* group = static_cast<lv_group_t*>(lv_obj_get_group(lv_obj_get_parent(label)));
    f.tap(Key::Home); auto* first = lv_group_get_focused(group);
    f.tap(Key::Tab); CHECK(lv_group_get_focused(group) != first);
    f.tap(Key::Tab, 0, true); CHECK(lv_group_get_focused(group) == first);
    f.tap(Key::End); CHECK(lv_group_get_focused(group) != first);
}
void direct_shortcuts_and_focus() {
    Fixture f;
    f.tap(Key::Character, '1'); f.command(Command::OpenHf);
    CHECK(f.intents.size() == 1);
    f.tool(Tool::Hf);
    auto* read = f.button("[R] Read");
    CHECK(lv_group_get_focused(static_cast<lv_group_t*>(lv_obj_get_group(read))) == read);
    f.tap(Key::Enter); f.command(Command::HfRead); f.publish();
    put(f.s.view.identity, "04112233445566");
    put(f.s.view.hf_data, "0102030405060708090A0B0C0D0E0F10");
    f.publish();
    f.tap(Key::Character, '1');
    CHECK(f.intents.size() == 2 && find_label(f.root, "0102030405060708090A0B0C0D0E0F10"));
    f.tap(Key::Character, 'R'); f.command(Command::HfRead); f.publish();
    f.tap(Key::Character, 'E');
    f.tap(Key::Home);
    for (unsigned n = 0; n < 32; ++n) f.tap(Key::Delete);
    const size_t before_editor = f.intents.size();
    f.type("123ABC");
    CHECK(f.intents.size() == before_editor && find_type(f.root, &lv_textarea_class));
    f.tap(Key::Enter); f.set(Field::HfValue, "123ABC");
    f.tap(Key::Character, 'S'); f.type("RW");
    CHECK(f.intents.size() == before_editor + 1); // Settings cannot start RF or prepare writes.
    f.tap(Key::Character, '1'); CHECK(f.intents.size() == before_editor + 1);
    f.tap(Key::Character, '2'); f.command(Command::OpenUhf); f.tool(Tool::Uhf);
    auto* start = f.button("[R] Start");
    CHECK(lv_group_get_focused(static_cast<lv_group_t*>(lv_obj_get_group(start))) == start);
    f.tap(Key::Character, 'R'); f.command(Command::UhfStart); f.publish();
    f.s.view.inventory = true; f.s.view.busy = true;
    f.s.view.tag_count = 1; f.s.view.tags[0].size = 2;
    f.s.view.tags[0].epc[0] = 0xab; f.s.view.tags[0].epc[1] = 0xcd; f.publish();
    f.focus("1. ABCD\nRSSI 0 dBm | reads 0");
    f.s.view.tags[0].reads = 9; f.s.view.tags[0].rssi = -41; f.publish();
    auto* tag = f.button("1. ABCD\nRSSI -41 dBm | reads 9");
    CHECK(lv_group_get_focused(static_cast<lv_group_t*>(lv_obj_get_group(tag))) == tag);
    const size_t before_busy = f.intents.size();
    f.type("123SHW"); CHECK(f.intents.size() == before_busy && !find_label(f.root, "Settings"));
    f.tap(Key::Character, 'R'); CHECK(f.stops == 1);
    f.s.view.inventory = f.s.view.busy = false; f.publish();
    f.tap(Key::Character, '2'); CHECK(f.intents.size() == before_busy);
    f.tap(Key::Home); f.tap(Key::Tab); f.tap(Key::Enter);
    f.command(Command::UhfSelectTag); f.publish();
    CHECK(find_label(f.root, "UHF Memory") && find_label(f.root, "[R] Read memory"));
}
void unavailable_readers_and_protected_units() {
    Fixture f;
    f.s.simulated = false; f.publish();
    f.type("123"); CHECK(f.intents.empty());
    f.s.config.hf_i2c = "/dev/i2c-1"; f.s.config.uhf_serial = "/dev/serial0";
    f.s.discovery.completed = true;
    f.s.discovery.config = f.s.config;
    f.s.discovery.devices[1] = {Detection::Missing, "/dev/i2c-1", "", ""};
    f.s.discovery.devices[0] = {Detection::Blocked, "/dev/serial0", "", ""}; f.publish();
    f.type("123"); CHECK(f.intents.empty());
    CHECK(find_label(f.root, "[1] HF / NFC  Not found"));
    f.s.discovery.config = {}; f.publish();
    CHECK(find_label(f.root, "[1] HF / NFC  Configured"));
    f.tap(Key::Character, '1'); f.command(Command::OpenHf); f.tool(Tool::Hf);
    f.tap(Key::Character, '2'); f.command(Command::OpenUhf); f.tool(Tool::Uhf);
    f.intents.clear();
    f.s.connected = false; f.s.simulated = true; f.publish();
    f.type("123"); CHECK(f.intents.empty());
    f.s.connected = true; f.s.view.hf_writable = false; f.tool(Tool::Hf);
    CHECK(find_label(f.root, "Protected unit: choose [U] unit"));
    CHECK(lv_obj_has_state(f.button("[E] Bytes"), LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(f.button("[W] Review"), LV_STATE_DISABLED));
    f.type("EW"); CHECK(f.intents.empty());
}
void lf_capture_fixed_primary() {
    Fixture f; f.tool(Tool::Lf);
    CHECK(lv_obj_has_state(f.button("[C] Capture"), LV_STATE_DISABLED));
    f.tap(Key::Character, 'C'); CHECK(f.intents.empty());
    f.s.view.lf_present = true; f.publish(); lv_refr_now(nullptr);
    auto* scan = f.button("[R] Scan");
    auto* capture = f.button("[C] Capture");
    lv_area_t scan_box, capture_box;
    lv_obj_get_coords(scan, &scan_box); lv_obj_get_coords(capture, &capture_box);
    CHECK(scan_box.y1 == capture_box.y1 && scan_box.y2 == capture_box.y2);
    CHECK(capture_box.x1 > scan_box.x2 && capture_box.x2 < 320);
    CHECK(capture_box.y1 >= 0 && capture_box.y2 < 170);
    auto* group = static_cast<lv_group_t*>(lv_obj_get_group(scan));
    CHECK(lv_group_get_focused(group) == scan);
    f.tap(Key::Character, 'C'); f.command(Command::LfCapture); f.publish();
    f.tap(Key::Home); f.tap(Key::Tab); f.tap(Key::Enter);
    f.command(Command::LfCapture); CHECK(f.intents.size() == 2);
}
void bounded_review_and_warning() {
    Fixture f; f.tool(Tool::Hf);
    f.s.view.review = true; f.s.view.review_token = 111;
    put(f.s.view.review_title, "Review target and range");
    for (auto& line : f.s.view.review_lines) put(line, std::string(120, 'A'));
    f.publish(); lv_refr_now(nullptr);
    auto* cancel = f.button("Cancel (default)");
    auto* confirm = f.button("Confirm once");
    auto* group = static_cast<lv_group_t*>(lv_obj_get_group(cancel));
    CHECK(lv_group_get_focused(group) == cancel);
    const auto within = [](lv_obj_t* obj) {
        lv_area_t box; lv_obj_get_coords(obj, &box);
        return box.x1 >= 0 && box.y1 >= 0 && box.x2 < 320 && box.y2 < 170;
    };
    CHECK(within(cancel) && within(confirm));
    f.focus("Confirm once");
    f.type("WR123SH"); CHECK(f.intents.empty());
    f.tap(Key::PageDown); lv_refr_now(nullptr);
    CHECK(within(cancel) && within(confirm));
    CHECK(lv_group_get_focused(group) == confirm);
    f.tap(Key::Enter); f.command(Command::Confirm); CHECK(f.intents.size() == 1);
    f.publish(); f.type("WR"); f.tap(Key::Enter);
    CHECK(std::count_if(f.intents.begin(), f.intents.end(), [](const local::Intent& i) {
        return i.command == Command::Confirm;
    }) == 1);
    f.s.view.review = false; f.s.view.status = WRITE_UNVERIFIED; f.publish();
    CHECK(find_label(f.root, "WRITE UNVERIFIED", true));
    f.s.view.status = OK; f.publish(); f.tap(Key::Character, 'S');
    CHECK(find_label(f.root, "WRITE UNVERIFIED", true));
    f.choose("Clear RAM session / credentials");
    f.s.view.status = CANCELLED; f.publish();
    CHECK(find_label(f.root, "WRITE UNVERIFIED", true));
    f.s.view.status = OK; f.publish(); // An unrelated successful action cannot acknowledge a canceled Clear.
    CHECK(find_label(f.root, "WRITE UNVERIFIED", true));
    f.choose("Clear RAM session / credentials"); f.publish();
    CHECK(!find_label(f.root, "WRITE UNVERIFIED", true));
    f.s.cleanup_failed = true; f.publish();
    CHECK(find_label(f.root, "CLEANUP UNVERIFIED", true));
}
}
int main() {
    lv_init();
    auto* display = lv_display_create(320, 170); CHECK(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    navigation_and_connections(); secrets_and_text(); ndef_pagination(); bulk_ui(); uhf_and_lf();
    live_inventory_updates(); review_replay_and_busy(); idle_and_theme(); discovered_defaults_and_rescan();
    direct_shortcuts_and_focus(); bounded_review_and_warning(); unavailable_readers_and_protected_units();
    lf_capture_fixed_primary();
    CHECK(flushes > 0);
    lv_display_delete(display); lv_deinit();
    std::puts("PASS: 13 device-free LVGL UI suites (config, masking, NDEF/bulk, UHF/LF, live tags, review/busy, idle/theme, discovery, shortcuts/focus, bounded review/warnings)");
}
