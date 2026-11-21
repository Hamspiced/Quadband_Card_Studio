#pragma once
#include "../bridge.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace qb::local {
inline void secure_zero(void* value, size_t size) {
    auto* p = static_cast<volatile uint8_t*>(value);
    while (size--) *p++ = 0;
}

enum class HfFamily : uint8_t { Unknown, Classic1K, Classic4K, Ntag213, Ntag215, Ntag216 };
struct HfIdentity {
    HfFamily family = HfFamily::Unknown;
    std::array<uint8_t, 10> uid{};
    uint8_t uid_size = 0;
    std::array<uint8_t, 2> atqa{};
    uint8_t sak = 0;
    std::array<uint8_t, 8> version{};
    unsigned units() const {
        switch (family) {
        case HfFamily::Classic1K: return 64;
        case HfFamily::Classic4K: return 256;
        case HfFamily::Ntag213: return 45;
        case HfFamily::Ntag215: return 135;
        case HfFamily::Ntag216: return 231;
        default: return 0;
        }
    }
    bool classic() const { return family == HfFamily::Classic1K || family == HfFamily::Classic4K; }
    bool ntag() const { return family == HfFamily::Ntag213 || family == HfFamily::Ntag215 || family == HfFamily::Ntag216; }
    unsigned unit_size() const { return ntag() ? 4 : 16; }
    bool operator==(const HfIdentity& other) const {
        return family == other.family && uid_size == other.uid_size && uid == other.uid &&
            atqa == other.atqa && sak == other.sak && version == other.version;
    }
    bool operator!=(const HfIdentity& other) const { return !(*this == other); }
};

struct ClassicKey {
    std::array<uint8_t, 6> bytes{{255, 255, 255, 255, 255, 255}};
    uint8_t type = 0x60;
    ~ClassicKey() { secure_zero(bytes.data(), bytes.size()); }
};
struct NtagAuth {
    uint8_t mode = 0;
    std::array<uint8_t, 4> password{};
    std::array<uint8_t, 2> pack{};
    ~NtagAuth() {
        secure_zero(password.data(), password.size());
        secure_zero(pack.data(), pack.size());
    }
};

class Port {
public:
    virtual ~Port() = default;
    virtual bool cancelled() const = 0;
    virtual uint32_t generation(uint8_t channel) const = 0;
    virtual bool supports(uint8_t channel, uint16_t command) const = 0;
    virtual void progress(unsigned done, unsigned total, const char* detail) = 0;
    virtual void wait_ms(unsigned milliseconds) = 0;
    virtual Status identify(HfIdentity&) = 0;
    virtual Status read_classic(const HfIdentity&, unsigned block, const ClassicKey&, std::array<uint8_t, 16>&) = 0;
    virtual Status write_classic(const HfIdentity&, unsigned block, const ClassicKey&,
                                 const std::array<uint8_t, 16>& previous,
                                 const std::array<uint8_t, 16>& desired,
                                 std::array<uint8_t, 16>& verified) = 0;
    virtual Status read_pages(const HfIdentity&, unsigned first, unsigned count, const NtagAuth&, Bytes&) = 0;
    virtual Status write_page(const HfIdentity&, unsigned page, const NtagAuth&,
                              const std::array<uint8_t, 4>& previous,
                              const std::array<uint8_t, 4>& desired,
                              std::array<uint8_t, 4>& verified) = 0;
    virtual Status reset_hf() = 0;
    virtual Status format_ndef(const HfIdentity&, uint8_t, const Bytes&) { return UNSUPPORTED; }
    virtual Status transact(uint8_t channel, const Bytes& command, Bytes& response) = 0;
};
}
