#pragma once
#include "../workbench.hpp"
#include "tools.hpp"
#include "surface.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace qb::ui {
enum class Page : uint8_t { Dashboard, Uhf, Hf, Lf, Files, Settings, Diagnostics, Ultra };
enum class Modal : uint8_t { None, Pairing, Mode, Usb, Bonds, UltraPin };

class Controller {
public:
    explicit Controller(Surface& surface) : surface_(surface) {}
    void initialize();
    void update(const UiSnapshot&);
    UiAction touch(int x, int y);
    Page page() const { return page_; }
    Modal modal() const;
    size_t command_count() const { return count_; }
    local::Intent take_local_intent();
    void take_ultra_pin(char (&digits)[7]);
private:
    enum class Ink : uint8_t { Fill, Text, Logo, Panel };
    struct Command {
        Rect rect{};
        uint32_t color = 0;
        uint32_t background = 0;
        Ink ink = Ink::Fill;
        uint8_t scale = 1;
        char label[53]{};
    };
    struct Hit {
        Rect rect{};
        UiAction action = UiAction::None;
        Page page = Page::Dashboard;
        bool navigate = false, settings_next = false;
    };
    struct Module {
        uint32_t generation = 1;
        bool present = false;
        char kind[33] = "unknown";
        char capabilities[3][41]{};
        size_t capability_count = 0;
    };
    struct State {
        WorkMode mode = WorkMode::Home, transition_target = WorkMode::Home, confirm_target = WorkMode::Home;
        bool transition_pending = false, confirm_mode = false, confirm_usb = false, confirm_bonds = false;
        bool release_unverified = false;
        bool paired = false, pairing_pending = false, pairing_open = false, busy = false, light_theme = false;
        uint32_t pairing_code = 0, request_id = 0, internal_bytes = 0, external_bytes = 0;
        unsigned last_status = 0;
        int battery_percent = -1;
        UsbHostState usb_host = UsbHostState::Off;
        Module peripherals[3]{};
        char message[105]{};
    };
    static constexpr size_t max_commands = 64, max_hits = 12;
    Surface& surface_;
    std::array<Command, max_commands> commands_{}, previous_{};
    std::array<Hit, max_hits> hits_{};
    size_t count_ = 0, previous_count_ = 0, hit_count_ = 0;
    State state_{};
    local::View local_{};
    UltraView ultra_{};
    UltraTransport transport_ = UltraTransport::Usb;
    char pin_[7]{};
    size_t pin_size_ = 0;
    local::Intent pending_intent_;
    ToolsUi tools_;
    bool tools_obscured_ = true;
    Page page_ = Page::Dashboard;
    unsigned settings_page_ = 0;
    bool initialized_ = false;
    uint32_t canvas_ = 0, surface_color_ = 0, accent_ = 0, text_ = 0, muted_ = 0;
    uint32_t border_ = 0, danger_ = 0;
    uint32_t label_background_ = 0;
    void render();
    void present();
    void add(Ink, Rect, uint32_t, const char* = "", unsigned scale = 1);
    void fill(Rect, uint32_t);
    void panel(Rect, uint32_t, uint32_t background, unsigned radius = 8);
    void label(int, int, int width, const char*, unsigned style = 1, bool muted = false, uint32_t background = UINT32_MAX);
    void button(Rect, const char*, UiAction, bool enabled = true);
    void navigation(Rect, const char*, Page);
    void header(const char*);
    void dashboard();
    void connected();
    void standalone();
    void detail();
    void settings();
    void diagnostics();
    void ultra();
    void overlay();
};
}
