#include "linux_port.hpp"
#include "standalone/format.hpp"
#include <chrono>
#include <set>
#include <thread>

namespace qb::zero::platform {
namespace {
bool uhf_allowed(unsigned cmd) {
    switch (cmd) {
    case 3: case 8: case 11: case 12: case 18: case 34: case 40:
    case 57: case 73: case 182: case 183: return true;
    default: return false;
    }
}
bool ultra_allowed(unsigned cmd) {
    switch (cmd) {
    case 1000: case 1001: case 1002: case 1017: case 1033: case 1035:
    case 3000: case 3002: case 3004: case 3010: case 3014: case 3016:
    case 3019: case 3021: return true;
    default: return false;
    }
}
class LinuxPort final : public ReaderPort {
    ReaderConfig config_;
    Progress progress_;
    std::unique_ptr<Wire> wire_;
    std::unique_ptr<Radio> radio_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> cancel_due_ns_{0};
    std::array<uint32_t, 3> generations_{{0, 0, 0}};
    std::array<bool, 3> attached_{{false, false, false}};
    std::set<unsigned> commands_;
    UhfGuard guard_;
    int original_mode_ = -1;
    bool mode_dirty_ = false, inventory_used_ = false, quarantined_ = false;
    bool hf_dirty_ = false, cleanup_failed_ = false;
    const char* detail_ = "Choose explicit reader endpoints; no card scan runs on connect.";
    void bump(unsigned channel) {
        if (++generations_[channel - 1] == 0) ++generations_[channel - 1];
    }
    void quarantine(const char* reason) noexcept {
        quarantined_ = true; detail_ = reason; commands_.clear(); guard_.clear();
        for (unsigned i = 0; i < 3; ++i) if (attached_[i]) { attached_[i] = false; bump(i + 1); }
    }
    Status live(unsigned channel) const {
        if (stop_.load(std::memory_order_acquire)) return CANCELLED;
        return channel >= 1 && channel <= 3 && attached_[channel - 1] && !quarantined_ ? OK : UNAVAILABLE;
    }
    static void require(Status status) { if (status != OK) throw status; }
    Deadline cleanup_deadline() const {
        auto deadline = Clock::now() + std::chrono::seconds(2);
        if (cancelled()) {
            auto due = cancel_due_ns_.load(std::memory_order_acquire);
            if (due) deadline = std::min(deadline, Deadline(std::chrono::duration_cast<Clock::duration>(
                std::chrono::nanoseconds(due))));
        }
        return deadline;
    }
    Status exchange(unsigned channel, const Bytes& request, Bytes& response, bool cleanup = false,
                    Deadline deadline = Deadline::max()) {
        unsigned timeout = channel == 3 ? usb_timeout_ms(unsigned(request[2]) << 8 | request[3]) : 2000;
        if (cleanup) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) return TIMEOUT;
            timeout = unsigned(std::min<int64_t>(timeout, remaining));
        }
        auto status = wire_->exchange(uint8_t(channel), request, response,
            timeout, [this, cleanup, deadline] { return cleanup ? Clock::now() >= deadline : cancelled(); });
        if (cleanup && Clock::now() >= deadline) return TIMEOUT;
        if (!cleanup && status == OK && cancelled()) return CANCELLED;
        return status;
    }
    template<class F> Status hf_call(bool& mutation, F fn) {
        auto status = live(1); if (status != OK) return status;
        hf_dirty_ = true;
        try { fn(); if (cancelled()) status = CANCELLED; }
        catch (Status failure) { status = failure; }
        catch (...) { status = IO_ERROR; }
        Status cleanup = IO_ERROR;
        try { cleanup = radio_->release(cleanup_deadline()); } catch (...) {}
        hf_dirty_ = cleanup != OK;
        if (cleanup != OK) {
            cleanup_failed_ = true;
            quarantine("HF cleanup unverified; reconnect before reader use.");
            if (status == OK) status = IO_ERROR;
        }
        if (status == IO_ERROR || status == TIMEOUT || status == STALE_GENERATION)
            quarantine("Reader attachment failed; reconnect before reader use.");
        return mutation && status != OK ? WRITE_UNVERIFIED : status;
    }
    void same(const local::HfIdentity& expected) {
        if (expected.uid_size != 4 && expected.uid_size != 7 && expected.uid_size != 10)
            throw INVALID_ARGUMENT;
        local::HfIdentity actual;
        require(radio_->identify(actual));
        if (actual != expected) throw STALE_DATA;
        if (cancelled()) throw CANCELLED;
    }
    Bytes read(unsigned address) {
        if (cancelled()) throw CANCELLED;
        Bytes data;
        require(radio_->read(address, data));
        if (data.size() != 16) throw IO_ERROR;
        if (cancelled()) throw CANCELLED;
        return data;
    }
    bool config_valid() {
        if (!config_.ultra_ble_token.empty() && (!config_.ultra_serial.empty() || !config_.ble_session)) return false;
        const std::string paths[]{config_.hf_i2c, config_.uhf_serial, config_.ultra_serial};
        std::set<std::string> unique;
        for (const auto& path : paths) if (!path.empty()) {
            if (path[0] != '/' || path.find('\0') != std::string::npos || !unique.insert(path).second) return false;
        }
        return !unique.empty() || !config_.ultra_ble_token.empty();
    }
    void negotiate_ultra() {
        Bytes response;
        require(exchange(3, ultra_frame(1000), response));
        if (!ultra_success(response) || response.size() != 12 || response[9] != 2) throw UNSUPPORTED;
        require(exchange(3, ultra_frame(1033), response));
        if (!ultra_success(response) || response.size() != 11 || response[9] != 0) throw UNSUPPORTED;
        require(exchange(3, ultra_frame(1035), response));
        if (!ultra_success(response) || response.size() <= 10 || (response.size() - 10) % 2) throw BAD_FRAME;
        for (size_t i = 9; i < response.size() - 1; i += 2) {
            unsigned cmd = unsigned(response[i]) << 8 | response[i + 1];
            if (!commands_.insert(cmd).second) throw BAD_FRAME;
        }
        if (!commands_.count(1001) || !commands_.count(1002)) throw UNSUPPORTED;
        require(exchange(3, ultra_frame(1002), response));
        if (!ultra_success(response) || response.size() != 11 || response[9] > 1) throw BAD_FRAME;
        original_mode_ = response[9];
    }
public:
    LinuxPort(ReaderConfig config, Progress progress, std::unique_ptr<Wire> wire, std::unique_ptr<Radio> radio)
        : config_(std::move(config)), progress_(std::move(progress)), wire_(std::move(wire)),
          radio_(std::move(radio)) {
        if (!radio_) radio_ = make_register_radio(*wire_, [this] { return cancelled(); });
    }
    ~LinuxPort() override {
        if (!cleanup_failed_ && (hf_dirty_ || inventory_used_ || mode_dirty_)) release();
        wire_->close();
    }
    bool cancelled() const override { return stop_.load(std::memory_order_acquire); }
    void request_cancel() override {
        int64_t expected = 0;
        auto due = std::chrono::duration_cast<std::chrono::nanoseconds>(
            (Clock::now() + std::chrono::seconds(2)).time_since_epoch()).count();
        cancel_due_ns_.compare_exchange_strong(expected, due, std::memory_order_acq_rel);
        stop_.store(true, std::memory_order_release);
    }
    void resume() override {
        cancel_due_ns_.store(0, std::memory_order_release);
        stop_.store(false, std::memory_order_release);
    }
    uint32_t generation(uint8_t channel) const override {
        return channel >= 1 && channel <= 3 && attached_[channel - 1] ? generations_[channel - 1] : 0;
    }
    bool supports(uint8_t channel, uint16_t cmd) const override {
        if (!generation(channel) || quarantined_) return false;
        if (channel == 1) return cmd >= 16 && cmd <= 23;
        if (channel == 2) return uhf_allowed(cmd);
        return ultra_allowed(cmd) && commands_.count(cmd);
    }
    void progress(unsigned done, unsigned total, const char* text) override {
        if (progress_) progress_(done, total, text);
    }
    void wait_ms(unsigned milliseconds) override {
        for (unsigned elapsed = 0; elapsed < milliseconds && !cancelled();) {
            auto step = std::min(10u, milliseconds - elapsed);
            std::this_thread::sleep_for(std::chrono::milliseconds(step)); elapsed += step;
        }
    }
    std::string detail() const override { return detail_; }
    Status connect() override {
        // Failed cleanup may not be papered over by re-negotiating the same open endpoint.
        auto prior = release();
        if (prior != OK) return prior;
        wire_->close();
        for (unsigned i = 0; i < 3; ++i) { attached_[i] = false; bump(i + 1); }
        guard_.clear(); commands_.clear(); original_mode_ = -1;
        inventory_used_ = mode_dirty_ = false;
        quarantined_ = false;
        if (cancelled()) return CANCELLED;
        if (!config_valid()) { detail_ = "Select distinct absolute reader endpoints (no automatic discovery)."; return INVALID_ARGUMENT; }
        if (!config_.hf_i2c.empty() && (!config_.grove_i2c_confirmed || config_.hf_address != 0x28)) {
            detail_ = "Confirm the chosen HF I2C connector at address 0x28. With HF on the Cap, keep the Zero's right Grove in UART mode for UHF.";
            return PERMISSION_DENIED;
        }
        Status status = OK;
        try {
            require(wire_->open(config_));
            if (!config_.hf_i2c.empty()) { require(radio_->initialize()); attached_[0] = true; }
            if (!config_.uhf_serial.empty()) {
                Bytes response; require(exchange(2, uhf_frame(3, {0}), response));
                if (response.size() < 9 || response[1] != 1 || response[2] != 3 || response[5] != 0)
                    throw UNSUPPORTED;
                std::string hardware(response.begin() + 6, response.end() - 2);
                for (auto& c : hardware) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
                if (hardware.find("M100") == std::string::npos && hardware.find("JRD") == std::string::npos &&
                    hardware.find("YRM") == std::string::npos) throw UNSUPPORTED;
                attached_[1] = true;
            }
            if (!config_.ultra_serial.empty() || !config_.ultra_ble_token.empty()) { negotiate_ultra(); attached_[2] = true; }
            if (cancelled()) throw CANCELLED;
            detail_ = "Reader modules identified; RF scan requires an explicit action. Grove switching is OS-managed.";
            return OK;
        } catch (Status failure) { status = failure; }
        catch (...) { status = IO_ERROR; }
        release(); wire_->close(); attached_.fill(false); commands_.clear();
        detail_ = "Reader identity/capability negotiation failed closed; check explicit endpoints and OS permissions.";
        return status;
    }
    Status release() override {
        const auto deadline = cleanup_deadline();
        const bool dirty = hf_dirty_ || inventory_used_ || mode_dirty_;
        Status status = OK;
        try {
            auto result = radio_->release(deadline);
            hf_dirty_ = result != OK;
            if (result != OK) status = result;
        }
        catch (...) { hf_dirty_ = true; status = IO_ERROR; }
        try {
            if (inventory_used_) {
                Bytes response;
                auto result = exchange(2, uhf_frame(0x28), response, true, deadline);
                if (result != OK || response.size() != 8 || response[1] != 1 ||
                    response[2] != 0x28 || response[5] != 0) status = IO_ERROR;
                else inventory_used_ = false;
            }
        } catch (...) { status = IO_ERROR; }
        try {
            if (mode_dirty_ && original_mode_ >= 0) {
                Bytes acknowledgement, response;
                auto result = exchange(3, ultra_frame(1001, {uint8_t(original_mode_)}), acknowledgement, true, deadline);
                if (result != OK || !ultra_success(acknowledgement) || acknowledgement.size() != 10 ||
                    exchange(3, ultra_frame(1002), response, true, deadline) != OK ||
                    !ultra_success(response) || response.size() != 11 || response[9] != original_mode_)
                    status = IO_ERROR;
                else mode_dirty_ = false;
            }
        } catch (...) { status = IO_ERROR; }
        guard_.clear();
        if (dirty && Clock::now() >= deadline) status = TIMEOUT;
        cleanup_failed_ = status != OK;
        if (status != OK) quarantine("RF/mode cleanup unverified; close and reconnect the reader before further use.");
        return status;
    }
    Status identify(local::HfIdentity& card) override {
        card = {}; bool mutation = false;
        return hf_call(mutation, [&] { require(radio_->identify(card)); });
    }
    Status read_classic(const local::HfIdentity& card, unsigned block, const local::ClassicKey& key,
                        std::array<uint8_t, 16>& out) override {
        out.fill(0);
        if (!card.classic() || block >= card.units() || (key.type != 0x60 && key.type != 0x61)) return INVALID_ARGUMENT;
        bool mutation = false;
        return hf_call(mutation, [&] {
            same(card); require(radio_->authenticate(card, block, key)); auto data = read(block);
            std::copy(data.begin(), data.end(), out.begin());
        });
    }
    Status write_classic(const local::HfIdentity& card, unsigned block, const local::ClassicKey& key,
                         const std::array<uint8_t, 16>& previous, const std::array<uint8_t, 16>& desired,
                         std::array<uint8_t, 16>& verified) override {
        verified.fill(0);
        if (!card.classic() || !classic_data(card.units(), block) || (key.type != 0x60 && key.type != 0x61))
            return PERMISSION_DENIED;
        bool mutation = false;
        return hf_call(mutation, [&] {
            same(card); require(radio_->authenticate(card, block, key));
            auto access = read(trailer(block));
            if (!classic_write_allowed(block, key.type, access.data())) throw PERMISSION_DENIED;
            auto actual = read(block);
            if (!std::equal(actual.begin(), actual.end(), previous.begin())) throw STALE_DATA;
            if (cancelled()) throw CANCELLED;
            mutation = true; require(radio_->write(block, Bytes(desired.begin(), desired.end())));
            actual = read(block);
            if (!std::equal(actual.begin(), actual.end(), desired.begin())) throw WRITE_UNVERIFIED;
            std::copy(actual.begin(), actual.end(), verified.begin());
        });
    }
    Status read_pages(const local::HfIdentity& card, unsigned first, unsigned count,
                      const local::NtagAuth& auth, Bytes& out) override {
        out.clear();
        if (!card.ntag() || !count || count > 16 || first >= card.units() || count > card.units() - first ||
            auth.mode > 2) return INVALID_ARGUMENT;
        bool mutation = false;
        return hf_call(mutation, [&] {
            same(card); bool authenticated = false; require(radio_->authenticate(auth, authenticated));
            Bytes result;
            for (unsigned i = 0; i < count; i += 4) {
                auto data = read(first + i);
                result.insert(result.end(), data.begin(), data.begin() + std::min(4u,count-i)*4);
            }
            same(card);
            out = result;
        });
    }
    Status write_page(const local::HfIdentity& card, unsigned page, const local::NtagAuth& auth,
                      const std::array<uint8_t, 4>& previous, const std::array<uint8_t, 4>& desired,
                      std::array<uint8_t, 4>& verified) override {
        verified.fill(0);
        if (!card.ntag() || page < 4 || page > ntag(card.units()).last_user || auth.mode > 2)
            return PERMISSION_DENIED;
        bool mutation = false;
        return hf_call(mutation, [&] {
            same(card); bool authenticated = false; require(radio_->authenticate(auth, authenticated));
            auto initial = read(0), dyn = read(ntag(card.units()).dynamic);
            auto config = read(ntag(card.units()).dynamic + 1);
            if (!ntag_write_allowed(card.units(), page, initial.data(), dyn.data(), config.data(), authenticated))
                throw PERMISSION_DENIED;
            auto actual = read(page);
            if (!std::equal(previous.begin(), previous.end(), actual.begin())) throw STALE_DATA;
            if (cancelled()) throw CANCELLED;
            mutation = true; require(radio_->write(page, Bytes(desired.begin(), desired.end())));
            actual = read(page);
            if (!std::equal(desired.begin(), desired.end(), actual.begin())) throw WRITE_UNVERIFIED;
            std::copy_n(actual.begin(), 4, verified.begin());
            same(card);
        });
    }
    Status format_ndef(const local::HfIdentity& card, uint8_t flags, const Bytes& expected) override {
        bool mutation = false;
        const auto generation_before = generation(1);
        const auto deadline = Clock::now()+std::chrono::seconds(120);
        return hf_call(mutation, [&] {
            local::FormatIO io;
            io.guard = [&]() -> Status {
                if (cancelled()) return CANCELLED;
                if (Clock::now() >= deadline) return TIMEOUT;
                if (!generation_before || generation(1) != generation_before) return STALE_GENERATION;
                same(card); return OK;
            };
            io.read = [&](unsigned b, const local::ClassicKey& key, Bytes& out) -> Status {
                same(card);
                if (card.classic()) require(radio_->authenticate(card,b,key));
                auto data = read(b);
                out.assign(data.begin(),data.begin()+(card.classic() ? 16 : 4)); return OK;
            };
            io.write = [&](unsigned b, const local::ClassicKey& key, const Bytes& data) -> Status {
                if (card.classic()) require(radio_->authenticate(card,b,key));
                if (cancelled()) return CANCELLED;
                mutation = true; return radio_->write(b,data);
            };
            require(local::canonical_format(card,flags,expected,io));
        });
    }
    Status reset_hf() override {
        if (!generation(1)) return UNAVAILABLE;
        try {
            auto status = radio_->release(cleanup_deadline());
            hf_dirty_ = status != OK; cleanup_failed_ = status != OK;
            if (status != OK) quarantine("HF RF-off could not be verified; reconnect required.");
            return status;
        } catch (...) {
            hf_dirty_ = cleanup_failed_ = true;
            quarantine("HF cleanup failed; reconnect required."); return IO_ERROR;
        }
    }
    Status transact(uint8_t channel, const Bytes& request, Bytes& response) override {
        response.clear();
        if (channel != 2 && channel != 3) return INVALID_ARGUMENT;
        auto status = live(channel); if (status != OK) return status;
        status = channel == 2 ? validate_uhf(request) : validate_chameleon(request);
        if (status != OK) return status;
        unsigned cmd = channel == 2 ? request[2] : unsigned(request[2]) << 8 | request[3];
        if (!supports(channel, uint16_t(cmd))) return UNSUPPORTED;
        bool mutation = false;
        try {
            if (channel == 2 && cmd == 0x49) {
                if (!guard_.writable()) return PERMISSION_DENIED;
                Bytes check, query = uhf_frame(0x0b);
                require(exchange(2, query, check)); guard_.observe(query, check);
                if (!guard_.writable()) return PERMISSION_DENIED;
                Bytes payload(request.begin() + 5, request.begin() + 9);
                payload.insert(payload.end(), {1, 0, 1, 0, 1});
                require(exchange(2, uhf_frame(0x39, payload), check));
                const auto& selected = guard_.identity();
                const size_t id_size = selected.size() - 7;
                if (check.size() != id_size + 10 || check[1] != 1 || check[2] != 0x39 || check[5] != id_size ||
                    !std::equal(selected.begin() + 7, selected.end(), check.begin() + 6) ||
                    !std::equal(selected.begin() + 7, selected.begin() + 9, check.begin() + 6 + id_size))
                    return STALE_DATA;
            }
            if (cancelled()) return CANCELLED;
            if (channel == 2 && cmd == 0x22) inventory_used_ = true;
            if (channel == 3 && cmd == 1001) mode_dirty_ = true;
            mutation = channel == 2 ? cmd == 0x49 || cmd == 0xb6 : cmd == 3016;
            status = exchange(channel, request, response);
            if (channel == 2) {
                if (status == OK) guard_.observe(request, response); else guard_.clear();
                if (cmd == 0x28 && status == OK && response.size() == 8 && response[5] == 0)
                    inventory_used_ = false;
            }
        } catch (Status failure) { status = failure; }
        catch (...) { status = IO_ERROR; }
        if (status == IO_ERROR || status == TIMEOUT || status == BAD_FRAME || status == CANCELLED) {
            // An abandoned serial response must never be consumed as the next command's reply.
            quarantine("Serial command interrupted or attachment lost; reconnect before reuse.");
        }
        return mutation && status != OK ? WRITE_UNVERIFIED : status;
    }
};
}
std::unique_ptr<ReaderPort> make_port(const ReaderConfig& config, Progress progress,
                                     std::unique_ptr<Wire> wire, std::unique_ptr<Radio> radio) {
    return std::make_unique<LinuxPort>(config, std::move(progress), std::move(wire), std::move(radio));
}
}
namespace qb::zero {
std::unique_ptr<ReaderPort> make_linux_port(const ReaderConfig& config, Progress progress) {
    return platform::make_port(config, std::move(progress), platform::make_posix_wire());
}
}
