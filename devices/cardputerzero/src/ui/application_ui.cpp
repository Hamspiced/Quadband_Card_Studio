#include "../ui.hpp"
#include "../platform/discovery.hpp"
#include "lvgl.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace qb::zero {
namespace {
using local::Command;
using local::Field;
using local::Intent;
using local::Tool;
template<size_t N> std::string text(const std::array<char, N>& a) {
    return std::string(a.data(), std::find(a.begin(), a.end(), '\0'));
}
std::string number(unsigned n) { return std::to_string(n); }
std::string epc(const local::TagView& tag) {
    const char* hex = "0123456789ABCDEF";
    std::string s;
    for (unsigned i = 0; i < std::min(62U, unsigned(tag.size)); ++i) {
        s += hex[tag.epc[i] >> 4]; s += hex[tag.epc[i] & 15];
    }
    return s;
}
const char* const lf_names[] = {
    "EM410x", "Electra", "HID Prox", "Viking", "ioProx",
    "PAC / Stanley", "Jablotron", "Pyramid", "Auto scan (read only)"
};
enum class Page {
    Home, Connections, Memory, Auth, NtagAuth, Ndef, Kinds, Fields, Records,
    Copy, Tags, UhfMemory, UhfData, Power, Lf, Formats, LfId, Hardware,
    Settings, About, HfEdit, UhfBank, UhfRange, UhfPassword, UhfEpc, Bulk, Drafts
};
void wipe(std::string& s) { local::secure_zero(s.data(), s.size()); s.clear(); }
}

struct ApplicationUi::State {
    struct Row {
        std::string label;
        std::function<void()> activate;
        bool enabled = true;
        bool wrap = false;
        std::string id;
        unsigned dock = 0;
        bool operator==(const Row& r) const {
            return label == r.label && enabled == r.enabled && wrap == r.wrap &&
                id == r.id && dock == r.dock;
        }
    };
    lv_obj_t* root;
    lv_obj_t* shell = nullptr;
    lv_obj_t* editor = nullptr;
    lv_obj_t* details = nullptr;
    lv_group_t* group = nullptr;
    Callbacks callbacks;
    Snapshot snapshot;
    ReaderConfig config;
    Page page = Page::Home;
    unsigned list_page = 0, form_page = 0;
    std::vector<Row> rows, painted_rows;
    std::string title, footer, painted_title, painted_footer, error;
    std::set<unsigned> held;
    std::array<char, 257> edit_value{};
    std::function<void(const std::string&)> accept_edit;
    size_t edit_limit = 256, cursor = 0;
    bool editing = false, secret = false, numeric = false, hex_only = false;
    bool initialized = false, light = false, theme_override = false;
    bool painted_light = false, submitted = false, review_released = false;
    bool was_review = false;
    uint64_t painted_token = 0, consumed_token = 0;
    uint64_t submitted_revision = 0;
    uint64_t discovery_sequence = 0;
    bool submitted_pending = false;
    std::string edit_title;
    Page painted_page = Page::Home;
    bool painted_review = false, painted_busy = false;
    std::vector<std::pair<lv_obj_t*, std::string>> focus_ids;
    bool sticky_write = false, clear_warning_pending = false;
    unsigned painted_list_page = 0, painted_form_page = 0;

    State(lv_obj_t* parent, Callbacks cb) : root(parent), callbacks(std::move(cb)) {
        group = lv_group_create();
        lv_group_set_wrap(group, true);
        lv_group_set_focus_cb(group, [](lv_group_t* g) {
            if (auto* obj = lv_group_get_focused(g)) lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
        });
    }
    ~State() {
        clear_editor();
        if (shell) lv_obj_delete(shell);
        lv_group_delete(group);
    }
    bool busy() const { return submitted || snapshot.pending || snapshot.view.busy || snapshot.view.inventory; }
    bool usable() const { return snapshot.connected && snapshot.view.reader_present && !snapshot.cleanup_failed; }
    bool selected() const {
        const auto& v = snapshot.view;
        return v.selected_tag < std::min(64U, v.tag_count) &&
            v.tags[v.selected_tag].size >= 2 && v.tags[v.selected_tag].size <= 28 &&
            v.tags[v.selected_tag].size % 2 == 0;
    }
    void clear_editor() {
        local::secure_zero(edit_value.data(), edit_value.size());
        // Clear the widget's real backing text before destroying it, including secret input.
        if (editor) lv_textarea_set_text(editor, "");
        editor = nullptr; editing = false; secret = false; cursor = 0; accept_edit = {};
    }
    void emit(Command command, unsigned index = 0) {
        if (busy() || (snapshot.cleanup_failed && command != Command::Exit &&
                       command != Command::ClearSession && command != Command::CancelReview)) return;
        Intent intent; intent.command = command; intent.index = index;
        if (command == Command::Confirm || command == Command::CancelReview)
            intent.review_token = snapshot.view.review_token;
        if (command == Command::Confirm) {
            if (!snapshot.view.review || !review_released || !painted_token ||
                painted_token != snapshot.view.review_token || painted_token <= consumed_token) return;
            consumed_token = painted_token;
        }
        if (callbacks.submit && callbacks.submit(intent)) {
            if (command == Command::ClearSession) clear_warning_pending = true;
            submitted = true; submitted_revision = snapshot.view.revision;
            submitted_pending = snapshot.pending;
            clear_editor();
        }
        render();
    }
    void set(Field field, const std::string& value) {
        if (busy() || snapshot.cleanup_failed) return;
        Intent intent; intent.command = Command::SetField; intent.field = field;
        std::copy_n(value.data(), std::min(size_t(256), value.size()), intent.text.data());
        if (callbacks.submit && callbacks.submit(intent)) {
            submitted = true; submitted_revision = snapshot.view.revision; submitted_pending = snapshot.pending;
        }
    }
    void go(Page destination) {
        if (busy() || snapshot.view.review) return;
        page = destination; list_page = 0; form_page = 0; error.clear(); painted_rows.clear(); render();
    }
    void row(std::string label, std::function<void()> action, bool enabled = true, bool wrap = false,
             std::string id = "", unsigned dock = 0) {
        if (id.empty()) id = label;
        rows.push_back({std::move(label), std::move(action), enabled, wrap, std::move(id), dock});
    }
    void info(std::string label) { row(std::move(label), {}, false); }
    void nav(const char* label, Page destination) {
        row(label, [this, destination] { go(destination); }, true, false,
            "page:" + number(unsigned(destination)), 2);
    }
    void action(const char* label, Command command, bool enabled = true, unsigned index = 0) {
        const bool primary = command == Command::HfRead || command == Command::UhfStart ||
            command == Command::UhfRead || command == Command::UhfReadPower || command == Command::LfScan;
        const bool review = command == Command::Confirm || command == Command::CancelReview;
        std::string id = "command:" + number(unsigned(command));
        if (command == Command::HfSelectUnit)
            id += std::string(label).find("Previous") != std::string::npos ? ":previous" : ":next";
        row(label, [this, command, index] { emit(command, index); }, enabled, false,
            std::move(id), review ? 2 : primary ? 1 : 0);
    }
    void choice(std::string label, Field field, unsigned value, bool enabled = true) {
        row(std::move(label), [this, field, value] { set(field, number(value)); render(); }, enabled,
            false, "choice:" + number(unsigned(field)) + ":" + number(value));
    }
    void paginate(unsigned count, unsigned per_page) {
        const unsigned pages = std::max(1U, (count + per_page - 1) / per_page);
        list_page = std::min(list_page, pages - 1);
        info("Page " + number(list_page + 1) + "/" + number(pages));
        row("< Prev", [this] { --list_page; painted_rows.clear(); render(); }, list_page > 0, false, "previous", 2);
        row("Next >", [this] { ++list_page; painted_rows.clear(); render(); }, list_page + 1 < pages, false, "next", 2);
    }
    void edit(std::string label, std::string initial, size_t limit,
              bool masked, bool digits, bool hex,
              std::function<void(const std::string&)> accept, bool enabled = true) {
        // Never retain a supplied secret in a row callback or label.
        if (masked) wipe(initial);
        row(label, [this, label, initial, limit, masked, digits, hex, accept] {
            clear_editor(); edit_title = label; edit_limit = std::min(size_t(256), limit);
            secret = masked; numeric = digits; hex_only = hex; editing = true;
            std::copy_n(initial.data(), std::min(initial.size(), edit_limit), edit_value.data());
            cursor = std::strlen(edit_value.data()); accept_edit = accept;
            painted_rows.clear(); render();
        }, enabled, false, "editor:" + label.substr(0, label.find(":")));
    }
    void field(std::string label, Field f, std::string initial = "", size_t limit = 256,
               bool masked = false, bool digits = false, bool hex = false, bool enabled = true) {
        edit(label, std::move(initial), limit, masked, digits, hex,
             [this, f](const std::string& s) { set(f, s); }, enabled);
        rows.back().id = "field:" + number(unsigned(f));
    }
    void home() {
        title = "QUADBAND / Card Studio";
        const char* names[]{"[1] HF / NFC", "[2] UHF", "[3] LF / Chameleon"};
        for (unsigned i = 0; i < 3; ++i) {
            row(std::string(names[i]) + "  " + reader_status(i),
                [this, i] { reader(i); }, reader_available(i), false, "reader:" + number(i));
        }
        nav("[S] Settings", Page::Settings);
        nav("[H] Hardware", Page::Hardware);
    }
    void reader(unsigned index) {
        const Tool tools[]{Tool::Hf, Tool::Uhf, Tool::Lf};
        const Page pages[]{Page::Memory, Page::Tags, Page::Lf};
        const Command commands[]{Command::OpenHf, Command::OpenUhf, Command::OpenLf};
        if (snapshot.view.tool == tools[index]) { go(pages[index]); return; }
        if (reader_available(index)) emit(commands[index]);
    }
    std::string reader_status(unsigned index) const {
        if (snapshot.cleanup_failed) return "Cleanup failed";
        if (!snapshot.connected) return "Not connected";
        if (snapshot.simulated) return "SIM";
        const std::string endpoints[]{snapshot.config.hf_i2c, snapshot.config.uhf_serial, snapshot.config.ultra_serial};
        if (endpoints[index].empty()) return "Not configured";
        if (index < 2 && snapshot.discovery.completed) {
            const auto& device = snapshot.discovery.devices[index == 0 ? 1 : 0];
            const auto& discovered = index == 0 ? snapshot.discovery.config.hf_i2c : snapshot.discovery.config.uhf_serial;
            if (discovered == endpoints[index] && device.endpoint == endpoints[index] && device.state != Detection::Unchecked)
                return platform::detection_name(device.state);
        }
        return "Configured";
    }
    bool reader_available(unsigned index) const {
        if (!snapshot.connected || snapshot.cleanup_failed) return false;
        if (snapshot.simulated) return true;
        const std::string endpoints[]{snapshot.config.hf_i2c, snapshot.config.uhf_serial, snapshot.config.ultra_serial};
        if (endpoints[index].empty()) return false;
        if (index < 2 && snapshot.discovery.completed) {
            const auto& device = snapshot.discovery.devices[index == 0 ? 1 : 0];
            const auto& discovered = index == 0 ? snapshot.discovery.config.hf_i2c : snapshot.discovery.config.uhf_serial;
            if (discovered == endpoints[index] && device.endpoint == endpoints[index] && device.state != Detection::Unchecked &&
                device.state != Detection::Detected && device.state != Detection::Manual) return false;
        }
        return true;
    }
    bool parse_number(const std::string& s, unsigned& result, unsigned max) {
        if (s.empty() || s[0] == '-' || s[0] == '+') return false;
        const int base = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 16 : 10;
        char* end = nullptr; errno = 0; unsigned long n = std::strtoul(s.c_str(), &end, base);
        if (errno || !end || *end || n > max) return false;
        result = unsigned(n); return true;
    }
    void connections() {
        title = "Connections (explicit endpoints)";
        row("Auto-detect / rescan known hardware", [this] {
            if (callbacks.detect && callbacks.detect(config)) {
                submitted = true; submitted_revision = snapshot.view.revision; submitted_pending = snapshot.pending;
                clear_editor();
            }
            render();
        });
        info("Right Grove = UHF UART. Hat Grove = HF I2C.");
        info("No pinmux changes or automatic card scans.");
        info("Ultra uses USB host. No broad bus probe.");
        edit("HF I2C path: " + config.hf_i2c, config.hf_i2c, 127, false, false, false,
             [this](const std::string& s) { config.hf_i2c = s; });
        edit("HF address (0x28): " + number(config.hf_address), number(config.hf_address),
             4, false, false, false, [this](const std::string& s) {
                 unsigned n = 0;
                 if (parse_number(s, n, 0x77) && n >= 0x08) config.hf_address = n;
                 else error = "Use explicit 7-bit address 0x08-0x77.";
             });
        row(config.grove_i2c_confirmed ? "HF I2C connector confirmed: YES" : "HF I2C connector confirmed: NO",
            [this] { config.grove_i2c_confirmed = !config.grove_i2c_confirmed; render(); }, true, false, "connector-confirm");
        edit("UHF serial path: " + config.uhf_serial, config.uhf_serial, 127, false, false, false,
             [this](const std::string& s) { config.uhf_serial = s; });
        edit("UHF baud: " + number(config.uhf_baud), number(config.uhf_baud), 7, false, true, false,
             [this](const std::string& s) {
                 unsigned n = 0;
                 if (parse_number(s, n, 4000000) && n > 0) config.uhf_baud = n;
                 else error = "Baud must be 1-4000000.";
             });
        edit("Ultra USB serial: " + config.ultra_serial, config.ultra_serial, 127, false, false, false,
             [this](const std::string& s) { config.ultra_serial = s; });
        row("Connect configured readers", [this] {
            if ((!config.hf_i2c.empty() && !config.grove_i2c_confirmed) ||
                (config.hf_i2c.empty() && config.uhf_serial.empty() && config.ultra_serial.empty() && !snapshot.simulated)) {
                error = "Choose endpoints and confirm Grove I2C."; render(); return;
            }
            if (callbacks.connect && callbacks.connect(config)) {
                submitted = true; submitted_revision = snapshot.view.revision; submitted_pending = snapshot.pending;
            }
            render();
        });
        nav("Home", Page::Home);
    }
    void hf() {
        const auto& v = snapshot.view;
        if (page == Page::Memory || page == Page::HfEdit) {
            title = page == Page::Memory ? "HF Memory" : "HF unit / edit";
            action("[R] Read", Command::HfRead, usable());
            if (page == Page::Memory) {
                info("UID " + text(v.identity) + " | unit " + number(v.hf_unit) + "/" + number(v.hf_units));
                info(v.hf_unit_known ? text(v.hf_data) : "No known bytes; Read explicitly.");
                if (v.hf_unit_known && !v.hf_writable) info("Protected unit: choose [U] unit");
            }
            if (page == Page::HfEdit) {
                action("< Previous unit", Command::HfSelectUnit, v.hf_ready && v.hf_unit > 0,
                       v.hf_unit ? v.hf_unit - 1 : 0);
                action("Next unit >", Command::HfSelectUnit, v.hf_ready && v.hf_unit + 1 < v.hf_units, v.hf_unit + 1);
                field("Go to unit", Field::HfUnit, number(v.hf_unit), 3, false, true, false, v.hf_ready);
                field("Edit unit bytes", Field::HfValue, text(v.hf_value[0] ? v.hf_value : v.hf_data),
                      std::min(32U, v.hf_unit_size * 2), false, false, true, v.hf_unit_known && v.hf_writable);
                action("Review unit write", Command::HfPrepareUnit,
                       usable() && v.hf_unit_known && v.hf_writable && v.hf_value[0]);
            } else {
                nav("[U] Unit", Page::HfEdit);
                nav("[A] Auth", Page::Auth);
                nav("[T] NTAG", Page::NtagAuth);
                nav("[N] NDEF", Page::Ndef);
                nav("[C] Copy", Page::Copy);
                field("[E] Bytes", Field::HfValue, text(v.hf_value[0] ? v.hf_value : v.hf_data),
                      std::min(32U, v.hf_unit_size * 2), false, false, true, v.hf_unit_known && v.hf_writable);
                rows.back().dock = 1;
                action("[W] Review", Command::HfPrepareUnit,
                       usable() && v.hf_unit_known && v.hf_writable && v.hf_value[0]);
                rows.back().dock = 1;
            }
        } else if (page == Page::Auth) {
            title = "HF Classic auth (RAM only)";
            choice(v.hf_key_type == 0x60 ? "Key A [selected]" : "Key A", Field::HfKeyType, 0x60);
            choice(v.hf_key_type == 0x61 ? "Key B [selected]" : "Key B", Field::HfKeyType, 0x61);
            field("Supplied key: ************", Field::HfKey, "", 12, true, false, true);
            row("Use factory FFFFFFFFFFFF key", [this] { set(Field::HfKey, "FFFFFFFFFFFF"); render(); });
            choice(v.hf_standard_keys ? "Standard NFC keys: ON" : "Standard NFC keys: OFF",
                   Field::HfStandardKeys, !v.hf_standard_keys);
            info("Changing auth invalidates the old read/review.");
        } else if (page == Page::NtagAuth) {
            title = "NTAG auth (RAM only)";
            choice("Mode: none" + std::string(v.ntag_mode == 0 ? " [selected]" : ""), Field::NtagMode, 0);
            choice("Mode: password" + std::string(v.ntag_mode == 1 ? " [selected]" : ""), Field::NtagMode, 1);
            choice("Mode: password + PACK" + std::string(v.ntag_mode == 2 ? " [selected]" : ""), Field::NtagMode, 2);
            field("Password: ********", Field::NtagPassword, "", 8, true, false, true);
            field("PACK: ****", Field::NtagPack, "", 4, true, false, true);
            info("Known locks / protection restrictions remain.");
        } else if (page == Page::Copy) {
            title = "HF RAM copy";
            info(v.hf_source ? "Source captured in RAM" : "No source captured");
            info("Read source > Capture > remove > Read no card");
            info("> present same-family target > Read > Review.");
            action("[R] Read / removal", Command::HfRead, usable());
            action("Capture source", Command::HfCapture, v.hf_ready);
            action("Review RAM copy", Command::HfPrepareCopy,
                   usable() && v.hf_ready && v.hf_source && v.hf_copy_armed);
            info(v.hf_copy_armed ? "Removal gate ready" : "Observe removal before copy/repeat.");
        } else ndef();
        if (page != Page::Memory) nav("[M] Memory", Page::Memory);
    }
    void ndef() {
        const auto& v = snapshot.view;
        if (page == Page::Ndef) {
            title = "NFC / NDEF";
            info(number(v.ndef_count) + " read | " + number(v.ndef_queue) + " queued (max 8)");
            info(number(v.ndef_size) + "/" + number(v.ndef_capacity) + " bytes");
            nav("[K] Kinds", Page::Kinds); nav("[F] Fields", Page::Fields);
            nav("[V] View", Page::Records);
            nav("[B] Bulk 1-3 records", Page::Bulk);
            nav("Draft order / remove", Page::Drafts);
            action("Add record to RAM", Command::HfAddRecord, v.ndef_queue < 8);
            action("Clear RAM queue", Command::HfClearRecords);
            action("Review NFC append", Command::HfPrepareNdef, usable() && v.ndef_available && v.ndef_queue > 0);
            action("[R] Read", Command::HfRead, usable());
        } else if (page == Page::Bulk) {
            title = "Bulk NFC job | " + number(v.bulk_bytes) + " bytes";
            const char* names[]{"Unknown","Classic1K","Classic4K","NTAG213","NTAG215","NTAG216"};
            unsigned family = unsigned(v.bulk_family);
            choice(std::string("Exact target: ")+names[std::min(5u,family)],Field::BulkFamily,
                   family == 1 ? 3 : family >= 5 ? 1 : family+1,!v.bulk_active);
            choice(v.bulk_overwrite ? "Overwrite: ON" : "Overwrite: OFF",Field::BulkOverwrite,!v.bulk_overwrite,!v.bulk_active);
            choice(v.bulk_formatting ? "Canonical format: ON" : "Canonical format: OFF",Field::BulkFormatting,!v.bulk_formatting,!v.bulk_active);
            info(number(v.ndef_queue)+" frozen draft records (1-3 only)");
            info("Written "+number(v.bulk_written)+" | same "+number(v.bulk_identical)+
                 " | rejected "+number(v.bulk_rejected)+" | failed "+number(v.bulk_failed));
            info(v.bulk_phase == 1 ? "Remove all cards; Read must observe No card." :
                 v.bulk_phase == 4 ? "UNVERIFIED: Clear session required." : "Read target; fresh Cancel-default review.");
            action("Arm frozen job",Command::HfArmBulk,!v.bulk_active && v.ndef_queue > 0 && v.ndef_queue <= 3);
            action("[R] Read / removal",Command::HfRead,usable() && v.bulk_phase != 4);
            action("Review bulk target",Command::HfPrepareBulk,usable() && v.hf_ready && v.bulk_phase == 2);
            row("Stop job", [this] { if (callbacks.stop) callbacks.stop(); },v.bulk_active);
            action("Clear session / unverified warning",Command::ClearSession);
            nav("NDEF",Page::Ndef);
        } else if (page == Page::Drafts) {
            title = "RAM draft order / removal";
            list_page = std::min(list_page,v.ndef_queue ? v.ndef_queue-1 : 0);
            info("Draft "+number(v.ndef_queue ? list_page+1 : 0)+"/"+number(v.ndef_queue));
            action("Move draft up",Command::HfMoveRecordUp,!v.bulk_active && list_page > 0,list_page);
            action("Remove draft",Command::HfRemoveRecord,!v.bulk_active && list_page < v.ndef_queue,list_page);
            paginate(v.ndef_queue,1); nav("NDEF",Page::Ndef);
        } else if (page == Page::Kinds) {
            title = "NDEF kinds (19)";
            list_page = std::min(list_page, 4U);
            for (unsigned i = list_page * 4; i < std::min(19U, list_page * 4 + 4); ++i) {
                row(number(i + 1) + ". " + text(v.ndef_kinds[i]), [this, i] {
                    set(Field::NdefKind, number(i)); page = Page::Fields; list_page = 0; painted_rows.clear(); render();
                }, true, false, "ndef-kind:" + number(i));
            }
            paginate(19, 4);
        } else if (page == Page::Fields) {
            title = "NDEF fields | " + text(v.ndef_kinds[std::min(18U, v.ndef_kind)]);
            const unsigned count = std::min(8U, unsigned(v.ndef_field_count));
            list_page = std::min(list_page, count ? (count - 1) / 2 : 0);
            for (unsigned i = list_page * 2; i < std::min(count, list_page * 2 + 2); ++i) {
                const auto& f = v.ndef_fields[i];
                const auto key = Field(unsigned(Field::NdefField0) + i);
                const std::string label = text(f.label) + ": " + (f.secret ? "********" : text(f.value));
                if (f.choice_count) {
                    row(label, [this, i, key] {
                        const auto& f = snapshot.view.ndef_fields[i];
                        const unsigned count = std::min(4U, unsigned(f.choice_count));
                        unsigned next = 0;
                        for (unsigned j = 0; j < count; ++j)
                            if (text(f.value) == text(f.choices[j])) next = (j + 1) % count;
                        if (count) set(key, text(f.choices[next]));
                        render();
                    }, true, false, "field:" + number(unsigned(key)));
                } else field(label, key, f.secret ? "" : text(f.value), 256, f.secret);
            }
            if (!count) info("This kind has no editable fields.");
            paginate(count, 2);
            action("Add record to RAM", Command::HfAddRecord, v.ndef_queue < 8);
            nav("[K] Kinds", Page::Kinds); nav("NDEF", Page::Ndef);
        } else if (page == Page::Records) {
            title = "Read records / + RAM drafts";
            unsigned count = 0;
            for (const auto& r : v.ndef_records) if (r[0]) ++count;
            list_page = std::min(list_page, count ? (count - 1) / 3 : 0);
            for (unsigned i = list_page * 3; i < std::min(count, list_page * 3 + 3); ++i)
                info(text(v.ndef_records[i]));
            info(number(v.ndef_queue) + " queued | '+' = draft, not written");
            paginate(count, 3);
            nav("NDEF", Page::Ndef);
        }
    }
    void uhf() {
        const auto& v = snapshot.view;
        if (page == Page::Tags) {
            title = "UHF Inventory | " + number(std::min(64U, v.tag_count)) + "/64";
            action("[R] Start", Command::UhfStart, usable());
            const unsigned count = std::min(64U, v.tag_count);
            list_page = std::min(list_page, count ? (count - 1) / 3 : 0);
            for (unsigned i = list_page * 3; i < std::min(count, list_page * 3 + 3); ++i) {
                row(number(i + 1) + ". " + epc(v.tags[i]) + "  " +
                    std::to_string(int(v.tags[i].rssi)) + "dBm x" + number(v.tags[i].reads),
                    [this, i] { emit(Command::UhfSelectTag, i); page = Page::UhfMemory; },
                    true, true, "tag:" + epc(v.tags[i]));
            }
            if (!count) info("No tags; Start explicitly to inventory.");
            paginate(count, 3);
            nav("[M] Memory", Page::UhfMemory); nav("[P] Power", Page::Power);
        } else if (page == Page::UhfMemory || page == Page::UhfBank || page == Page::UhfRange ||
                   page == Page::UhfPassword || page == Page::UhfEpc) {
            title = "UHF Memory";
            action("[R] Read memory", Command::UhfRead, usable() && selected());
            info(v.selected_tag < std::min(64U, v.tag_count) ? epc(v.tags[v.selected_tag]) : "Select a tag first.");
            static const char* banks[] = {"Reserved", "EPC", "TID", "User"};
            if (page == Page::UhfBank) {
                for (unsigned i = 0; i < 4; ++i)
                    choice(std::string(banks[i]) + (v.uhf_bank == i ? " [selected]" : ""), Field::UhfBank, i);
            } else if (page == Page::UhfRange) {
                field("Word 0-65535: " + number(v.uhf_word), Field::UhfWord, number(v.uhf_word), 5, false, true);
                field("Count 1-64: " + number(v.uhf_count), Field::UhfCount, number(v.uhf_count), 2, false, true);
            } else if (page == Page::UhfPassword) {
                field("Access password: ********", Field::UhfPassword, "", 8, true, false, true);
            } else if (page == Page::UhfEpc) {
                field("New EPC (2-28 bytes)", Field::UhfEpc, text(v.uhf_epc), 56, false, false, true, selected());
                action("Review EPC rename", Command::UhfPrepareEpc, usable() && selected() && v.uhf_epc[0]);
            } else {
                info(std::string(banks[std::min(3U, v.uhf_bank)]) + " | word " + number(v.uhf_word) +
                     " + " + number(v.uhf_count));
                info(v.uhf_memory ? text(v.uhf_data).substr(0, 64) : "No range read. Select bank / range.");
                nav("[B] Bank", Page::UhfBank); nav("[G] Range", Page::UhfRange);
                nav("[D] Data", Page::UhfData); nav("[E] EPC", Page::UhfEpc);
                nav("[A] Pass", Page::UhfPassword);
            }
            if (page != Page::UhfMemory) nav("[M] Memory", Page::UhfMemory);
        } else if (page == Page::UhfData) {
            title = "UHF data | bank " + number(v.uhf_bank) + " word " + number(v.uhf_word);
            const auto data = text(v.uhf_data);
            const unsigned chunks = std::max(1U, unsigned((data.size() + 63) / 64));
            list_page = std::min(list_page, chunks - 1);
            info(v.uhf_memory ? data.substr(list_page * 64, 64) : "Read the exact range first.");
            paginate(chunks, 1);
            const bool writable = usable() && selected() && v.uhf_memory && v.uhf_bank == 3 &&
                v.uhf_count >= 1 && v.uhf_count <= 32;
            field("Edit User bytes (1-32 words)", Field::UhfValue,
                  v.uhf_value[0] ? text(v.uhf_value) : text(v.uhf_data), 128, false, false, true, writable);
            action("Review User write", Command::UhfPrepareUser, writable && v.uhf_value[0]);
            nav("[M] Memory", Page::UhfMemory);
        } else if (page == Page::Power) {
            title = "UHF RF power";
            info("Target " + number(v.uhf_power) + " dBm | " + (v.uhf_power_known ? "read known" : "not read"));
            info("18-26 dBm only. Region unchanged.");
            action("[R] Read power", Command::UhfReadPower, usable());
            field("Set target dBm", Field::UhfPower, number(v.uhf_power), 2, false, true);
            action("Review power change", Command::UhfPreparePower, usable() && v.uhf_power_known);
        }
        if (page != Page::Tags && page != Page::UhfMemory) nav("[2] Tags", Page::Tags);
    }
    void lf() {
        const auto& v = snapshot.view;
        const unsigned format = std::min(8U, v.lf_format);
        const bool scan_ok = usable() && (format == 8 ? v.lf_scan_mask != 0 : (v.lf_scan_mask & (1U << format)));
        const bool write_ok = usable() && format < 8 && format != 2 && format != 7 &&
            (v.lf_write_mask & (1U << format));
        if (page == Page::Lf) {
            title = "LF Scan";
            action("[R] Scan", Command::LfScan, scan_ok);
            info(lf_names[format]);
            info(v.lf_present ? "Scanned: " + text(v.lf_actual) : "No scanned identity");
            action("[C] Capture", Command::LfCapture, v.lf_present);
            rows.back().dock = 1;
            nav("[F] Formats", Page::Formats); nav("[I] ID/write", Page::LfId);
            if (format == 2 || format == 8)
                field("HID hint (0-30 or 42): " + number(v.lf_hint), Field::LfHint, number(v.lf_hint), 2, false, true);
        } else if (page == Page::Formats) {
            title = "LF formats (capability-driven)";
            list_page = std::min(list_page, 2U);
            for (unsigned i = list_page * 4; i < std::min(9U, list_page * 4 + 4); ++i) {
                const bool readable = i == 8 ? v.lf_scan_mask != 0 : (v.lf_scan_mask & (1U << i));
                const bool writable = i < 8 && i != 2 && i != 7 && (v.lf_write_mask & (1U << i));
                choice(std::string(lf_names[i]) + (i == format ? " *" : "") +
                       (writable ? " [R/W]" : " [read only]"), Field::LfFormat, i, readable);
            }
            paginate(9, 4);
            nav("[3] Scan", Page::Lf);
        } else if (page == Page::LfId) {
            title = std::string("LF ID / ") + lf_names[format];
            info("Desired: " + text(v.lf_value));
            const unsigned limits[] = {10, 26, 26, 8, 32, 8, 10, 32, 32};
            field("Edit custom ID", Field::LfId, text(v.lf_value), limits[format],
                  false, false, format != 5, format < 8);
            action("Default test ID 0102030405", Command::LfTestId);
            info("Capture/set > remove all > Scan no card.");
            action("[R] Scan / removal", Command::LfScan, scan_ok);
            info("ONE unprotected T5577; password-free only.");
            info(v.lf_write_armed ? "Removal gate ready" : "Removal gate NOT ready");
            action("Review T5577 write", Command::LfPrepareWrite, write_ok && v.lf_write_armed && v.lf_card_valid);
            nav("[3] Scan", Page::Lf);
        }
    }
    void live_inventory() {
        const auto& v = snapshot.view;
        const unsigned count = std::min(64U, v.tag_count);
        title = "UHF live | " + number(count) + "/64 tags";
        row("[R] Stop", [this] { if (callbacks.stop) callbacks.stop(); }, true, false, "stop", 1);
        list_page = std::min(list_page, count ? (count - 1) / 3 : 0);
        for (unsigned i = list_page * 3; i < std::min(count, list_page * 3 + 3); ++i) {
            // Focus permits native scrolling of the full EPC. Activation is local and read-only.
            row(number(i + 1) + ". " + epc(v.tags[i]) + "\nRSSI " +
                std::to_string(int(v.tags[i].rssi)) + " dBm | reads " + number(v.tags[i].reads),
                [] {}, true, true, "tag:" + epc(v.tags[i]));
        }
        if (!count) info("Waiting for tags; inventory is active.");
        paginate(count, 3);
        row("Exit application", [this] { if (callbacks.quit) callbacks.quit(); });
    }
    void build() {
        rows.clear(); title.clear();
        const auto& v = snapshot.view;
        footer = error.empty() ? (v.message[0] ? text(v.message) : snapshot.detail) : error;
        if (snapshot.cleanup_failed) footer = "CLEANUP UNVERIFIED - reconnect/restart. " + snapshot.detail;
        else if (sticky_write) footer = "WRITE UNVERIFIED - no auto retry. " + footer;
        if (busy()) {
            if (v.inventory) { live_inventory(); return; }
            title = "Working / pending";
            info(text(v.message));
            info(number(v.done) + "/" + number(v.total) + " | " + number(std::min(64U, v.tag_count)) + " tags");
            row("[R] Stop", [this] { if (callbacks.stop) callbacks.stop(); }, true, false, "stop", 1);
            row("Exit application", [this] { if (callbacks.quit) callbacks.quit(); });
            return;
        }
        if (v.review) {
            title = text(v.review_title);
            action("Cancel (default)", Command::CancelReview);
            for (const auto& line : v.review_lines) info(text(line));
            info("Release key, inspect target, then fresh Enter.");
            action("Confirm once", Command::Confirm, v.review_token && !snapshot.cleanup_failed &&
                   v.review_token > consumed_token);
            if (!snapshot.cleanup_failed && !sticky_write)
                footer = "Review #" + std::to_string(v.review_token) + " | no automatic write";
            return;
        }
        if (page == Page::Home) home();
        else if (page == Page::Settings) {
            title = "Settings";
            nav("Connections", Page::Connections); nav("Hardware", Page::Hardware); nav("About", Page::About);
            row(light ? "Theme: Light > Dark" : "Theme: Dark > Light", [this] {
                light = !light; theme_override = true; render();
            }, true, false, "theme");
            action("Clear RAM session / credentials", Command::ClearSession);
            row("Quit", [this] { if (callbacks.quit) callbacks.quit(); });
        } else if (page == Page::About) {
            title = "About / Card Studio";
            info("Original project: Tototo31 / Ashsh");
            info("CardputerZero | keyboard reader workspaces");
            info("Cap radio / built-in NFC: identity only");
            nav("[S] Settings", Page::Settings);
        }
        else if (page == Page::Connections) connections();
        else if (page == Page::Hardware) {
            title = "Detected modules (not cards)";
            const char* names[]{"Right Grove UHF","External RFID / RFID2","Cap CC1101","Cap NFC"};
            for (unsigned i = 0; i < 4; ++i) {
                const auto& item = snapshot.discovery.devices[i];
                row(std::string(names[i]) + ": " + platform::detection_name(item.state), [] {}, true, false, "hardware:" + number(i));
                if (!item.endpoint.empty()) info(item.endpoint);
                if (!item.identity.empty()) info(item.identity);
                if (!item.detail.empty()) info(item.detail);
            }
            info("Cap chips: identity only. HF tools use RFID/RFID2.");
            nav("Home",Page::Home);
        }
        else if (v.tool == Tool::Hf) hf();
        else if (v.tool == Tool::Uhf) uhf();
        else if (v.tool == Tool::Lf) lf();
        else { page = Page::Home; home(); }
    }
    lv_color_t color(uint32_t dark, uint32_t day) { return lv_color_hex(light ? day : dark); }
    lv_obj_t* label(lv_obj_t* parent, const std::string& s, int x, int y, int w,
                    const lv_font_t* font = &lv_font_montserrat_12) {
        auto* obj = lv_label_create(parent); lv_label_set_text(obj, s.c_str());
        lv_obj_set_pos(obj, x, y); lv_obj_set_width(obj, w);
        lv_obj_set_style_text_font(obj, font, 0);
        lv_obj_set_style_text_color(obj, color(0xe6edf6, 0x15243b), 0);
        return obj;
    }
    void render() {
        if (editing) { render_editor(); return; }
        build();
        const bool form = !busy() && !snapshot.view.review && (page == Page::Connections || page == Page::HfEdit ||
            page == Page::Auth || page == Page::NtagAuth || page == Page::Copy || page == Page::LfId ||
            page == Page::Ndef || page == Page::Bulk);
        if (form) {
            unsigned count = 0;
            for (const auto& r : rows) if (!r.dock) ++count;
            const unsigned pages = std::max(1U, (count + 2) / 3);
            form_page = std::min(form_page, pages - 1);
            if (pages > 1) {
                unsigned index = 0;
                rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) {
                    if (r.dock) return false;
                    return index++ / 3 != form_page;
                }), rows.end());
                row("< Form", [this] { --form_page; render(); }, form_page > 0, false, "form-prev", 2);
                row("Form >", [this] { ++form_page; render(); }, form_page + 1 < pages, false, "form-next", 2);
                title += " " + number(form_page + 1) + "/" + number(pages);
            }
        }
        if (shell && painted_title == title && painted_footer == footer && painted_light == light && painted_rows == rows) {
            for (size_t i = 0; i < rows.size(); ++i) painted_rows[i].activate = rows[i].activate;
            return;
        }
        const int scroll = details ? lv_obj_get_scroll_y(details) : 0;
        std::string focused;
        if (shell) for (const auto& item : focus_ids)
            if (item.first == lv_group_get_focused(group)) focused = item.second;
        const bool same_page = painted_page == page && painted_review == snapshot.view.review &&
            painted_busy == busy() && (!snapshot.view.review || painted_token == snapshot.view.review_token);
        const bool same_scroll = same_page && painted_list_page == list_page && painted_form_page == form_page;
        if (shell) lv_obj_delete(shell);
        shell = nullptr; editor = nullptr; focus_ids.clear();
        painted_rows = rows; painted_title = title; painted_footer = footer; painted_light = light;
        painted_page = page; painted_review = snapshot.view.review; painted_busy = busy();
        painted_list_page = list_page; painted_form_page = form_page;
        make_shell();
        auto* heading = label(shell, title, 7, 3, 306, &lv_font_montserrat_14);
        lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_CLIP); lv_obj_set_height(heading, 18);
        const unsigned bars = unsigned(std::count_if(rows.begin(), rows.end(), [](const Row& r) { return r.dock == 2; }));
        const unsigned primary = unsigned(std::count_if(rows.begin(), rows.end(), [](const Row& r) { return r.dock == 1; }));
        const int body_top = primary ? 51 : 25;
        const int body_bottom = bars ? 108 : 134;
        auto* list = lv_obj_create(shell); details = list;
        lv_obj_set_pos(list, 4, body_top); lv_obj_set_size(list, 312, body_bottom - body_top);
        lv_obj_set_style_pad_all(list, 0, 0); lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_radius(list, 3, 0);
        lv_obj_set_style_bg_color(list, color(0x14243a, 0xffffff), 0);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        int y = 0; unsigned bar = 0, top = 0;
        // Primary controls enter the focus group first, independent of body row order.
        for (unsigned pass : {1U, 0U, 2U}) for (auto& r : painted_rows) {
            if (r.dock != pass) continue;
            if (!r.activate) {
                auto* obj = label(list, r.label, 5, y + 2, 296);
                lv_obj_update_layout(obj);
                y += std::max(18, int(lv_obj_get_height(obj)) + 4);
                continue;
            }
            const int width = r.dock ? (308 / int(std::max(1U, r.dock == 2 ? bars : primary))) - 3 : 304;
            auto* obj = lv_button_create(r.dock ? shell : list);
            lv_obj_set_pos(obj, r.dock == 2 ? 6 + int(bar++) * (width + 3) : r.dock ? 6 + int(top++) * (width + 3) : 2,
                r.dock == 2 ? 111 : r.dock ? 25 : y);
            lv_obj_set_size(obj, width, 23);
            lv_obj_set_style_radius(obj, 3, 0); lv_obj_set_style_pad_all(obj, 3, 0);
            lv_obj_set_style_bg_color(obj, color(r.dock == 1 ? 0x145c60 : 0x20354b, r.dock == 1 ? 0xb8e8de : 0xdce8f2), 0);
            lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUSED);
            lv_obj_set_style_outline_color(obj, color(0x5ce0c1, 0x087c75), LV_STATE_FOCUSED);
            auto* caption = label(obj, r.label, 1, 0, width - 8,
                r.dock ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
            int height = 23;
            if (r.wrap && !r.dock) {
                lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_WRAP);
                lv_obj_update_layout(caption);
                height = std::max(height, int(lv_obj_get_height(caption)) + 8);
                lv_obj_set_height(obj, height);
            } else {
                lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_CLIP);
                lv_obj_set_height(caption, 16);
            }
            if (r.enabled) {
                lv_group_add_obj(group, obj); focus_ids.push_back({obj, r.id});
                lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
                lv_obj_add_event_cb(obj, [](lv_event_t* event) {
                    auto invoke = static_cast<Row*>(lv_event_get_user_data(event))->activate;
                    if (invoke) invoke();
                }, LV_EVENT_CLICKED, &r);
            } else {
                lv_obj_add_state(obj, LV_STATE_DISABLED); lv_obj_set_style_opa(obj, LV_OPA_50, 0);
            }
            if (!r.dock) y += height + 3;
        }
        auto* status = label(shell, footer, 7, 137, 306, &lv_font_montserrat_10);
        lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_CLIP); lv_obj_set_height(status, 20);
        if (snapshot.cleanup_failed || sticky_write) lv_obj_set_style_text_color(status, color(0xff8ba0, 0xb92742), 0);
        label(shell, snapshot.view.review ? "PgUp/Dn details | Enter decision | Esc cancel" :
            busy() ? "R/Esc stop | Tab inspect | Ctrl+C stop" :
            "1 HF  2 UHF  3 LF | S settings | Esc back", 7, 158, 306, &lv_font_montserrat_10);
        if (!focus_ids.empty()) {
            auto* target = focus_ids.front().first;
            if (same_page) for (const auto& item : focus_ids) if (item.second == focused) target = item.first;
            lv_group_focus_obj(target);
        }
        if (same_scroll) lv_obj_scroll_to_y(list, scroll, LV_ANIM_OFF);
        painted_token = snapshot.view.review ? snapshot.view.review_token : 0;
        lv_obj_invalidate(shell);
    }
    void make_shell() {
        shell = lv_obj_create(root);
        lv_obj_remove_style_all(shell);
        lv_obj_set_pos(shell, 0, 0); lv_obj_set_size(shell, 320, 170);
        lv_obj_remove_flag(shell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(shell, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(shell, color(0x091321, 0xf1f5fa), 0);
    }
    void render_editor() {
        if (editor) return;
        if (shell) lv_obj_delete(shell);
        shell = nullptr; details = nullptr; painted_rows.clear(); painted_title.clear();
        focus_ids.clear();
        make_shell();
        auto* heading = label(shell, edit_title, 7, 5, 306, &lv_font_montserrat_14);
        lv_label_set_long_mode(heading, LV_LABEL_LONG_MODE_CLIP); lv_obj_set_height(heading, 20);
        label(shell, secret ? "RAM only | masked input" : "Physical ASCII keyboard", 7, 33, 306);
        editor = lv_textarea_create(shell);
        lv_obj_set_pos(editor, 7, 57); lv_obj_set_size(editor, 306, 49);
        lv_textarea_set_one_line(editor, true);
        lv_textarea_set_max_length(editor, uint32_t(edit_limit));
        lv_textarea_set_password_bullet(editor, "*");
        lv_textarea_set_password_show_time(editor, 0);
        lv_textarea_set_password_mode(editor, secret);
        lv_obj_set_style_text_font(editor, &lv_font_montserrat_14, 0);
        lv_textarea_set_text(editor, edit_value.data());
        lv_textarea_set_cursor_pos(editor, int32_t(cursor));
        lv_group_add_obj(group, editor); lv_group_focus_obj(editor);
        label(shell, "Enter save | Esc cancel | Home/End | Backspace", 7, 118, 306, &lv_font_montserrat_10);
        label(shell, "No RF operation is run by editing.", 7, 144, 306);
        lv_obj_invalidate(shell);
    }
    void update(const Snapshot& s) {
        const auto old_tool = snapshot.view.tool;
        const bool inventory_started = s.view.inventory && !snapshot.view.inventory;
        const bool new_review = s.view.review && (!was_review || painted_token != s.view.review_token);
        if (submitted && (s.view.revision != submitted_revision || s.pending != submitted_pending))
            submitted = false;
        if (!initialized) { config = s.config; light = s.view.light_theme; initialized = true; }
        else if (!theme_override) light = s.view.light_theme;
        if (s.discovery.sequence != discovery_sequence) {
            discovery_sequence = s.discovery.sequence;
            if (!editing && s.discovery.completed) config = s.config;
        }
        snapshot = s;
        sticky_write = sticky_write || s.view.status == WRITE_UNVERIFIED;
        if (clear_warning_pending && !busy()) {
            if (s.view.status == OK) sticky_write = false;
            clear_warning_pending = false;
        }
        if (inventory_started) { page = Page::Tags; list_page = 0; form_page = 0; }
        if (new_review) {
            clear_editor(); review_released = false; painted_rows.clear(); painted_title.clear();
        }
        if (busy() || snapshot.cleanup_failed || old_tool != s.view.tool) clear_editor();
        if (old_tool != s.view.tool) {
            page = s.view.tool == Tool::Hf ? Page::Memory : s.view.tool == Tool::Uhf ? Page::Tags :
                   s.view.tool == Tool::Lf ? Page::Lf : Page::Home;
            list_page = 0; form_page = 0; painted_rows.clear();
        }
        was_review = s.view.review;
        render();
    }
    void edit_key(const KeyEvent& e) {
        if (e.key == Key::Escape) { clear_editor(); render(); return; }
        if (e.key == Key::Enter) {
            std::string value(edit_value.data());
            auto accept = accept_edit; clear_editor();
            if (accept) accept(value);
            wipe(value); render(); return;
        }
        size_t length = std::strlen(edit_value.data());
        if (e.key == Key::Left && cursor) --cursor;
        else if (e.key == Key::Right && cursor < length) ++cursor;
        else if (e.key == Key::Home) cursor = 0;
        else if (e.key == Key::End) cursor = length;
        else if (e.key == Key::Backspace && cursor) {
            std::memmove(edit_value.data() + cursor - 1, edit_value.data() + cursor, length - cursor + 1); --cursor;
        } else if (e.key == Key::Delete && cursor < length) {
            std::memmove(edit_value.data() + cursor, edit_value.data() + cursor + 1, length - cursor);
        } else if (e.key == Key::Character && length < edit_limit) {
            const unsigned char c = static_cast<unsigned char>(e.character);
            const bool digit = c >= '0' && c <= '9';
            const bool hex = digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (c < 32 || c > 126 || (numeric && !digit) || (hex_only && !hex)) return;
            std::memmove(edit_value.data() + cursor + 1, edit_value.data() + cursor, length - cursor + 1);
            edit_value[cursor++] = char(c);
        } else return;
        // set_text avoids keeping deleted secret bytes in a separate std::string.
        lv_textarea_set_text(editor, edit_value.data());
        lv_textarea_set_cursor_pos(editor, int32_t(cursor));
    }
    void key(const KeyEvent& e) {
        const unsigned key_id = unsigned(e.key) * 256U +
            (e.key == Key::Character ? static_cast<unsigned char>(e.character) : 0U);
        if (!e.pressed) {
            if (e.key == Key::Character && !e.character) {
                for (auto it = held.begin(); it != held.end();) {
                    if (*it / 256U == unsigned(Key::Character)) it = held.erase(it);
                    else ++it;
                }
            } else held.erase(key_id);
            if (snapshot.view.review && held.empty()) review_released = true;
            return;
        }
        if (e.repeat || !held.insert(key_id).second) return;
        if (e.key == Key::Quit) { if (callbacks.quit) callbacks.quit(); return; }
        if (e.key == Key::Stop) { if (callbacks.stop) callbacks.stop(); return; }
        if (busy()) {
            if (e.key == Key::Escape) { if (callbacks.stop) callbacks.stop(); return; }
            if (e.key == Key::Character && (e.character == 'r' || e.character == 'R')) {
                if (callbacks.stop) callbacks.stop();
                return;
            }
            if (e.key == Key::Enter) activate();
            else navigate(e);
            return;
        }
        if (editing) { edit_key(e); return; }
        if (snapshot.view.review) {
            if (e.key == Key::Escape) emit(Command::CancelReview);
            else if (e.key == Key::Enter) activate();
            else navigate(e);
            return;
        }
        if (e.key == Key::Character) {
            const char c = char(std::toupper(static_cast<unsigned char>(e.character)));
            if (c >= '1' && c <= '3') { reader(unsigned(c - '1')); return; }
            if (c == 'S') { go(Page::Settings); return; }
            if (c == 'H') { go(Page::Hardware); return; }
            if (c == 'R') {
                // Only the visible primary action can run; settings/home never start RF.
                for (const auto& r : rows) if (r.dock == 1 && r.enabled && r.activate &&
                    r.label.find("[R]") == 0) { auto invoke = r.activate; invoke(); return; }
                return;
            }
            const std::string hint = std::string("[") + c + "]";
            for (const auto& r : rows) {
                if (r.enabled && r.activate && r.label.find(hint) == 0) {
                    auto invoke = r.activate; invoke(); return;
                }
            }
            if (c == 'W') {
                for (const auto& r : rows) if (r.enabled && r.activate && r.label.find("Review ") == 0) {
                    auto invoke = r.activate; invoke(); return;
                }
            }
            return;
        }
        if (e.key == Key::Escape) {
            if (snapshot.view.review) emit(Command::CancelReview);
            else if (page == Page::Home) { if (callbacks.quit) callbacks.quit(); }
            else if (page == Page::Connections || page == Page::Hardware || page == Page::Settings || page == Page::About) go(Page::Home);
            else if (page == Page::Memory || page == Page::Tags || page == Page::Lf) emit(Command::Exit);
            else go(snapshot.view.tool == Tool::Hf ? Page::Memory : snapshot.view.tool == Tool::Uhf ? Page::Tags : Page::Lf);
            return;
        }
        if (e.key == Key::Enter) activate();
        else navigate(e);
    }
    void activate() {
        auto* obj = lv_group_get_focused(group);
        if (obj) lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    }
    void navigate(const KeyEvent& e) {
        if (details && (e.key == Key::PageDown || e.key == Key::PageUp)) {
            lv_obj_scroll_by(details, 0, e.key == Key::PageDown ? -55 : 55, LV_ANIM_OFF); return;
        }
        if (e.key == Key::Up || e.key == Key::Left || (e.key == Key::Tab && e.shift))
            lv_group_focus_prev(group);
        else if (e.key == Key::Down || e.key == Key::Right || e.key == Key::Tab)
            lv_group_focus_next(group);
        else if (e.key == Key::Home && lv_group_get_obj_count(group))
            lv_group_focus_obj(lv_group_get_obj_by_index(group, 0));
        else if (e.key == Key::End && lv_group_get_obj_count(group))
            lv_group_focus_obj(lv_group_get_obj_by_index(group, lv_group_get_obj_count(group) - 1));
        else if (e.key == Key::PageDown || e.key == Key::PageUp)
            for (unsigned i = 0; i < 3; ++i)
                e.key == Key::PageDown ? lv_group_focus_next(group) : lv_group_focus_prev(group);
    }
};

ApplicationUi::ApplicationUi(_lv_obj_t* root, Callbacks callbacks)
    : state_(std::make_unique<State>(root, std::move(callbacks))) {}
ApplicationUi::~ApplicationUi() = default;
void ApplicationUi::update(const Snapshot& snapshot) { state_->update(snapshot); }
void ApplicationUi::key(const KeyEvent& event) { state_->key(event); }
}
