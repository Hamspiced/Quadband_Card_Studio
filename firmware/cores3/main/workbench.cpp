#include "workbench.hpp"

namespace qb {
bool pairing_reply_matches(bool approve, uint64_t expected, uint64_t current) {
    return expected ? expected == current : !approve;
}
bool release_mode_verified(const Bytes& acknowledgement, const Bytes& readback, uint8_t original) {
    auto success = [](const Bytes& frame, unsigned command, size_t size) {
        return frame.size() == size && frame[2] == (command >> 8) && frame[3] == (command & 255) &&
            frame[4] == 0 && frame[5] == 0x68;
    };
    return original <= 1 && success(acknowledgement, 1001, 10) &&
        success(readback, 1002, 11) && readback[9] == original;
}
const char* mode_name(WorkMode mode) {
    switch (mode) {
    case WorkMode::Connected: return "connected";
    case WorkMode::Standalone: return "standalone";
    default: return "home";
    }
}
const char* mode_owner(WorkMode mode) {
    switch (mode) {
    case WorkMode::Connected: return "desktop";
    case WorkMode::Standalone: return "local";
    default: return "none";
    }
}
ModeRequest ModeBroker::request(WorkMode target, bool physical_busy, bool paired, bool confirmed) {
    if (pending_) return target == target_ ? ModeRequest::Unchanged : ModeRequest::Busy;
    if (target == active_) return ModeRequest::Unchanged;
    if (!confirmed && active_ != WorkMode::Home && (physical_busy || paired))
        return ModeRequest::Confirm;
    target_ = target;
    pending_ = true;
    return ModeRequest::Queued;
}
bool ModeBroker::complete(bool physical_idle) {
    if (!pending_ || !physical_idle) return false;
    active_ = target_;
    pending_ = false;
    return true;
}
Status ModeBroker::admit(Session& session, const Frame& frame, bool authorized, const std::array<Slot, 3>& slots) const {
    Status status = session.admit(frame, authorized, slots);
    if (status == OK && frame.channel && !remote_allowed()) {
        if (session.pending == frame.id) session.pending = 0;
        return BUSY;
    }
    return status;
}
bool ModeBroker::cancel_remote(Session& session, uint32_t request_id) const {
    if (!remote_allowed() || !request_id || !session.pending || session.pending != request_id) return false;
    session.cancelled = true;
    return true;
}
bool TouchGate::permit(const UiSnapshot& state, bool pressed, bool touching) {
    uint64_t key = unsigned(state.mode) | (uint64_t(state.transition_pending) << 2) |
        (uint64_t(state.confirm_mode) << 3) | (uint64_t(state.confirm_usb) << 4) |
        (uint64_t(state.confirm_bonds) << 5) | (uint64_t(state.pairing_pending) << 6) |
        (uint64_t(state.confirm_target) << 7) | (uint64_t(state.pairing_code) << 10);
    const uint64_t local_review = state.local.review ? state.local.review_token : 0;
    if (key != presentation_ || state.pairing_challenge != challenge_ || local_review != local_review_) {
        presentation_ = key; challenge_ = state.pairing_challenge; local_review_ = local_review; armed_ = false;
        return false;
    }
    if (!touching) { armed_ = true; return false; }
    if (!pressed || !armed_) return false;
    armed_ = false;
    return true;
}
}
