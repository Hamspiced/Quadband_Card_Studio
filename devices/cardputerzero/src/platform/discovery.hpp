#pragma once
#include "../contract.hpp"
#include <chrono>

namespace qb::zero::platform {
using DiscoveryClock = std::chrono::steady_clock;
struct DiscoveryEnvironment {
    bool zero = false, uart = false, i2c = false, cap_power = false, spi_routes = false;
};
class DiscoveryProbe {
public:
    virtual ~DiscoveryProbe() = default;
    virtual DiscoveryEnvironment environment() = 0;
    virtual Status grove_power() = 0;
    virtual Status identify(Peripheral, const ReaderConfig&, Bytes&, const std::function<bool()>&,
                            DiscoveryClock::time_point) = 0;
};
DiscoveryReport discover(const ReaderConfig&, DiscoveryProbe&, const std::function<bool()>&,
                         DiscoveryClock::time_point deadline);
DiscoveryReport discover_linux(const ReaderConfig&, const std::function<bool()>&);
const char* detection_name(Detection);
Bytes identity_request(Peripheral, unsigned step = 0);
}
