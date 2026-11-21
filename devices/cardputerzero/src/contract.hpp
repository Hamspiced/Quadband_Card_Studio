#pragma once
#include "local_model.hpp"
#include <functional>
#include <memory>
#include <string>

namespace qb::zero {
struct ReaderConfig {
    std::string hf_i2c;
    unsigned hf_address = 0x28;
    std::string uhf_serial;
    unsigned uhf_baud = 115200;
    std::string ultra_serial;
    bool grove_i2c_confirmed = false;
};
using Progress = std::function<void(unsigned, unsigned, const char*)>;
enum class Peripheral : uint8_t { Uhf, Rfid, Cc1101, CapNfc };
enum class Detection : uint8_t { Unchecked, Detected, Missing, Busy, Permission, Unsupported, Blocked, Cancelled, Manual };
struct DeviceDetection {
    Detection state = Detection::Unchecked;
    std::string endpoint, identity, detail;
};
struct DiscoveryReport {
    uint64_t sequence = 0;
    ReaderConfig config;
    std::array<DeviceDetection, 4> devices{};
    bool attempted = false, completed = false;
};

// All device methods are worker-owned except request_cancel(), which is thread-safe.
class ReaderPort : public local::Port {
public:
    virtual Status connect() = 0;
    virtual Status release() = 0;
    virtual void request_cancel() = 0;
    virtual void resume() = 0;
    virtual std::string detail() const = 0;
};
std::unique_ptr<ReaderPort> make_linux_port(const ReaderConfig&, Progress);
std::unique_ptr<ReaderPort> make_simulated_port(Progress);

struct Snapshot {
    local::View view;
    ReaderConfig config;
    bool pending = false;
    bool connected = false;
    bool cleanup_failed = false;
    bool simulated = false;
    DiscoveryReport discovery;
    std::string detail = "Choose reader endpoints; no scan runs automatically.";
};
struct Callbacks {
    std::function<bool(const local::Intent&)> submit;
    std::function<bool(const ReaderConfig&)> connect;
    std::function<void()> stop;
    std::function<void()> quit;
    std::function<bool(const ReaderConfig&)> detect;
};
enum class Key {
    Character, Up, Down, Left, Right, Tab, Enter, Escape, Backspace,
    Delete, Home, End, PageUp, PageDown, Stop, Quit
};
struct KeyEvent {
    Key key = Key::Character;
    char character = 0;
    bool pressed = false;
    bool repeat = false;
    bool shift = false;
};
}
