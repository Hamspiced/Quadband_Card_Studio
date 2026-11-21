#include "platform/rfid.hpp"
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>
using namespace qb;
using namespace qb::local;
using namespace qb::zero;
using namespace qb::zero::platform;
namespace {
Bytes crc(Bytes data) {
    uint16_t value = 0x6363;
    for (auto byte : data) {
        uint8_t x = byte ^ uint8_t(value); x ^= uint8_t(x << 4);
        value = uint16_t((value >> 8) ^ (uint16_t(x) << 8) ^ (uint16_t(x) << 3) ^ (x >> 4));
    }
    data.push_back(uint8_t(value)); data.push_back(uint8_t(value >> 8)); return data;
}
struct RegisterWire : Wire {
    struct Step { Bytes tx, rx; unsigned tail = 0, error = 0; bool authenticate = false; };
    std::deque<Step> script;
    std::array<uint8_t, 64> registers{};
    Bytes tx, rx;
    unsigned rx_at = 0, writes = 0, transactions = 0;
    bool silent = false, stuck_rf = false;
    unsigned access_delay_ms = 0;
    std::function<void(uint8_t, bool)> after_access;
    RegisterWire(uint8_t version = 0x92) { registers[0x37] = version; }
    Status open(const ReaderConfig&) override { return OK; }
    void close() noexcept override {}
    void execute(bool authentication) {
        ++transactions;
        if (silent) { registers[4] = 0; return; }
        assert(!script.empty()); auto step = script.front(); script.pop_front();
        assert(step.authenticate == authentication); assert(step.tx == tx);
        rx = step.rx; rx_at = 0; registers[0x0a] = uint8_t(rx.size()); registers[0x0c] = uint8_t(step.tail);
        registers[6] = uint8_t(step.error); registers[4] = authentication ? 0x10 : 0x30;
        if (authentication) registers[8] |= 8;
    }
    Status read_register(uint8_t reg, uint8_t& value) override {
        assert(reg < 64);
        if (access_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(access_delay_ms));
        if (reg == 9) { assert(rx_at < rx.size()); value = rx[rx_at++]; }
        else value = registers[reg];
        if (after_access) after_access(reg, false);
        return OK;
    }
    Status write_register(uint8_t reg, uint8_t value) override {
        assert(reg < 64); ++writes;
        if (access_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(access_delay_ms));
        // A reader-only implementation must never issue software reset / power-down.
        assert(reg != 1 || value == 0 || value == 0x0c || value == 0x0e);
        if (reg == 9) tx.push_back(value);
        else if (reg == 0x0a && value == 0x80) { tx.clear(); rx.clear(); rx_at = 0; registers[reg] = 0; }
        else if (reg == 4 && value == 0x7f) registers[reg] = 0;
        else {
            if (!(stuck_rf && reg == 0x14)) registers[reg] = value;
            if (reg == 1 && value == 0x0e) execute(true);
            if (reg == 0x0d && (value & 0x80)) execute(false);
        }
        if (after_access) after_access(reg, true);
        return OK;
    }
    Status exchange(uint8_t, const Bytes&, Bytes&, unsigned, const Stop&) override { assert(false); return IO_ERROR; }
    void classic_select(bool bad_bcc = false) {
        script.push_back({{0x52}, {4, 0}});
        script.push_back({{0x93, 0x20}, {1, 2, 3, 4, uint8_t(bad_bcc ? 5 : 4)}});
        if (!bad_bcc) script.push_back({crc({0x93, 0x70, 1, 2, 3, 4, 4}), crc({8})});
    }
    void ntag_select() {
        script.push_back({{0x52}, {0x44, 0}});
        script.push_back({{0x93, 0x20}, {0x88, 1, 2, 3, 0x88}});
        script.push_back({crc({0x93, 0x70, 0x88, 1, 2, 3, 0x88}), crc({4})});
        script.push_back({{0x95, 0x20}, {4, 5, 6, 7, 0}});
        script.push_back({crc({0x95, 0x70, 4, 5, 6, 7, 0}), crc({0})});
        script.push_back({crc({0x60}), crc({0, 4, 4, 2, 1, 0, 0x0f, 3})});
    }
};
void identify_and_classic() {
    // CRC_A golden for ISO14443 HALT, not just a roundtrip through the driver.
    assert(crc({0x50, 0}) == Bytes({0x50, 0, 0x57, 0xcd}));
    for (unsigned version : {0x91u, 0x92u, 0x15u}) {
        RegisterWire wire{uint8_t(version)}; bool stop = false;
        auto radio = make_register_radio(wire, [&] { return stop; });
        assert(radio->initialize() == OK && wire.writes == 0 && wire.transactions == 0);
        wire.classic_select();
        HfIdentity card; assert(radio->identify(card) == OK);
        assert(card.family == HfFamily::Classic1K && card.uid_size == 4 && card.uid[3] == 4);
        ClassicKey key;
        wire.script.push_back({{0x60, 4, 255, 255, 255, 255, 255, 255, 1, 2, 3, 4}, {}, 0, 0, true});
        assert(radio->authenticate(card, 4, key) == OK);
        Bytes data(16, 0xab), out;
        wire.script.push_back({crc({0x30, 4}), crc(data)});
        assert(radio->read(4, out) == OK && out == data);
        wire.script.push_back({crc({0xa0, 4}), {0x0a}, 4});
        wire.script.push_back({crc(data), {0x0a}, 4});
        assert(radio->write(4, data) == OK);
        stop = true; assert(radio->release() == OK);
        assert(!(wire.registers[8] & 8) && !(wire.registers[0x14] & 3));
        assert(wire.script.empty());
    }
}
void ntag_frames_and_errors() {
    RegisterWire wire(0x15); auto radio = make_register_radio(wire, [] { return false; });
    assert(radio->initialize() == OK); wire.ntag_select();
    HfIdentity card; assert(radio->identify(card) == OK);
    assert(card.family == HfFamily::Ntag213 && card.uid_size == 7 && card.uid[6] == 7);
    NtagAuth auth; auth.mode = 2; auth.password = {1, 2, 3, 4}; auth.pack = {0xab, 0xcd};
    wire.script.push_back({crc({0x1b, 1, 2, 3, 4}), crc({0xab, 0xcd})});
    bool authenticated = false; assert(radio->authenticate(auth, authenticated) == OK && authenticated);
    wire.script.push_back({crc({0xa2, 4, 1, 2, 3, 4}), {0x0a}, 4});
    assert(radio->write(4, {1, 2, 3, 4}) == OK);
    wire.script.push_back({crc({0x1b, 1, 2, 3, 4}), crc({0, 0})});
    assert(radio->authenticate(auth, authenticated) == AUTH_FAILED && !authenticated);
    Bytes invalid_crc = crc(Bytes(16, 0)); invalid_crc.back() ^= 1;
    wire.script.push_back({crc({0x30, 4}), invalid_crc});
    Bytes data; assert(radio->read(4, data) == IO_ERROR && data.empty());
    wire.script.push_back({crc({0xa2, 4, 1, 2, 3, 4}), {0}, 4});
    assert(radio->write(4, {1, 2, 3, 4}) == IO_ERROR); assert(wire.script.empty());
    wire.stuck_rf = true; wire.registers[0x14] = 3;
    assert(radio->release() == IO_ERROR);
}
void fail_closed() {
    RegisterWire unknown(0x88); auto radio = make_register_radio(unknown, [] { return false; });
    assert(radio->initialize() == UNSUPPORTED && unknown.writes == 0);
    RegisterWire wire; radio = make_register_radio(wire, [] { return false; });
    assert(radio->initialize() == OK); wire.classic_select(true);
    HfIdentity card; assert(radio->identify(card) == IO_ERROR && card.family == HfFamily::Unknown);
    assert(radio->release() == OK);
    wire.silent = true;
    auto start = std::chrono::steady_clock::now();
    assert(radio->identify(card) == TIMEOUT);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    assert(radio->release() == OK);
    bool stop = true;
    radio = make_register_radio(wire, [&] { return stop; });
    assert(radio->initialize() == CANCELLED);
}
void fifo_cancellation_and_cleanup_deadline() {
    RegisterWire wire; bool stop = false;
    auto radio = make_register_radio(wire, [&] { return stop; });
    assert(radio->initialize() == OK); wire.classic_select();
    HfIdentity card; assert(radio->identify(card) == OK);
    wire.script.push_back({crc({0x30, 4}), crc(Bytes(16, 0xa5))});
    wire.after_access = [&](uint8_t reg, bool writing) { if (reg == 9 && !writing) stop = true; };
    Bytes data; assert(radio->read(4, data) == CANCELLED);
    assert(wire.rx_at == 1 && data.empty());
    wire.after_access = {}; assert(radio->release() == OK);
    stop = false; wire.classic_select(); assert(radio->identify(card) == OK);
    auto transactions = wire.transactions;
    wire.after_access = [&](uint8_t reg, bool writing) { if (reg == 9 && writing) stop = true; };
    assert(radio->write(4, Bytes(16, 0xa5)) == CANCELLED);
    assert(wire.tx.size() == 1 && wire.transactions == transactions);
    wire.after_access = {};
    // RF-off uses the caller's shared deadline, not a fresh deadline per register.
    auto writes = wire.writes;
    assert(radio->release(Clock::now() + std::chrono::milliseconds(50)) == TIMEOUT);
    assert(wire.writes == writes);
    wire.access_delay_ms = 50;
    auto start = Clock::now();
    assert(radio->release(start + std::chrono::milliseconds(300)) == TIMEOUT);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    assert(elapsed >= 150 && elapsed < 400);
    wire.access_delay_ms = 0;
    assert(radio->release() == OK);
    assert(!(wire.registers[0x14] & 3) && !(wire.registers[8] & 8));
    std::cout << "HF cleanup shared deadline stopped after " << elapsed << "ms; FIFO read/write cancellation observed after first access\n";
}
}
int main() {
    identify_and_classic(); ntag_frames_and_errors(); fail_closed(); fifo_cancellation_and_cleanup_deadline();
    std::cout << "platform_rfid_test: RFID/RFID2 register identity, Type-A selection, CRC_A, supplied auth, Classic/NTAG write frames, bad CRC/BCC/ACK, bounded wait and RF-off verification passed\n";
}
