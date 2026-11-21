#pragma once
#include "../contract.hpp"
#include <atomic>
#include <chrono>

namespace qb::zero::platform {
using Stop = std::function<bool()>;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
// Only the serialized reader worker calls Wire. Stop is checked between bounded calls.
class Wire {
public:
    virtual ~Wire() = default;
    virtual Status open(const ReaderConfig&) = 0;
    virtual void close() noexcept = 0;
    virtual Status read_register(uint8_t, uint8_t&) = 0;
    virtual Status write_register(uint8_t, uint8_t) = 0;
    virtual Status exchange(uint8_t channel, const Bytes&, Bytes&, unsigned timeout_ms, const Stop&) = 0;
};
std::unique_ptr<Wire> make_posix_wire();
Bytes uhf_frame(uint8_t command, const Bytes& = {});
Bytes ultra_frame(unsigned command, const Bytes& = {});
bool ultra_success(const Bytes&, unsigned status = 0x68);
}
