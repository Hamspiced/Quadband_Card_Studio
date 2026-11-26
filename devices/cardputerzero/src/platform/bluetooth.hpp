#pragma once
#include "wire.hpp"
#include <mutex>

namespace qb::zero::platform {
class BleSession {
public:
    using Observer = std::function<void(const std::string&)>;
    explicit BleSession(Observer = {}, Stop = {}, const std::string& helper = "");
    ~BleSession();
    Status preflight(std::string&);
    Status scan(std::vector<BlePeer>&);
    Status connect(const std::string&);
    Status disconnect();
    bool cleanup_unverified() const { return cleanup_unverified_; }
    const std::string& detail() const { return last_error_; }
    Status exchange(const Bytes&, Bytes&, unsigned, const Stop&);
    bool pin(const std::string&);
    bool healthy();
    void shutdown() noexcept;
private:
    int input_ = -1, output_ = -1, child_ = -1;
    uint32_t session_ = 0, request_ = 0;
    Observer observer_;
    Stop stop_;
    std::mutex pin_mutex_;
    std::array<char, 6> pin_{};
    bool prompt_ = false, answered_ = false;
    bool connected_ = false, cleanup_unverified_ = false;
    std::string last_error_;
    Status request(uint8_t, const Bytes&, Bytes&, unsigned, const Stop&);
    bool transfer(int, uint8_t*, size_t, bool, Deadline, const Stop&);
    bool send(uint8_t, const Bytes&, Deadline, const Stop&);
};
}
