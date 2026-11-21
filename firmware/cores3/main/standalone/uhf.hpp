#pragma once
#include "port.hpp"

namespace qb::local {
struct UhfTag {
    std::array<uint8_t, 62> epc{};
    std::array<uint8_t, 2> pc{};
    uint8_t epc_size = 0;
    int16_t rssi = 0;
    uint32_t reads = 0, generation = 0;
    bool same_identity(const UhfTag&) const;
};
enum class UhfBank : uint8_t { Reserved = 0, Epc = 1, Tid = 2, User = 3 };
struct UhfMemory {
    UhfTag tag{};
    UhfBank bank = UhfBank::User;
    uint16_t word = 0, words = 0;
    std::array<uint8_t, 128> data{};
    bool valid = false;
    void clear();
    ~UhfMemory() { clear(); }
};
struct UhfPassword {
    std::array<uint8_t, 4> bytes{};
    ~UhfPassword() { secure_zero(bytes.data(), bytes.size()); }
};
struct UhfResult {
    Status status = OK;
    Status cause = OK;
    uint8_t module_error = 0;
    bool mutation_attempted = false;
    bool verified = false;
    explicit operator bool() const { return status == OK; }
};
struct UhfPower {
    uint16_t centidbm = 0;
    uint32_t generation = 0;
    bool valid = false;
};

class Uhf {
public:
    explicit Uhf(Port& port) : port_(port) {}
    ~Uhf() { clear(); }
    Uhf(const Uhf&) = delete;
    Uhf& operator=(const Uhf&) = delete;
    void clear();
    const std::array<UhfTag, 64>& tags() const { return tags_; }
    size_t tag_count() const { return count_; }
    uint32_t dropped_reports() const { return dropped_; }
    bool scanning() const { return scanning_; }
    void cancel_inventory() { scanning_ = false; stream_size_ = 0; }
    const UhfMemory& memory() const { return memory_; }
    const UhfPower& power() const { return power_; }
    UhfResult inventory_start();
    UhfResult inventory_poll();
    UhfResult inventory_stop();
    // Raw local UART notification stream; supports fragments and concatenated frames.
    UhfResult feed(const uint8_t* bytes, size_t size);
    UhfResult read(const UhfTag&, UhfBank, uint16_t word, uint16_t words,
                   const UhfPassword&, UhfMemory& output);
    UhfResult write_user(const UhfTag&, const UhfMemory& expected,
                         const uint8_t* desired, size_t size, const UhfPassword&,
                         UhfMemory& verified);
    // EPC word 1 snapshot covers max(existing PC+EPC, desired PC+EPC), including expansion bytes.
    UhfResult rename_epc(const UhfTag&, const UhfMemory& expected,
                         const uint8_t* epc, size_t size, const UhfPassword&,
                         UhfTag& verified);
    UhfResult read_power();
    UhfResult set_power(const UhfPower& expected, unsigned dbm);
private:
    Port& port_;
    std::array<UhfTag, 64> tags_{};
    size_t count_ = 0;
    uint32_t dropped_ = 0, generation_ = 0;
    bool scanning_ = false;
    UhfMemory memory_{};
    UhfPower power_{};
    std::array<uint8_t, 1031> stream_{};
    size_t stream_size_ = 0;
    UhfResult check(uint32_t generation) const;
    UhfResult command(uint8_t command, const Bytes& payload, Bytes& response, uint32_t generation);
    UhfResult select(const UhfTag&);
    UhfResult report(const uint8_t*, size_t);
    UhfResult read_selected(const UhfTag&, UhfBank, uint16_t, uint16_t,
                            const UhfPassword&, UhfMemory&);
    UhfResult mutate(const UhfTag&, const UhfMemory&, const Bytes&,
                     const UhfPassword&, const UhfTag&, UhfMemory&);
    bool known(const UhfTag&) const;
};
}
