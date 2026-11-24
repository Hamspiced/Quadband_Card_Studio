#pragma once
#include "bridge.hpp"
#include "local_model.hpp"
#include "ultra_link.hpp"

namespace qb {
enum class WorkMode : uint8_t { Home, Connected, Standalone };
enum class ModeRequest : uint8_t { Unchanged, Confirm, Queued, Busy };
const char* mode_name(WorkMode);
const char* mode_owner(WorkMode);
bool pairing_reply_matches(bool approve, uint64_t expected, uint64_t current);
bool release_mode_verified(const Bytes& acknowledgement, const Bytes& readback, uint8_t original);

class ModeBroker {
    WorkMode active_ = WorkMode::Home;
    WorkMode target_ = WorkMode::Home;
    bool pending_ = false;
public:
    WorkMode mode() const { return active_; }
    WorkMode target() const { return target_; }
    bool pending() const { return pending_; }
    bool remote_allowed() const { return active_ == WorkMode::Connected && !pending_; }
    bool local_allowed() const { return active_ == WorkMode::Standalone && !pending_; }
    Status admit(Session&, const Frame&, bool authorized, const std::array<Slot, 3>&) const;
    bool cancel_remote(Session&, uint32_t request_id) const;
    ModeRequest request(WorkMode target, bool physical_busy, bool paired, bool confirmed = false);
    bool complete(bool physical_idle);
    void release_failed() { active_ = target_ = WorkMode::Home; pending_ = false; }
};

enum class UsbHostState : uint8_t { Off, Requested, Enabled, ExternalPowerBlocked, Failed };
enum class UiAction : uint8_t {
    None, ChooseConnected, ChooseStandalone, ChooseHome, ConfirmMode, CancelMode,
    OpenPairing, ApprovePairing, RejectPairing, RequestUsbHost, ConfirmUsbHost,
    CancelUsbHost, RequestClearBonds, ConfirmClearBonds, CancelClearBonds,
    Stop, ToggleTheme, Refresh, LocalRequest, UltraScan, UltraUsb, UltraDisconnect, UltraPin,
    UltraPeer0, UltraPeer1, UltraPeer2, UltraPeer3, UltraPeer4, UltraPeer5
};

struct UiSnapshot {
    WorkMode mode = WorkMode::Home;
    WorkMode transition_target = WorkMode::Home;
    bool transition_pending = false;
    bool confirm_mode = false;
    WorkMode confirm_target = WorkMode::Home;
    bool confirm_usb = false, confirm_bonds = false;
    bool release_unverified = false;
    bool paired = false, pairing_pending = false, pairing_open = false, busy = false;
    uint32_t pairing_code = 0, request_id = 0;
    uint64_t pairing_challenge = 0;
    unsigned last_status = 0;
    int battery_percent = -1;
    uint32_t internal_bytes = 0, external_bytes = 0;
    UsbHostState usb_host = UsbHostState::Off;
    bool light_theme = false;
    std::array<Slot, 3> peripherals;
    std::string message;
    local::View local;
    UltraView ultra;
    UltraTransport ultra_transport = UltraTransport::Usb;
};

class TouchGate {
    uint64_t presentation_ = UINT64_MAX, challenge_ = 0, local_review_ = 0;
    bool armed_ = false;
    uint64_t ultra_epoch_ = 0, ultra_challenge_ = 0;
public:
    bool permit(const UiSnapshot&, bool pressed, bool touching);
};

namespace ui {
// The UI owns rendering/navigation only; the runtime owns all physical actions.
void initialize();
void update(const UiSnapshot&);
UiAction touch(int x, int y);
local::Intent take_local_intent();
void take_ultra_pin(char (&digits)[7]);
}
}
