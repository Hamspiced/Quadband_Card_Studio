#pragma once
#include "../contract.hpp"
#include <chrono>
#include <vector>

namespace qb::zero::platform {
using DiscoveryClock = std::chrono::steady_clock;
struct DiscoveryEnvironment {
    bool zero = false, uart = false, i2c = false, cap_power = false;
    bool cc1101_spi = false, cap_nfc_spi = false;
    std::string cap_power_detail;
};
class DiscoveryProbe {
public:
    virtual ~DiscoveryProbe() = default;
    virtual DiscoveryEnvironment environment() = 0;
    virtual std::vector<std::string> ultra_endpoints(const std::function<bool()>&,
                                                    DiscoveryClock::time_point) { return {}; }
    virtual Status grove_power() = 0;
    virtual Status identify(Peripheral, const ReaderConfig&, Bytes&, const std::function<bool()>&,
                            DiscoveryClock::time_point) = 0;
};
DiscoveryReport discover(const ReaderConfig&, DiscoveryProbe&, const std::function<bool()>&,
                         DiscoveryClock::time_point deadline);
DiscoveryReport discover_linux(const ReaderConfig&, const std::function<bool()>&);
const char* detection_name(Detection);
Bytes identity_request(Peripheral, unsigned step = 0);
std::vector<std::string> ultra_usb_endpoints(const std::string& tty_root = "/sys/class/tty",
                                          const std::string& device_root = "/dev",
                                          const std::function<bool()>& stop = {},
                                          DiscoveryClock::time_point deadline = DiscoveryClock::time_point::max());
class Wire;
Status identify_ultra(Wire&, Bytes&, const std::function<bool()>&, DiscoveryClock::time_point);
}
