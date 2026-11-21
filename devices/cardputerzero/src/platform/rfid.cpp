#include "rfid.hpp"
#include <chrono>
#include <thread>

namespace qb::zero::platform {
namespace {
// Register/Type-A references: firmware/cores3/main/hardware.cpp and
// M5Stack M5Unit-RFID UnitMFRC522 / UnitWS1850S (MIT, M5Stack Technology).
// This driver uses software CRC_A, matching their NFC-A framing configuration.
uint16_t crc_a(const uint8_t* bytes, size_t size) {
    uint16_t crc = 0x6363;
    while (size--) {
        uint8_t b = *bytes++ ^ uint8_t(crc);
        b ^= uint8_t(b << 4);
        crc = uint16_t((crc >> 8) ^ (uint16_t(b) << 8) ^ (uint16_t(b) << 3) ^ (b >> 4));
    }
    return crc;
}
class RegisterRadio final : public Radio {
    Wire& wire_;
    Stop stop_;
    uint8_t version_ = 0;
    bool active_ = false;
    bool cleanup_ = false;
    Deadline cleanup_deadline_ = Deadline::max();
    void access_check(bool before = true) {
        if (!cleanup_) { check(); return; }
        // Reserve the configured Linux I2C_TIMEOUT (100ms) before starting an ioctl.
        // A kernel adapter must honor that timeout; userspace cannot interrupt ioctl.
        if (Clock::now() + std::chrono::milliseconds(before ? 100 : 0) >= cleanup_deadline_) throw TIMEOUT;
    }
    uint8_t get(uint8_t reg) {
        access_check();
        uint8_t value = 0;
        auto status = wire_.read_register(reg, value);
        if (status != OK) throw status;
        access_check(false);
        return value;
    }
    void set(uint8_t reg, uint8_t value) {
        access_check();
        auto status = wire_.write_register(reg, value);
        if (status != OK) throw status;
        access_check(false);
    }
    void check() { if (stop_()) throw CANCELLED; }
    template<class F> Status guarded(F fn) {
        try { check(); fn(); check(); return OK; }
        catch (Status status) { return status; }
        catch (...) { return IO_ERROR; }
    }
    void append_crc(Bytes& bytes) {
        auto crc = crc_a(bytes.data(), bytes.size());
        bytes.push_back(uint8_t(crc)); bytes.push_back(uint8_t(crc >> 8));
    }
    Bytes communicate(Bytes tx, unsigned bits = 0, bool crc = true, bool ack = false,
                      uint8_t command = 0x0c) {
        check();
        if (crc) append_crc(tx);
        if (tx.empty() || tx.size() > 64) throw INVALID_ARGUMENT;
        set(0x01, 0); set(0x04, 0x7f); set(0x0a, 0x80);
        for (auto byte : tx) { check(); set(0x09, byte); }
        set(0x0d, uint8_t(bits)); set(0x01, command);
        if (command == 0x0c) set(0x0d, uint8_t(0x80 | bits));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        for (;;) {
            check();
            auto irq = get(0x04);
            if (irq & (command == 0x0e ? 0x10 : 0x30)) break;
            if (irq & 1) throw command == 0x0e ? AUTH_FAILED : NO_CARD;
            if (std::chrono::steady_clock::now() >= deadline) throw TIMEOUT;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (get(0x06) & 0x1f) throw command == 0x0e ? AUTH_FAILED : IO_ERROR;
        if (command == 0x0e) {
            if (!(get(0x08) & 8)) throw AUTH_FAILED;
            return {};
        }
        unsigned size = get(0x0a);
        if (size > 64) throw IO_ERROR;
        unsigned tail = get(0x0c) & 7;
        Bytes rx;
        while (size--) rx.push_back(get(0x09));
        if (ack) {
            if (rx.size() != 1 || tail != 4 || (rx[0] & 15) != 0x0a) throw IO_ERROR;
        } else if (tail) throw IO_ERROR;
        else if (crc) {
            if (rx.size() < 3) throw IO_ERROR;
            auto expected = crc_a(rx.data(), rx.size() - 2);
            if (rx[rx.size() - 2] != uint8_t(expected) || rx.back() != uint8_t(expected >> 8))
                throw IO_ERROR;
            rx.resize(rx.size() - 2);
        }
        return rx;
    }
    void configure() {
        check();
        if (get(0x37) != version_) throw STALE_GENERATION;
        active_ = true;
        set(0x01, 0); set(0x08, uint8_t(get(0x08) & ~8));
        set(0x14, uint8_t(get(0x14) & ~3));
        set(0x12, 0); set(0x13, 0); set(0x11, 0x3d); set(0x15, 0x40);
        set(0x2a, 0x80); set(0x2b, 0xa9); set(0x2c, 3); set(0x2d, 0xe8);
        if (version_ == 0x15) { set(0x28, 0x20); set(0x29, 0x20); set(0x1e, 0); }
        std::this_thread::sleep_for(std::chrono::milliseconds(8)); check();
        set(0x14, uint8_t(get(0x14) | 3));
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); check();
    }
public:
    RegisterRadio(Wire& wire, Stop stop) : wire_(wire), stop_(std::move(stop)) {}
    Status initialize() override {
        return guarded([&] {
            version_ = get(0x37);
            if (version_ != 0x15 && version_ != 0x91 && version_ != 0x92) throw UNSUPPORTED;
        });
    }
    Status identify(local::HfIdentity& output) override {
        output = {};
        return guarded([&] {
            configure();
            local::HfIdentity card;
            auto atqa = communicate({0x52}, 7, false);
            if (atqa.size() != 2) throw IO_ERROR;
            std::copy(atqa.begin(), atqa.end(), card.atqa.begin());
            bool completed = false;
            for (unsigned level = 0; level < 3; ++level) {
                uint8_t cascade = uint8_t(0x93 + 2 * level);
                auto part = communicate({cascade, 0x20}, 0, false);
                if (part.size() != 5 || (part[0] ^ part[1] ^ part[2] ^ part[3]) != part[4])
                    throw IO_ERROR;
                Bytes select{cascade, 0x70}; select.insert(select.end(), part.begin(), part.end());
                auto sak = communicate(select);
                if (sak.size() != 1) throw IO_ERROR;
                bool more = sak[0] & 4;
                bool tag = part[0] == 0x88;
                if (more != tag || (more && level == 2)) throw UNSUPPORTED;
                unsigned begin = tag ? 1 : 0;
                for (unsigned i = begin; i < 4; ++i) card.uid[card.uid_size++] = part[i];
                card.sak = sak[0];
                if (!more) { completed = true; break; }
            }
            if (!completed) throw IO_ERROR;
            auto geometry = classic_geometry(card.sak, card.atqa.data(), card.uid_size);
            if (geometry) card.family = geometry == 64 ? local::HfFamily::Classic1K : local::HfFamily::Classic4K;
            else if (card.sak == 0 && card.uid_size == 7 && card.atqa == std::array<uint8_t, 2>{{0x44, 0}}) {
                auto version = communicate({0x60});
                if (version.size() != 8) throw UNSUPPORTED;
                std::copy(version.begin(), version.end(), card.version.begin());
                auto storage = version[6];
                if (version != Bytes{0, 4, 4, 2, 1, 0, storage, 3}) throw UNSUPPORTED;
                switch (storage) {
                case 0x0f: card.family = local::HfFamily::Ntag213; break;
                case 0x11: card.family = local::HfFamily::Ntag215; break;
                case 0x13: card.family = local::HfFamily::Ntag216; break;
                default: throw UNSUPPORTED;
                }
            } else throw UNSUPPORTED;
            output = card;
        });
    }
    Status authenticate(const local::HfIdentity& card, unsigned address, const local::ClassicKey& key) override {
        return guarded([&] {
            if (!card.classic() || card.uid_size < 4 || address >= card.units() ||
                (key.type != 0x60 && key.type != 0x61)) throw INVALID_ARGUMENT;
            Bytes tx{key.type, uint8_t(address)};
            tx.insert(tx.end(), key.bytes.begin(), key.bytes.end());
            tx.insert(tx.end(), card.uid.begin() + card.uid_size - 4, card.uid.begin() + card.uid_size);
            communicate(tx, 0, false, false, 0x0e);
        });
    }
    Status authenticate(const local::NtagAuth& auth, bool& authenticated) override {
        authenticated = false;
        return guarded([&] {
            if (auth.mode > 2) throw INVALID_ARGUMENT;
            if (!auth.mode) return;
            Bytes tx{0x1b}; tx.insert(tx.end(), auth.password.begin(), auth.password.end());
            auto pack = communicate(tx);
            if (pack.size() != 2 || (auth.mode == 2 && !std::equal(pack.begin(), pack.end(), auth.pack.begin())))
                throw AUTH_FAILED;
            authenticated = true;
        });
    }
    Status read(unsigned address, Bytes& out) override {
        out.clear();
        return guarded([&] {
            if (address > 255) throw INVALID_ARGUMENT;
            auto data = communicate({0x30, uint8_t(address)});
            if (data.size() != 16) throw IO_ERROR;
            out = data;
        });
    }
    Status write(unsigned address, const Bytes& data) override {
        return guarded([&] {
            if (address > 255 || (data.size() != 4 && data.size() != 16)) throw INVALID_ARGUMENT;
            if (data.size() == 16) {
                communicate({0xa0, uint8_t(address)}, 0, true, true);
                check(); communicate(data, 0, true, true);
            } else {
                Bytes tx{0xa2, uint8_t(address)}; tx.insert(tx.end(), data.begin(), data.end());
                communicate(tx, 0, true, true);
            }
        });
    }
    Status release(Deadline deadline) override {
        if (!active_) return OK;
        // Cleanup ignores cancellation only inside the shared bounded deadline.
        cleanup_ = true; cleanup_deadline_ = deadline;
        struct Reset { bool& value; ~Reset() { value = false; } } reset{cleanup_};
        try {
            set(0x01, 0); set(0x08, uint8_t(get(0x08) & ~8));
            set(0x14, uint8_t(get(0x14) & ~3));
            if ((get(0x08) & 8) || (get(0x14) & 3)) return IO_ERROR;
            active_ = false; return OK;
        } catch (Status status) { return status; }
        catch (...) { return IO_ERROR; }
    }
};
}
std::unique_ptr<Radio> make_register_radio(Wire& wire, Stop stop) {
    return std::make_unique<RegisterRadio>(wire, std::move(stop));
}
}
