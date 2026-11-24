#include "../main/ui/controller.hpp"
#include "../main/ui/typography.hpp"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using qb::UiAction;
using qb::UiSnapshot;
using qb::UsbHostState;
using qb::WorkMode;
using namespace qb::ui;

namespace {
struct Draw {
    Rect rect, clip;
    uint32_t color;
    unsigned scale;
    std::string text;
    uint32_t background = 0;
};
class RecordingSurface final : public Surface {
public:
    Rect clipping{0, 0, 320, 240};
    std::vector<Draw> draws;
    std::vector<Rect> clips;
    void clip(Rect r) override {
        assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= 320 && r.y + r.h <= 240);
        clipping = r; clips.push_back(r);
    }
    void fill(Rect r, uint32_t color) override {
        assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= 320 && r.y + r.h <= 240);
        draws.push_back({r, clipping, color, 0, {}});
    }
    void text(int x, int y, const char* value, unsigned scale, uint32_t color, uint32_t background) override {
        const int w = text_width(value, scale);
        assert(x >= 0 && y >= 0 && x + w <= 320 && y + text_height(scale) <= 240);
        assert(w <= 312 && std::char_traits<char>::length(value) <= 256);
        draws.push_back({{x, y, w, text_height(scale)}, clipping, color, scale, value, background});
    }
    bool saw(const std::string& value) const {
        return std::any_of(draws.begin(), draws.end(), [&](const Draw& d) {
            return d.text.find(value) != std::string::npos;
        });
    }
    static std::string escape(const std::string& value) {
        std::string out;
        for (char c : value) {
            if (c == '&') out += "&amp;";
            else if (c == '<') out += "&lt;";
            else if (c == '>') out += "&gt;";
            else if (c == '"') out += "&quot;";
            else out += c;
        }
        return out;
    }
    void svg(const std::filesystem::path& path) const {
        std::ofstream out(path);
        assert(out);
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"240\" viewBox=\"0 0 320 240\">\n";
        // Every operation below comes from the same Surface used by the production M5 adapter.
        size_t i = 0;
        for (const auto& d : draws) {
            out << "<defs><clipPath id=\"c" << i << "\"><rect x=\"" << d.clip.x
                << "\" y=\"" << d.clip.y << "\" width=\"" << d.clip.w << "\" height=\"" << d.clip.h
                << "\"/></clipPath></defs><g clip-path=\"url(#c" << i << ")\">";
            ++i;
            std::ostringstream rgb;
            rgb << '#' << std::hex << std::setw(6) << std::setfill('0') << d.color;
            if (!d.scale)
                out << "<rect x=\"" << d.rect.x << "\" y=\"" << d.rect.y << "\" width=\"" << d.rect.w
                    << "\" height=\"" << d.rect.h << "\" fill=\"" << rgb.str() << "\"/>";
            else {
                raster_text(d.rect.x, d.rect.y, d.text.c_str(), d.scale, d.color, d.background,
                    [&](int x, int y, int width, uint32_t color) {
                        out << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << width
                            << "\" height=\"1\" fill=\"#" << std::hex << std::setw(6) << std::setfill('0')
                            << color << std::dec << "\"/>";
                    });
            }
            out << "</g>\n";
        }
        out << "</svg>\n";
    }
};
struct Fixture {
    RecordingSurface surface;
    Controller ui{surface};
    UiSnapshot state;
    Fixture() { ui.initialize(); }
    void update() { ui.update(state); assert(ui.command_count() < 64); }
    void mode(WorkMode value) { state.mode = value; update(); }
    void settings() {
        if (state.mode == WorkMode::Home) ui.touch(30, 225);
        else if (state.mode == WorkMode::Standalone) ui.touch(220, 225);
        else ui.touch(250, 210);
        assert(ui.page() == Page::Settings);
    }
};

void ultra_selection_and_pin() {
    Fixture f; f.settings(); f.ui.touch(40, 150);
    assert(f.ui.page() == Page::Ultra);
    assert(f.ui.touch(40, 140) == UiAction::UltraScan);
    assert(f.ui.touch(220, 140) == UiAction::UltraUsb);
    f.state.ultra.phase = qb::UltraPhase::Selecting; f.state.ultra.count = 6;
    for (unsigned i = 0; i < 6; ++i)
        std::snprintf(f.state.ultra.peers[i].label,40,"NUS selected candidate %u",i);
    f.update();
    for (unsigned i=0;i<6;++i)
        assert(f.ui.touch(40,90+int(i)*20) == UiAction(unsigned(UiAction::UltraPeer0)+i));
    f.state.busy = true; f.update(); assert(f.ui.touch(40,90)==UiAction::None);
    f.state.busy = false; f.state.ultra.phase = qb::UltraPhase::Pairing;
    f.state.ultra.challenge = 91; std::snprintf(f.state.ultra.selected.label,40,"Selected Ultra peer label");
    f.update(); assert(f.ui.modal()==Modal::UltraPin);
    assert(f.ui.touch(40,215)==UiAction::None);
    for(unsigned i=0;i<6;++i) f.ui.touch(20,100);
    assert(f.ui.touch(40,215)==UiAction::UltraPin);
    assert(!f.surface.saw(std::string(6,'0')));
    char digits[7]{}; f.ui.take_ultra_pin(digits);
    assert(std::all_of(digits,digits+6,[](char c){return c=='0';}) && !digits[6]);
    std::fill_n(digits,7,0); f.ui.take_ultra_pin(digits);
    assert(std::all_of(digits,digits+7,[](char c){return !c;}));
    for(unsigned i=0;i<6;++i) f.ui.touch(20,100);
    ++f.state.ultra.challenge; f.update(); assert(f.ui.touch(40,215)==UiAction::None);
    assert(f.ui.touch(150,180)==UiAction::UltraDisconnect);
    f.state.pairing_pending=true; f.update(); assert(f.ui.modal()==Modal::Pairing);
}
void home_and_authority() {
    Fixture f;
    f.state.last_status = qb::IO_ERROR; f.update();
    assert(!f.surface.saw("Release unverified"));
    f.state.release_unverified = true; f.update();
    assert(f.surface.saw("Release unverified"));
    assert(f.surface.saw("Restart CoreS3"));
    f.state.confirm_mode = f.state.confirm_usb = f.state.confirm_bonds = true;
    f.update(); assert(f.ui.modal() == Modal::None);
    assert(f.ui.touch(30, 185) == UiAction::None);
    assert(f.ui.touch(200, 185) == UiAction::None);
    f.state.confirm_mode = f.state.confirm_usb = f.state.confirm_bonds = false;
    f.state.release_unverified = false; f.state.last_status = 0; f.update();
    assert(f.surface.saw("BAT --"));
    assert(f.ui.touch(-1, 180) == UiAction::None);
    assert(f.ui.touch(320, 180) == UiAction::None);
    assert(f.ui.touch(170, 240) == UiAction::None);
    assert(f.ui.touch(8, 166) == UiAction::ChooseConnected);
    assert(f.ui.touch(155, 205) == UiAction::ChooseConnected);
    assert(f.ui.touch(156, 205) == UiAction::None);
    assert(f.ui.touch(164, 166) == UiAction::ChooseStandalone);
    // Selection does not change the authoritative Home snapshot.
    assert(f.ui.touch(8, 166) == UiAction::ChooseConnected);
    const auto draws = f.surface.draws.size();
    f.update();
    assert(f.surface.draws.size() == draws);
    f.state.battery_percent = 74;
    f.surface.clips.clear();
    f.update();
    assert(f.surface.saw("BAT 74%"));
    assert(f.surface.clips.front().w * f.surface.clips.front().h < 320 * 36);
    f.mode(WorkMode::Connected);
    assert(f.ui.touch(40, 210) == UiAction::Stop);
    assert(f.ui.touch(150, 210) == UiAction::ChooseHome);
    assert(f.ui.touch(40, 210) == UiAction::Stop);
    f.state.paired = true; f.state.busy = true; f.state.pairing_open = true;
    f.update();
    assert(f.surface.saw("paired / authorized") && f.surface.saw("remote request running"));
    f.settings();
    assert(f.ui.touch(80, 110) == UiAction::OpenPairing);
    assert(f.ui.touch(80, 220) == UiAction::None);
    assert(f.ui.page() == Page::Dashboard);
    assert(f.ui.touch(40, 210) == UiAction::Stop);
}
void standalone_navigation() {
    Fixture f;
    f.mode(WorkMode::Standalone);
    const int xs[] = {40, 200, 40};
    const int ys[] = {90, 90, 150};
    const Page pages[] = {Page::Uhf, Page::Hf, Page::Lf};
    const qb::local::Tool tools[] = {qb::local::Tool::Uhf, qb::local::Tool::Hf, qb::local::Tool::Lf};
    const qb::local::Command commands[] = {qb::local::Command::OpenUhf, qb::local::Command::OpenHf, qb::local::Command::OpenLf};
    for (unsigned i = 0; i < 3; ++i) {
        assert(f.ui.touch(xs[i], ys[i]) == UiAction::LocalRequest);
        assert(f.ui.take_local_intent().command == commands[i]);
        assert(f.ui.page() == pages[i]);
        f.state.local.tool = tools[i]; f.state.local.reader_present = true; ++f.state.local.revision; f.update();
        assert(f.ui.touch(30, 16) == UiAction::LocalRequest);
        assert(f.ui.take_local_intent().command == qb::local::Command::Exit);
        assert(f.ui.page() == Page::Dashboard);
    }
    assert(f.ui.touch(200, 150) == UiAction::None);
    assert(f.ui.page() == Page::Files);
    assert(f.surface.saw("File services unavailable"));
    assert(f.ui.touch(30, 220) == UiAction::None);
    assert(f.ui.touch(30, 220) == UiAction::ChooseHome);
    assert(f.ui.page() == Page::Dashboard);
    f.settings();
    assert(f.ui.touch(30, 220) == UiAction::None);
    assert(f.ui.page() == Page::Dashboard);
    assert(f.ui.touch(30, 220) == UiAction::ChooseHome);
    f.state.local.tool = qb::local::Tool::Uhf; ++f.state.local.revision; f.update();
    assert(f.ui.touch(30, 90) == UiAction::LocalRequest);
    f.mode(WorkMode::Connected);
    assert(f.ui.page() == Page::Dashboard);
    assert(f.ui.touch(30, 210) == UiAction::Stop);
}
void exhaustive_modal(Fixture& f, UiAction approve, UiAction cancel) {
    // No pixel outside these explicit buttons may approve or dispatch a background action.
    for (int y = -1; y <= 240; ++y) {
        for (int x = -1; x <= 320; ++x) {
            const auto expected = Rect{20, 178, 132, 42}.contains(x, y) ? cancel :
                Rect{168, 178, 132, 42}.contains(x, y) ? approve : UiAction::None;
            assert(f.ui.touch(x, y) == expected);
        }
    }
}
void modal_priority() {
    Fixture f;
    f.state.confirm_bonds = f.state.confirm_usb = f.state.confirm_mode = true;
    f.state.confirm_target = WorkMode::Standalone;
    f.state.pairing_pending = true; f.state.pairing_code = 1234;
    f.update();
    assert(f.ui.modal() == Modal::Pairing && f.surface.saw("Code: 001234"));
    exhaustive_modal(f, UiAction::ApprovePairing, UiAction::RejectPairing);
    f.state.transition_pending = true; f.update();
    assert(f.ui.modal() == Modal::Pairing);
    exhaustive_modal(f, UiAction::ApprovePairing, UiAction::RejectPairing);
    f.state.pairing_pending = false; f.update();
    assert(f.ui.modal() == Modal::None);
    for (int y = 0; y < 240; y += 4)
        for (int x = 0; x < 320; x += 4) assert(f.ui.touch(x, y) == UiAction::None);
    f.state.transition_pending = false; f.update();
    assert(f.ui.modal() == Modal::Mode && f.surface.saw("Standalone"));
    exhaustive_modal(f, UiAction::ConfirmMode, UiAction::CancelMode);
    f.state.confirm_mode = false; f.update();
    assert(f.ui.modal() == Modal::Usb);
    assert(f.surface.saw("Disconnect PC / external VBUS first."));
    assert(f.surface.saw("USB output supplies device power."));
    assert(f.surface.saw("External VBUS interlock enforced."));
    exhaustive_modal(f, UiAction::ConfirmUsbHost, UiAction::CancelUsbHost);
    f.state.confirm_usb = false; f.update();
    assert(f.ui.modal() == Modal::Bonds);
    exhaustive_modal(f, UiAction::ConfirmClearBonds, UiAction::CancelClearBonds);
}
void settings_theme_usb() {
    Fixture f;
    f.settings();
    const size_t before = f.surface.draws.size();
    assert(f.ui.touch(80, 60) == UiAction::ToggleTheme);
    assert(f.surface.draws.size() == before);
    f.state.light_theme = true; f.update();
    assert(f.surface.saw("Theme: Light"));
    assert(std::any_of(f.surface.draws.begin(), f.surface.draws.end(), [](const Draw& d) {
        return d.color == 0xf0f4f8 && !d.scale;
    }));
    assert(f.ui.touch(80, 110) == UiAction::OpenPairing);
    f.ui.touch(220, 220);
    assert(f.ui.touch(80, 80) == UiAction::RequestUsbHost);
    assert(f.ui.touch(80, 150) == UiAction::RequestClearBonds);
    for (auto state : {UsbHostState::Off, UsbHostState::Requested, UsbHostState::Enabled,
                       UsbHostState::ExternalPowerBlocked, UsbHostState::Failed}) {
        f.state.usb_host = state; f.update();
        const auto expected = state == UsbHostState::Enabled || state == UsbHostState::Requested
            ? UiAction::None : UiAction::RequestUsbHost;
        assert(f.ui.touch(80, 80) == expected);
    }
    assert(f.surface.saw("Requested") && f.surface.saw("Enabled") &&
           f.surface.saw("Blocked: external VBUS") && f.surface.saw("Failed"));
    f.ui.touch(80, 220);
    assert(f.ui.page() == Page::Dashboard);
    f.ui.touch(210, 225);
    assert(f.ui.page() == Page::Diagnostics);
    assert(f.ui.touch(220, 220) == UiAction::Refresh);
}
void bounded_and_transitions() {
    static_assert(sizeof(Controller) < 32768, "UI must fit a bounded internal-RAM budget");
    Fixture f;
    f.state.message = std::string(10000, 'X');
    f.state.peripherals[1].kind = std::string(10000, 'Y');
    f.state.peripherals[1].present = true;
    f.state.peripherals[1].capabilities = std::vector<std::string>(100, std::string(10000, 'Z'));
    f.state.local.tool = qb::local::Tool::Hf;
    f.state.local.identity.fill('Y'); f.state.local.message.fill('X');
    f.mode(WorkMode::Standalone);
    f.ui.touch(200, 90);
    assert(f.surface.saw("YYYY"));
    assert(f.surface.saw("..."));
    f.ui.touch(30, 16);
    f.settings();
    f.ui.touch(220, 220);
    f.state.transition_pending = true; f.state.transition_target = WorkMode::Connected;
    f.update();
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) assert(f.ui.touch(x, y) == UiAction::None);
    assert(f.surface.saw("Switching owner..."));
    f.state.transition_pending = false; f.mode(WorkMode::Connected);
    f.state.battery_percent = 10000; f.update();
    assert(f.surface.saw("BAT 100%"));
}
void evidence(const std::filesystem::path& out) {
    std::filesystem::create_directories(out);
    for (bool light : {false, true}) {
        const std::string suffix = light ? "-light" : "-dark";
        Fixture f;
        f.state.light_theme = light;
        f.state.battery_percent = 82;
        f.state.internal_bytes = 142336; f.state.external_bytes = 0;
        f.state.peripherals[0] = {2, true, "rfid2", {"identify", "read"}};
        f.state.peripherals[1] = {3, true, "jrd4035", {"inventory", "read"}};
        f.state.peripherals[2].kind = "chameleon";
        f.update(); f.surface.svg(out / ("home" + suffix + ".svg"));
        f.mode(WorkMode::Connected);
        f.state.paired = true; f.state.pairing_open = true; f.update();
        f.surface.svg(out / ("connected" + suffix + ".svg"));
        f.mode(WorkMode::Standalone);
        f.surface.svg(out / ("standalone" + suffix + ".svg"));
        f.state.local.tool = qb::local::Tool::Uhf; f.state.local.reader_present = true;
        ++f.state.local.revision; f.update();
        f.ui.touch(50, 90); f.surface.svg(out / ("uhf-info" + suffix + ".svg"));
        f.ui.touch(30, 16); f.ui.touch(200, 150);
        f.surface.svg(out / ("files-unavailable" + suffix + ".svg"));
        f.ui.touch(50, 220); f.settings();
        f.surface.svg(out / ("settings" + suffix + ".svg"));
        f.ui.touch(220, 220);
        f.surface.svg(out / ("usb-settings" + suffix + ".svg"));
        f.state.confirm_usb = true; f.update();
        f.surface.svg(out / ("usb-confirm" + suffix + ".svg"));
        f.state.confirm_bonds = true; f.state.confirm_usb = false; f.update();
        f.surface.svg(out / ("bonds-confirm" + suffix + ".svg"));
        f.state.confirm_mode = true; f.state.confirm_target = WorkMode::Connected; f.update();
        f.surface.svg(out / ("mode-confirm" + suffix + ".svg"));
        f.state.pairing_pending = true; f.state.pairing_code = 628401; f.update();
        f.surface.svg(out / ("pairing-priority" + suffix + ".svg"));
        f.state.pairing_pending = false; f.state.transition_pending = true;
        f.state.transition_target = WorkMode::Connected; f.update();
        f.surface.svg(out / ("switching" + suffix + ".svg"));
    }
}
}
int main(int argc, char** argv) {
    assert(text_width("WWW", 1) > text_width("iii", 1));
    assert(mix_color(0xffffff, 0, 0) == 0);
    assert(mix_color(0xffffff, 0, 255) == 0xffffff);
    bool antialiased = false;
    raster_text(0, 0, "A", 2, 0xffffff, 0, [&](int x, int y, int width, uint32_t color) {
        assert(x >= 0 && y >= 0 && x + width <= text_width("A", 2) && y < text_height(2));
        if (color != 0 && color != 0xffffff) antialiased = true;
    });
    assert(antialiased);
    ultra_selection_and_pin();
    home_and_authority();
    standalone_navigation();
    modal_priority();
    settings_theme_usb();
    bounded_and_transitions();
    if (argc == 2) evidence(argv[1]);
    std::cout << "UI host tests passed: navigation, authoritative modes, modal priority, all modal pixels,\n"
                 "touch bounds, themes, unavailable features, module state, USB states, bounded buffers,\n"
                 "transition action suppression and dirty redraw. Controller bytes: " << sizeof(Controller) << '\n';
}
