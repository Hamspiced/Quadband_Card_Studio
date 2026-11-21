#pragma once
#include "wire.hpp"

namespace qb::zero::platform {
class Radio {
public:
    virtual ~Radio() = default;
    virtual Status initialize() = 0;
    virtual Status identify(local::HfIdentity&) = 0;
    virtual Status authenticate(const local::HfIdentity&, unsigned, const local::ClassicKey&) = 0;
    virtual Status authenticate(const local::NtagAuth&, bool&) = 0;
    virtual Status read(unsigned address, Bytes&) = 0;
    virtual Status write(unsigned address, const Bytes&) = 0;
    virtual Status release(Deadline deadline = Clock::now() + std::chrono::seconds(2)) = 0;
};
std::unique_ptr<Radio> make_register_radio(Wire&, Stop);
}
