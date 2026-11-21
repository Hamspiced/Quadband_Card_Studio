#pragma once
#include "port.hpp"
#include <functional>

namespace qb::local {
class DevicePort final : public Port {
    uint64_t epoch_;
    std::array<uint32_t, 3> generations_;
    uint8_t used_ = 0;
    std::function<void(unsigned, unsigned, const char*)> progress_;
    Status call(uint8_t channel, uint8_t operation, const Bytes&, Bytes&);
public:
    Status format_ndef(const HfIdentity&, uint8_t, const Bytes&) override;
    DevicePort(uint64_t epoch, const std::array<uint32_t, 3>& generations,
               std::function<void(unsigned, unsigned, const char*)> progress)
        : epoch_(epoch), generations_(generations), progress_(std::move(progress)) {}
    void rebind(uint64_t epoch, const std::array<uint32_t, 3>& generations) {
        epoch_ = epoch; generations_ = generations; used_ = 0;
    }
    uint8_t used_channels() const { return used_; }
    bool cancelled() const override;
    uint32_t generation(uint8_t channel) const override;
    bool supports(uint8_t channel, uint16_t command) const override;
    void progress(unsigned done, unsigned total, const char* detail) override;
    void wait_ms(unsigned milliseconds) override;
    Status identify(HfIdentity&) override;
    Status read_classic(const HfIdentity&, unsigned, const ClassicKey&, std::array<uint8_t, 16>&) override;
    Status write_classic(const HfIdentity&, unsigned, const ClassicKey&,
                         const std::array<uint8_t, 16>& previous,
                         const std::array<uint8_t, 16>& desired,
                         std::array<uint8_t, 16>& verified) override;
    Status read_pages(const HfIdentity&, unsigned, unsigned, const NtagAuth&, Bytes&) override;
    Status write_page(const HfIdentity&, unsigned, const NtagAuth&,
                      const std::array<uint8_t, 4>& previous,
                      const std::array<uint8_t, 4>& desired,
                      std::array<uint8_t, 4>& verified) override;
    Status reset_hf() override;
    Status transact(uint8_t channel, const Bytes&, Bytes&) override;
};
}
