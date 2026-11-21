#include "controller.hpp"
#include "logo.hpp"
#include "typography.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace qb::ui {
namespace {
constexpr Rect screen{0, 0, 320, 240};
bool intersects(Rect a, Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}
Rect united(Rect a, Rect b) {
    const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x,
            std::max(a.y + a.h, b.y + b.h) - y};
}
const char* mode_title(WorkMode m) {
    switch (m) {
    case WorkMode::Connected: return "Connected";
    case WorkMode::Standalone: return "Standalone";
    default: return "Home";
    }
}
const char* usb_title(UsbHostState s) {
    switch (s) {
    case UsbHostState::Requested: return "Requested";
    case UsbHostState::Enabled: return "Enabled";
    case UsbHostState::ExternalPowerBlocked: return "Blocked: external VBUS";
    case UsbHostState::Failed: return "Failed";
    default: return "Off";
    }
}
template<size_t N> void bounded_copy(char (&target)[N], const std::string& source) {
    const size_t size = std::min(N - 1, source.size());
    std::memcpy(target, source.data(), size);
    target[size] = '\0';
    if (source.size() >= N) std::memcpy(target + N - 4, "...", 3);
}
uint32_t blended(uint16_t rgba, uint32_t bg) {
    const unsigned alpha = rgba & 15;
    uint32_t out = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        const unsigned component = (rgba >> (shift / 2 + 4)) & 15;
        out |= ((component * 17 * alpha + ((bg >> shift) & 255) * (15 - alpha)) / 15) << shift;
    }
    return out;
}
}

void Controller::initialize() {
    page_ = Page::Dashboard;
    settings_page_ = 0;
    previous_count_ = 0;
    initialized_ = true;
    render();
}

void Controller::update(const UiSnapshot& snapshot) {
    const bool changed_mode = state_.mode != snapshot.mode;
    // Fixed buffers keep rendering allocation-free even if an upstream peer misbehaves.
    state_.mode = snapshot.mode;
    state_.transition_target = snapshot.transition_target;
    state_.transition_pending = snapshot.transition_pending;
    state_.confirm_mode = snapshot.confirm_mode;
    state_.confirm_target = snapshot.confirm_target;
    state_.confirm_usb = snapshot.confirm_usb;
    state_.confirm_bonds = snapshot.confirm_bonds;
    state_.release_unverified = snapshot.release_unverified;
    state_.paired = snapshot.paired;
    state_.pairing_pending = snapshot.pairing_pending;
    state_.pairing_open = snapshot.pairing_open;
    state_.busy = snapshot.busy;
    state_.pairing_code = snapshot.pairing_code;
    state_.request_id = snapshot.request_id;
    state_.last_status = snapshot.last_status;
    state_.battery_percent = snapshot.battery_percent;
    state_.internal_bytes = snapshot.internal_bytes;
    state_.external_bytes = snapshot.external_bytes;
    state_.usb_host = snapshot.usb_host;
    state_.light_theme = snapshot.light_theme;
    bounded_copy(state_.message, snapshot.message);
    for (size_t i = 0; i < 3; ++i) {
        auto& target = state_.peripherals[i];
        const auto& source = snapshot.peripherals[i];
        target.present = source.present;
        target.generation = source.generation;
        bounded_copy(target.kind, source.kind);
        target.capability_count = std::min(size_t(3), source.capabilities.size());
        for (size_t n = 0; n < target.capability_count; ++n)
            bounded_copy(target.capabilities[n], source.capabilities[n]);
    }
    local_ = snapshot.local;
    local_.light_theme = snapshot.light_theme; local_.battery = snapshot.battery_percent;
    if (changed_mode) { tools_.close(); tools_obscured_ = true; page_ = Page::Dashboard; settings_page_ = 0; }
    if (!initialized_) initialize();
    else render();
}

Modal Controller::modal() const {
    if (state_.pairing_pending) return Modal::Pairing;
    if (state_.release_unverified) return Modal::None;
    // Once a handoff is running no stale confirmation can authorize another action.
    if (state_.transition_pending) return Modal::None;
    if (state_.confirm_mode) return Modal::Mode;
    if (state_.confirm_usb) return Modal::Usb;
    if (state_.confirm_bonds) return Modal::Bonds;
    return Modal::None;
}

void Controller::add(Ink ink, Rect rect, uint32_t color, const char* value, unsigned scale) {
    if (count_ == max_commands) return;
    auto& cmd = commands_[count_++];
    cmd = {};
    cmd.ink = ink; cmd.rect = rect; cmd.color = color; cmd.scale = scale;
    std::snprintf(cmd.label, sizeof(cmd.label), "%s", value);
}
void Controller::fill(Rect rect, uint32_t color) { add(Ink::Fill, rect, color); }
void Controller::panel(Rect rect, uint32_t color, uint32_t background, unsigned radius) {
    add(Ink::Panel, rect, color, "", radius);
    commands_[count_ - 1].background = background;
}
void Controller::label(int x, int y, int width, const char* value, unsigned scale, bool muted, uint32_t background) {
    char bounded[53]{};
    size_t len = 0;
    int used = 0;
    while (value[len] && len < 52) {
        const auto c = static_cast<unsigned char>(value[len]);
        const int advance = glyph(c, scale).advance;
        if (used + advance > width) break;
        bounded[len] = c >= 32 && c <= 126 ? char(c) : '?';
        used += advance; ++len;
    }
    if (value[len]) {
        const int dots = text_width("...", scale);
        while (len && (used + dots > width || len > 49)) used -= glyph(bounded[--len], scale).advance;
        if (used + dots <= width) std::memcpy(bounded + len, "...", 4);
        else bounded[0] = '\0';
    }
    add(Ink::Text, {x, y, text_width(bounded, scale), text_height(scale)}, muted ? muted_ : text_, bounded, scale);
    commands_[count_ - 1].background = background == UINT32_MAX ? label_background_ : background;
}
void Controller::button(Rect rect, const char* title, UiAction action, bool enabled) {
    const bool primary = action == UiAction::ChooseConnected || action == UiAction::ApprovePairing ||
        action == UiAction::ConfirmMode || action == UiAction::ConfirmUsbHost;
    const bool danger = action == UiAction::Stop || action == UiAction::ConfirmClearBonds;
    const uint32_t bg = enabled && primary ? accent_ : enabled && danger ?
        (state_.light_theme ? 0xffe4e9 : 0x321822) : surface_color_;
    const uint32_t behind = modal() == Modal::None ? canvas_ : surface_color_;
    panel(rect, enabled && primary ? accent_ : border_, behind);
    panel({rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2}, bg, enabled && primary ? accent_ : border_, 7);
    const uint32_t old = text_;
    text_ = !enabled ? muted_ : primary ? (state_.light_theme ? 0xffffff : 0x002c29) : danger ? danger_ : text_;
    const int title_width = text_width(title, 2);
    label(rect.x + std::max(6, (rect.w - title_width) / 2),
          rect.y + (rect.h - text_height(2)) / 2, rect.w - 12, title, 2, false, bg);
    text_ = old;
    if (enabled && hit_count_ < max_hits)
        hits_[hit_count_++] = {rect, action, Page::Dashboard, false, false};
}
void Controller::navigation(Rect rect, const char* title, Page page) {
    button(rect, title, UiAction::None);
    auto& hit = hits_[hit_count_ - 1];
    hit.page = page; hit.navigate = true;
}
void Controller::header(const char* title) {
    fill({0, 0, 320, 36}, canvas_);
    fill({8, 10, 3, 18}, accent_);
    label(18, 10, 198, title);
    fill({8, 35, 304, 1}, border_);
    char battery[16];
    if (state_.battery_percent < 0) std::snprintf(battery, sizeof(battery), "BAT --");
    else std::snprintf(battery, sizeof(battery), "BAT %d%%", std::min(100, state_.battery_percent));
    label(228, 12, 84, battery, 1, true);
}
void Controller::dashboard() {
    header("QUADBAND STUDIO");
    if (state_.mode == WorkMode::Home) {
        add(Ink::Logo, {10, 42, logo_width, logo_height}, canvas_);
        label(134, 42, 180, "QUADBAND", 2);
        label(134, 66, 180, "CARD STUDIO", 2);
        label(134, 96, 180, state_.release_unverified ? "Release unverified" :
            state_.last_status == WRITE_UNVERIFIED ? "Write unverified" : "CoreS3 workbench");
        label(134, 113, 180, state_.release_unverified ? "Restart CoreS3" :
            state_.last_status == WRITE_UNVERIFIED ? "Read target again" : "Select mode", 1, true);
        unsigned readers = 0;
        for (const auto& slot : state_.peripherals) readers += slot.present;
        char summary[32];
        std::snprintf(summary, sizeof(summary), "%u / 3 readers online", readers);
        label(134, 133, 180, summary);
        std::snprintf(summary, sizeof(summary), "HF %s  UHF %s  USB %s",
            state_.peripherals[0].present ? "+" : "-",
            state_.peripherals[1].present ? "+" : "-",
            state_.peripherals[2].present ? "+" : "-");
        label(134, 148, 180, summary, 1, true);
        button({8, 166, 148, 40}, "Connected", UiAction::ChooseConnected, !state_.transition_pending && !state_.release_unverified);
        button({164, 166, 148, 40}, "Standalone", UiAction::ChooseStandalone, !state_.transition_pending && !state_.release_unverified);
        navigation({8, 208, 148, 32}, "Settings", Page::Settings);
        navigation({164, 208, 148, 32}, "Diagnostics", Page::Diagnostics);
    } else if (state_.mode == WorkMode::Connected) connected();
    else standalone();
}
void Controller::connected() {
    label(8, 44, 304, "CONNECTED", 2);
    label(8, 67, 304, state_.paired ? "Session: paired / authorized" : "Session: not paired");
    label(8, 82, 304, state_.pairing_open ? "Pairing window: open" : "Pairing window: closed", 1, true);
    label(8, 101, 304, state_.busy ? "Activity: remote request running" : "Activity: idle");
    const char* names[] = {"HF", "UHF", "USB / LF"};
    for (unsigned i = 0; i < 3; ++i) {
        const int x = 8 + int(i) * 104;
        panel({x, 122, 96, 42}, surface_color_, canvas_);
        fill({x + 10, 130, 3, 24}, state_.peripherals[i].present ? accent_ : border_);
        label(x + 20, 125, 72, names[i], 1, false, surface_color_);
        label(x + 20, 144, 72, state_.peripherals[i].present ? "Detected" : "Absent", 1, true, surface_color_);
    }
    label(8, 171, 304, !state_.message[0] ? "Desktop controls card operations." : state_.message, 1, true);
    button({8, 190, 96, 44}, "Stop", UiAction::Stop, !state_.transition_pending);
    button({112, 190, 96, 44}, "Modes", UiAction::ChooseHome, !state_.transition_pending);
    navigation({216, 190, 96, 44}, "Setup", Page::Settings);
}
void Controller::standalone() {
    label(8, 42, 304, "STANDALONE", 2);
    const char* names[] = {"UHF", "HF / NFC", "LF / USB", "Files"};
    const Page pages[] = {Page::Uhf, Page::Hf, Page::Lf, Page::Files};
    const unsigned slots[] = {1, 0, 2};
    const uint32_t colors[] = {accent_, state_.light_theme ? 0x8547d6u : 0xc69affu,
        state_.light_theme ? 0x2d74d9u : 0x61afffu, state_.light_theme ? 0xb46e12u : 0xf3c26fu};
    for (unsigned i = 0; i < 4; ++i) {
        const int x = 8 + (i % 2) * 156, y = 66 + (i / 2) * 60;
        panel({x, y, 148, 54}, border_, canvas_);
        panel({x + 1, y + 1, 146, 52}, surface_color_, border_, 7);
        fill({x + 10, y + 12, 3, 30}, colors[i]);
        label(x + 20, y + 4, 122, names[i], 2, false, surface_color_);
        const char* status = i == 3 ? "Not available" :
            state_.peripherals[slots[i]].present ? "Attached" : "Not detected";
        label(x + 20, y + 32, 122, status, 1, true, surface_color_);
        hits_[hit_count_++] = {{x, y, 148, 54}, UiAction::None, pages[i], true, false};
        // Reader tiles open the local worker-backed tools; file storage is separate.
    }
    label(8, 188, 304, state_.last_status == WRITE_UNVERIFIED ?
        "Write unverified. Read target again." : "Local reader tools / RAM-only session", 1, true);
    button({8, 207, 148, 33}, "Modes", UiAction::ChooseHome, !state_.transition_pending);
    navigation({164, 207, 148, 33}, "Settings", Page::Settings);
}
void Controller::detail() {
    const unsigned slot = page_ == Page::Uhf ? 1 : (page_ == Page::Hf ? 0 : 2);
    const char* title = page_ == Page::Uhf ? "UHF module" :
        page_ == Page::Hf ? "HF / NFC module" : page_ == Page::Lf ? "LF / USB module" : "Files";
    header(title);
    if (page_ == Page::Files) {
        label(8, 49, 304, "File services unavailable", 2);
        label(8, 83, 304, "No local browse / save / load.");
        label(8, 103, 304, "Storage is not initialized by this UI.", 1, true);
    } else {
        const auto& module = state_.peripherals[slot];
        label(8, 48, 304, module.present ? "Module detected" : "Module absent", 2);
        label(8, 77, 304, module.kind);
        if (!module.present)
            label(8, 98, 304, slot == 2 ? "Attach supported USB Chameleon." :
                slot == 1 ? "Attach UHF reader on its configured port." : "Attach supported Grove HF reader.");
        else {
            label(8, 98, 304, "Reported capabilities (read-only):", 1, true);
            for (size_t i = 0; i < module.capability_count; ++i)
                label(8, 114 + int(i) * 14, 304, module.capabilities[i]);
        }
    }
    panel({8, 162, 304, 36}, surface_color_, canvas_);
    label(16, 166, 288, page_ == Page::Files ? "File storage not implemented" : "Local reader workspace", 1, false, surface_color_);
    label(16, 182, 288, page_ == Page::Files ? "No SD/file I/O in this build." :
        "Choose Standalone for local tools.", 1, true, surface_color_);
    navigation({8, 207, 148, 33}, "Back", Page::Dashboard);
    navigation({164, 207, 148, 33}, "Diagnostics", Page::Diagnostics);
}
void Controller::settings() {
    header(settings_page_ ? "Settings 2 / 2" : "Settings 1 / 2");
    const bool safe = !state_.transition_pending;
    if (!settings_page_) {
        button({8, 44, 304, 40}, state_.light_theme ? "Theme: Light" : "Theme: Dark", UiAction::ToggleTheme, safe);
        button({8, 91, 304, 40}, state_.pairing_open ? "Pairing window open" : "Open pairing window", UiAction::OpenPairing, safe);
        navigation({8, 138, 304, 40}, "Diagnostics", Page::Diagnostics);
        label(8, 187, 304, "Preferences are applied by the runtime.", 1, true);
    } else {
        char usb[53]; std::snprintf(usb, sizeof(usb), "USB: %s", usb_title(state_.usb_host));
        label(8, 43, 304, usb);
        const bool can_enable = safe && state_.usb_host != UsbHostState::Enabled &&
            state_.usb_host != UsbHostState::Requested;
        button({8, 61, 304, 40}, state_.usb_host == UsbHostState::Enabled ? "USB host enabled" :
            state_.usb_host == UsbHostState::Requested ? "USB host requested" : "Enable USB host...", UiAction::RequestUsbHost, can_enable);
        label(8, 108, 304, "Disconnect PC / external VBUS first.", 1, true);
        button({8, 130, 304, 40}, "Reset BLE bonds...", UiAction::RequestClearBonds, safe);
        label(8, 179, 304, "Reset requires physical confirmation.", 1, true);
    }
    navigation({8, 207, 148, 33}, "Back", Page::Dashboard);
    button({164, 207, 148, 33}, settings_page_ ? "Previous" : "Next", UiAction::None);
    hits_[hit_count_ - 1].settings_next = true;
}
void Controller::diagnostics() {
    header("Diagnostics");
    char line[53];
    std::snprintf(line, sizeof(line), "Owner: %s", mode_title(state_.mode)); label(8, 47, 304, line);
    std::snprintf(line, sizeof(line), "Request: %lu  status: %u", static_cast<unsigned long>(state_.request_id), state_.last_status);
    label(8, 65, 304, line);
    std::snprintf(line, sizeof(line), "RAM internal: %lu B", static_cast<unsigned long>(state_.internal_bytes)); label(8, 83, 304, line);
    std::snprintf(line, sizeof(line), "RAM external: %lu B", static_cast<unsigned long>(state_.external_bytes)); label(8, 101, 304, line);
    std::snprintf(line, sizeof(line), "USB host: %s", usb_title(state_.usb_host)); label(8, 119, 304, line);
    for (unsigned i = 0; i < 3; ++i) {
        std::snprintf(line, sizeof(line), "%u: %.20s %s gen %lu", i + 1, state_.peripherals[i].kind,
            state_.peripherals[i].present ? "yes" : "no", static_cast<unsigned long>(state_.peripherals[i].generation));
        label(8, 139 + int(i) * 15, 304, line);
    }
    label(8, 187, 304, !state_.message[0] ? "No runtime message." : state_.message, 1, true);
    navigation({8, 207, 148, 33}, "Back", Page::Dashboard);
    button({164, 207, 148, 33}, "Refresh", UiAction::Refresh, !state_.transition_pending);
}
void Controller::overlay() {
    const auto current = modal();
    if (current == Modal::None) return;
    // Modal composition discards every underlying hit region.
    hit_count_ = 0;
    fill(screen, canvas_);
    panel({8, 8, 304, 224}, border_, canvas_, 12);
    panel({9, 9, 302, 222}, surface_color_, border_, 11);
    label_background_ = surface_color_;
    UiAction approve = UiAction::None, cancel = UiAction::None;
    if (current == Modal::Pairing) {
        label(20, 26, 280, "Approve pairing?", 2);
        char code[32]; std::snprintf(code, sizeof(code), "Code: %06lu", static_cast<unsigned long>(state_.pairing_code % 1000000));
        label(20, 65, 280, code, 2);
        label(20, 102, 280, "Compare with your computer.");
        label(20, 123, 280, "Approve only if both codes match.", 1, true);
        approve = UiAction::ApprovePairing; cancel = UiAction::RejectPairing;
    } else if (current == Modal::Mode) {
        label(20, 26, 280, "Change owner?", 2);
        label(20, 64, 280, mode_title(state_.confirm_target), 2);
        label(20, 102, 280, "Current work may be stopped.");
        label(20, 123, 280, "Runtime waits for a safe boundary.", 1, true);
        approve = UiAction::ConfirmMode; cancel = UiAction::CancelMode;
    } else if (current == Modal::Usb) {
        label(20, 26, 280, "Enable USB host?", 2);
        label(20, 66, 280, "Disconnect PC / external VBUS first.");
        label(20, 89, 280, "Battery or approved DinBase power.");
        label(20, 114, 280, "USB output supplies device power.", 1, true);
        label(20, 135, 280, "External VBUS interlock enforced.", 1, true);
        approve = UiAction::ConfirmUsbHost; cancel = UiAction::CancelUsbHost;
    } else {
        label(20, 26, 280, "Reset BLE bonds?", 2);
        label(20, 69, 280, "Forget previously paired computers.");
        label(20, 95, 280, "Pair again after reset.", 1, true);
        approve = UiAction::ConfirmClearBonds; cancel = UiAction::CancelClearBonds;
    }
    button({20, 178, 132, 42}, current == Modal::Pairing ? "Reject" : "Cancel", cancel);
    button({168, 178, 132, 42}, "Approve", approve);
}
void Controller::render() {
    const bool reader_page = page_ == Page::Hf || page_ == Page::Uhf || page_ == Page::Lf;
    if (reader_page && tools_.active() && state_.mode == WorkMode::Standalone &&
        modal() == Modal::None && !state_.transition_pending && !state_.release_unverified) {
        hit_count_ = 0; count_ = 0; previous_count_ = 0;
        tools_.paint(surface_, local_, tools_obscured_);
        tools_obscured_ = false;
        return;
    }
    if (tools_.active()) tools_obscured_ = true;
    canvas_ = state_.light_theme ? 0xf0f4f8 : 0x060c14;
    surface_color_ = state_.light_theme ? 0xffffff : 0x111f30;
    accent_ = state_.light_theme ? 0x007c72 : 0x00dfbd;
    text_ = state_.light_theme ? 0x102438 : 0xf2f7fc;
    muted_ = state_.light_theme ? 0x506477 : 0x91a9bf;
    border_ = state_.light_theme ? 0xcbd7e2 : 0x2a4055;
    danger_ = state_.light_theme ? 0xa32142 : 0xff94ac;
    label_background_ = canvas_;
    count_ = 0; hit_count_ = 0;
    fill(screen, canvas_);
    switch (page_) {
    case Page::Dashboard: dashboard(); break;
    case Page::Settings: settings(); break;
    case Page::Diagnostics: diagnostics(); break;
    default: detail(); break;
    }
    if (state_.transition_pending && modal() == Modal::None) {
        hit_count_ = 0;
        fill({0, 36, 320, 204}, canvas_);
        label(16, 70, 288, "Switching owner...", 2);
        label(16, 108, 288, mode_title(state_.transition_target), 2);
        label(16, 144, 288, "Waiting for safe hardware release.", 1, true);
        label(16, 166, 288, "Controls paused during transition.", 1, true);
    }
    overlay();
    present();
}
void Controller::present() {
    Rect dirty{0, 0, 0, 0};
    auto mark = [&](Rect r) { dirty = dirty.w ? united(dirty, r) : r; };
    const size_t total = std::max(count_, previous_count_);
    for (size_t i = 0; i < total; ++i) {
        bool same = i < count_ && i < previous_count_;
        if (same) {
            const auto& a = commands_[i]; const auto& b = previous_[i];
            same = a.ink == b.ink && a.color == b.color && a.background == b.background && a.scale == b.scale &&
                a.rect.x == b.rect.x && a.rect.y == b.rect.y && a.rect.w == b.rect.w &&
                a.rect.h == b.rect.h && std::strcmp(a.label, b.label) == 0;
        }
        if (!same) {
            if (i < count_) mark(commands_[i].rect);
            if (i < previous_count_) mark(previous_[i].rect);
        }
    }
    if (dirty.w) {
        surface_.paint(dirty, [this, &dirty](Surface& target) {
            for (size_t i = 0; i < count_; ++i) {
                const auto& cmd = commands_[i];
                if (!intersects(cmd.rect, dirty)) continue;
                if (cmd.ink == Ink::Fill) target.fill(cmd.rect, cmd.color);
                else if (cmd.ink == Ink::Text)
                    target.text(cmd.rect.x, cmd.rect.y, cmd.label, cmd.scale, cmd.color, cmd.background);
                else if (cmd.ink == Ink::Panel) {
                    const float radius = std::min(float(cmd.scale), float(cmd.rect.h) / 2);
                    for (int row = 0; row < cmd.rect.h; ++row) {
                        const float dy = std::max(0.0f, radius - std::min(row + 0.5f, cmd.rect.h - row - 0.5f));
                        const float inset = radius - std::sqrt(std::max(0.0f, radius * radius - dy * dy));
                        const int edge = int(std::ceil(inset));
                        target.fill({cmd.rect.x + edge, cmd.rect.y + row, cmd.rect.w - 2 * edge, 1}, cmd.color);
                        if (edge) {
                            const uint32_t color = mix_color(cmd.color, cmd.background, unsigned((edge - inset) * 255));
                            target.fill({cmd.rect.x + edge - 1, cmd.rect.y + row, 1, 1}, color);
                            target.fill({cmd.rect.x + cmd.rect.w - edge, cmd.rect.y + row, 1, 1}, color);
                        }
                    }
                }
                else {
                    for (int y = 0; y < logo_height; ++y) {
                        int x = 0;
                        while (x < logo_width) {
                            const uint32_t color = blended(logo_rgba[y * logo_width + x], cmd.color);
                            const int begin = x++;
                            while (x < logo_width && blended(logo_rgba[y * logo_width + x], cmd.color) == color) ++x;
                            target.fill({cmd.rect.x + begin, cmd.rect.y + y, x - begin, 1}, color);
                        }
                    }
                }
            }
        });
    }
    previous_ = commands_; previous_count_ = count_;
}
UiAction Controller::touch(int x, int y) {
    if (!initialized_ || !screen.contains(x, y)) return UiAction::None;
    const bool reader_page = page_ == Page::Hf || page_ == Page::Uhf || page_ == Page::Lf;
    if (reader_page && tools_.active() && modal() == Modal::None && !state_.transition_pending &&
        state_.mode == WorkMode::Standalone && !state_.release_unverified) {
        auto intent = tools_.touch(x, y, local_);
        if (intent.command == local::Command::Exit) {
            page_ = Page::Dashboard; tools_obscured_ = true; previous_count_ = 0;
        }
        const bool request = intent.command != local::Command::None;
        if (request) pending_intent_ = intent;
        render();
        return request ? UiAction::LocalRequest : UiAction::None;
    }
    for (size_t i = 0; i < hit_count_; ++i) {
        const auto hit = hits_[i];
        if (!hit.rect.contains(x, y)) continue;
        if (hit.navigate) {
            page_ = hit.page; settings_page_ = 0;
            if (state_.mode == WorkMode::Standalone && (page_ == Page::Hf || page_ == Page::Uhf || page_ == Page::Lf)) {
                const auto tool = page_ == Page::Hf ? local::Tool::Hf : page_ == Page::Uhf ? local::Tool::Uhf : local::Tool::Lf;
                tools_.open(tool); tools_obscured_ = true;
                pending_intent_ = {};
                pending_intent_.command = tool == local::Tool::Hf ? local::Command::OpenHf :
                    tool == local::Tool::Uhf ? local::Command::OpenUhf : local::Command::OpenLf;
                render(); return UiAction::LocalRequest;
            }
            render();
        }
        else if (hit.settings_next) { settings_page_ ^= 1; render(); }
        return hit.action;
    }
    return UiAction::None;
}
local::Intent Controller::take_local_intent() {
    local::Intent intent = pending_intent_;
    pending_intent_ = {};
    return intent;
}
}
