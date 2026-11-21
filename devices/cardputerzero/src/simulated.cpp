#include "contract.hpp"
#include "standalone/lf.hpp"
#include "standalone/format.hpp"
#include <atomic>
#include <thread>

namespace qb::zero {
namespace {
Bytes m100(uint8_t command, const Bytes& data, uint8_t kind = 1) {
    Bytes out{0xbb, kind, command, uint8_t(data.size() >> 8), uint8_t(data.size())};
    out.insert(out.end(), data.begin(), data.end());
    uint8_t sum = 0; for (size_t i = 1; i < out.size(); ++i) sum += out[i];
    out.push_back(sum); out.push_back(0x7e); return out;
}
uint16_t crc(const Bytes& data) {
    uint16_t value = 0xffff;
    for (auto byte : data) {
        value ^= uint16_t(byte) << 8;
        for (unsigned i = 0; i < 8; ++i) value = uint16_t((unsigned(value) << 1) ^ (value & 0x8000 ? 0x1021 : 0));
    }
    return value ^ 0xffff;
}
class Simulated final : public ReaderPort {
    Progress progress_;
    std::atomic<bool> cancelled_{false};
    bool connected_ = false;
    std::array<std::array<uint8_t, 4>, 45> pages_{};
    Bytes epc_{0x11, 0x22, 0x33, 0x44}, filter_;
    std::array<uint8_t, 2> pc_{{0x10, 0}};
    std::array<uint8_t, 512> user_{};
    unsigned power_ = 2000;
    local::HfIdentity identity() const {
        local::HfIdentity card; card.family = local::HfFamily::Ntag213;
        card.uid_size = 7; card.uid = {4, 1, 2, 3, 4, 5, 6};
        card.atqa = {{0x44,0}}; card.version = {{0,4,4,2,1,0,15,3}};
        return card;
    }
    Bytes uhf_identity() const {
        Bytes out{uint8_t(epc_.size() + 2), pc_[0], pc_[1]};
        out.insert(out.end(), epc_.begin(), epc_.end()); return out;
    }
public:
    explicit Simulated(Progress callback) : progress_(std::move(callback)) {
        pages_[3] = {0xe1, 0x10, 18, 0}; pages_[41][3] = 255;
    }
    Status connect() override { connected_ = true; resume(); return OK; }
    Status release() override { return OK; }
    void request_cancel() override { cancelled_ = true; }
    void resume() override { cancelled_ = false; }
    bool cancelled() const override { return cancelled_; }
    std::string detail() const override { return "SIMULATION ONLY: synthetic HF/UHF, LF read-only; no hardware access."; }
    uint32_t generation(uint8_t channel) const override { return connected_ && channel >= 1 && channel <= 3 ? 1 : 0; }
    bool supports(uint8_t channel, uint16_t command) const override {
        if (!generation(channel)) return false;
        if (channel == 1) return (command >= 16 && command <= 21) || command == 23;
        return channel != 3 || command == 1001 || command == 3000;
    }
    void progress(unsigned done, unsigned total, const char* text) override { if (progress_) progress_(done, total, text); }
    void wait_ms(unsigned milliseconds) override { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
    Status identify(local::HfIdentity& card) override {
        if (cancelled()) return CANCELLED;
        card = identity(); return OK;
    }
    Status read_classic(const local::HfIdentity&, unsigned, const local::ClassicKey&, std::array<uint8_t, 16>&) override { return UNSUPPORTED; }
    Status write_classic(const local::HfIdentity&, unsigned, const local::ClassicKey&, const std::array<uint8_t, 16>&,
                         const std::array<uint8_t, 16>&, std::array<uint8_t, 16>&) override { return UNSUPPORTED; }
    Status read_pages(const local::HfIdentity& card, unsigned first, unsigned count, const local::NtagAuth&, Bytes& out) override {
        if (cancelled()) return CANCELLED;
        if (card != identity() || !count || count > 16 || first + count > pages_.size()) return INVALID_ARGUMENT;
        out.clear();
        for (unsigned i = first; i < first + count; ++i) out.insert(out.end(), pages_[i].begin(), pages_[i].end());
        return OK;
    }
    Status write_page(const local::HfIdentity& card, unsigned page, const local::NtagAuth&, const std::array<uint8_t, 4>& previous,
                      const std::array<uint8_t, 4>& desired, std::array<uint8_t, 4>& verified) override {
        if (cancelled()) return CANCELLED;
        if (card != identity() || page < 4 || page > 39) return PERMISSION_DENIED;
        if (pages_[page] != previous) return STALE_DATA;
        pages_[page] = verified = desired; return OK;
    }
    Status reset_hf() override { return OK; }
    Status format_ndef(const local::HfIdentity& card, uint8_t flags, const Bytes& expected) override {
        local::FormatIO io;
        io.guard = [&]() { return cancelled() ? CANCELLED : card != identity() ? STALE_DATA : OK; };
        io.read = [&](unsigned page, const local::ClassicKey&, Bytes& out) {
            out.assign(pages_[page].begin(),pages_[page].end()); return OK;
        };
        io.write = [&](unsigned page, const local::ClassicKey&, const Bytes& data) {
            std::copy(data.begin(),data.end(),pages_[page].begin()); return OK;
        };
        return local::canonical_format(card,flags,expected,io);
    }
    Status transact(uint8_t channel, const Bytes& request, Bytes& response) override {
        if (cancelled()) return CANCELLED;
        if (channel == 3) {
            if (validate_chameleon(request) != OK) return INVALID_ARGUMENT;
            const unsigned command = unsigned(request[2]) << 8 | request[3];
            Bytes data;
            const unsigned status = command == 1001 ? 0x68 : command == 3000 ? 0x40 : 1;
            if (command == 3000) data = {1, 2, 3, 4, 5};
            response = {0x11, 0xef, request[2], request[3], 0, uint8_t(status), 0, uint8_t(data.size()), 0};
            uint8_t sum = 0; for (auto byte : response) sum += byte; response[8] = uint8_t(-sum);
            response.insert(response.end(), data.begin(), data.end());
            sum = 0; for (auto byte : data) sum += byte; response.push_back(uint8_t(-sum)); return OK;
        }
        if (channel != 2 || validate_uhf(request) != OK) return INVALID_ARGUMENT;
        const auto command = request[2]; Bytes payload(request.begin() + 5, request.end() - 2), data{0};
        switch (command) {
        case 0x12: case 0x28: break;
        case 0x0c: filter_ = payload; break;
        case 0x0b: data = filter_; break;
        case 0x22: {
            data.assign(pc_.begin(), pc_.end()); data.insert(data.end(), epc_.begin(), epc_.end());
            const auto checksum = crc(data); data.insert(data.begin(), 0xc9);
            data.push_back(uint8_t(checksum >> 8)); data.push_back(uint8_t(checksum));
            response = m100(command, data, 2); return OK;
        }
        case 0xb7: data = {uint8_t(power_ >> 8), uint8_t(power_)}; break;
        case 0xb6: power_ = unsigned(payload[0]) * 256 + payload[1]; break;
        case 0x39: {
            const unsigned word = unsigned(payload[5]) * 256 + payload[6], count = unsigned(payload[7]) * 256 + payload[8];
            Bytes memory(user_.begin(), user_.end());
            if (payload[4] == 1) {
                memory = {0, 0, pc_[0], pc_[1]}; memory.insert(memory.end(), epc_.begin(), epc_.end()); memory.resize(512);
            }
            if ((word + count) * 2 > memory.size()) return INVALID_ARGUMENT;
            data = uhf_identity(); data.insert(data.end(), memory.begin() + word * 2, memory.begin() + (word + count) * 2); break;
        }
        case 0x49: {
            data = uhf_identity(); data.push_back(0);
            if (payload[4] == 1) { pc_ = {payload[9], payload[10]}; epc_.assign(payload.begin() + 11, payload.end()); }
            else {
                const unsigned word = unsigned(payload[5]) * 256 + payload[6];
                if (word * 2 + payload.size() - 9 > user_.size()) return INVALID_ARGUMENT;
                std::copy(payload.begin() + 9, payload.end(), user_.begin() + word * 2);
            }
            break;
        }
        default: return UNSUPPORTED;
        }
        response = m100(command, data); return OK;
    }
};
}
std::unique_ptr<ReaderPort> make_simulated_port(Progress callback) { return std::make_unique<Simulated>(std::move(callback)); }
}
