#pragma once
#include "bridge.hpp"
#include <array>

namespace qb {
enum class UltraTransport : uint8_t { Usb, Bluetooth };
enum class UltraPhase : uint8_t {
    Disconnected, Scanning, Selecting, Connecting, Pairing, Discovering, Subscribing, Validating, Ready, Failed
};
struct UltraPeer {
    std::array<uint8_t, 6> address{};
    uint8_t type = 0;
    char label[40]{};
    bool operator==(const UltraPeer& other) const { return type == other.type && address == other.address; }
};
struct UltraView {
    UltraPhase phase = UltraPhase::Disconnected;
    uint64_t epoch = 1, challenge = 0, scan_epoch = 0;
    int64_t expires = 0;
    std::array<UltraPeer, 6> peers{};
    size_t count = 0;
    UltraPeer selected;
    char message[72] = "Choose USB or scan for a Bluetooth reader.";
};
// The lifecycle token scopes every callback, peer selection, and passkey response.
class UltraLifecycle {
public:
    UltraView view;
    void reset(UltraPhase phase = UltraPhase::Disconnected) {
        ++view.epoch; view.phase = phase; view.challenge = 0; view.expires = 0;
    }
    void scan(int64_t now) {
        reset(UltraPhase::Scanning); view.scan_epoch = view.epoch;
        view.count = 0; view.peers = {}; view.selected = {}; view.expires = now + 8000000;
    }
    bool found(uint64_t epoch, const UltraPeer& peer) {
        if (epoch != view.epoch || view.phase != UltraPhase::Scanning) return false;
        for (size_t i = 0; i < view.count; ++i) if (view.peers[i] == peer) return true;
        if (view.count == view.peers.size()) return false;
        view.peers[view.count++] = peer; return true;
    }
    bool select(uint64_t scan, size_t index, int64_t now) {
        if (scan != view.scan_epoch || view.phase != UltraPhase::Selecting ||
            now > view.expires || index >= view.count) return false;
        view.selected = view.peers[index]; reset(UltraPhase::Connecting);
        view.expires = now + 30000000; return true;
    }
    bool current(uint64_t epoch) const { return epoch == view.epoch; }
    bool input(uint64_t epoch, uint64_t challenge, const char* digits, int64_t now) const {
        if (!current(epoch) || !challenge || challenge != view.challenge ||
            view.phase != UltraPhase::Pairing || now > view.expires || !digits) return false;
        for (unsigned i = 0; i < 6; ++i) if (digits[i] < '0' || digits[i] > '9') return false;
        return digits[6] == '\0';
    }
};
class UltraReply {
    Bytes bytes_;
    unsigned command_ = 0;
    bool failed_ = false;
public:
    void begin(unsigned command) { bytes_.clear(); command_ = command; failed_ = false; }
    bool feed(const uint8_t* p, size_t n) {
        if (failed_ || !n || n > max_frame || bytes_.size() + n > max_frame) { failed_ = true; return false; }
        bytes_.insert(bytes_.end(), p, p + n);
        if (bytes_.size() >= 9) {
            const size_t total = (size_t(bytes_[6]) << 8 | bytes_[7]) + 10;
            if (bytes_[0] != 0x11 || bytes_[1] != 0xef || total > max_payload ||
                bytes_.size() > total || (unsigned(bytes_[2]) << 8 | bytes_[3]) != command_) failed_ = true;
        }
        return !failed_;
    }
    Status take(Bytes& out) {
        if (failed_) return IO_ERROR;
        if (bytes_.size() < 9 || bytes_.size() < (size_t(bytes_[6]) << 8 | bytes_[7]) + 10) return BUSY;
        uint8_t h = 0, p = 0;
        for (size_t i = 0; i < bytes_.size(); ++i) (i < 9 ? h : p) += bytes_[i];
        if (h || p) { failed_ = true; return IO_ERROR; }
        out = bytes_; bytes_.clear(); return OK;
    }
};
}
