#include "wire.hpp"
#include "bluetooth.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#if defined(__linux__) || defined(QB_ZERO_PTY_TEST)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#endif

namespace qb::zero::platform {
Bytes uhf_frame(uint8_t command, const Bytes& data) {
    Bytes out{0xbb, 0, command, uint8_t(data.size() >> 8), uint8_t(data.size())};
    out.insert(out.end(), data.begin(), data.end());
    uint8_t sum = 0;
    for (size_t i = 1; i < out.size(); ++i) sum += out[i];
    out.push_back(sum); out.push_back(0x7e);
    return out;
}
Bytes ultra_frame(unsigned command, const Bytes& data) {
    Bytes out{0x11, 0xef, uint8_t(command >> 8), uint8_t(command), 0, 0,
              uint8_t(data.size() >> 8), uint8_t(data.size()), 0};
    uint8_t sum = 0;
    for (auto b : out) sum += b;
    out[8] = uint8_t(-sum);
    out.insert(out.end(), data.begin(), data.end()); sum = 0;
    for (auto b : data) sum += b;
    out.push_back(uint8_t(-sum));
    return out;
}
bool ultra_success(const Bytes& response, unsigned status) {
    return response.size() >= 10 && response[4] == uint8_t(status >> 8) && response[5] == uint8_t(status);
}
namespace {
class PosixWire final : public Wire {
    std::shared_ptr<BleSession> ble_;
    [[maybe_unused]] int fds_[3]{-1, -1, -1};
    [[maybe_unused]] unsigned address_ = 0;
#if defined(__linux__) || defined(QB_ZERO_PTY_TEST)
    termios saved_[3]{};
    bool saved_valid_[3]{};
    static speed_t baud(unsigned value) {
        switch (value) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return 0;
        }
    }
    Status endpoint(const std::string& path, unsigned slot, unsigned rate) {
        if (path.empty()) return OK;
        if (path[0] != '/' || path.find('\0') != std::string::npos) return INVALID_ARGUMENT;
        int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return errno == EBUSY ? BUSY : errno == EACCES || errno == EPERM ? PERMISSION_DENIED : UNAVAILABLE;
        fds_[slot] = fd;
        struct stat current{};
        if (fstat(fd, &current) || !S_ISCHR(current.st_mode) || flock(fd, LOCK_EX | LOCK_NB)) return BUSY;
        for (unsigned i = 0; i < slot; ++i) if (fds_[i] >= 0) {
            struct stat prior{};
            if (fstat(fds_[i], &prior)) return IO_ERROR;
            if (prior.st_dev == current.st_dev && prior.st_ino == current.st_ino) return BUSY;
            if (prior.st_rdev == current.st_rdev) return BUSY;
        }
        if (!slot) return OK;
        if (!baud(rate) || tcgetattr(fd, &saved_[slot])) return INVALID_ARGUMENT;
        saved_valid_[slot] = true;
        if (ioctl(fd, TIOCEXCL)) return BUSY;
        termios options = saved_[slot];
        cfmakeraw(&options);
        options.c_cflag |= CLOCAL | CREAD;
        options.c_cflag &= ~(CSTOPB | PARENB | CSIZE);
        options.c_cflag |= CS8;
#ifdef CRTSCTS
        options.c_cflag &= ~CRTSCTS;
#endif
        options.c_cc[VMIN] = 0; options.c_cc[VTIME] = 0;
        if (cfsetispeed(&options, baud(rate)) || cfsetospeed(&options, baud(rate)) ||
            tcsetattr(fd, TCSANOW, &options) || tcflush(fd, TCIOFLUSH)) return IO_ERROR;
        return OK;
    }
#endif
public:
    ~PosixWire() override { close(); }
    Status open(const ReaderConfig& config) override {
        close();
        if (!config.ultra_ble_token.empty()) {
            if (!config.ultra_serial.empty() || !config.ble_session) return INVALID_ARGUMENT;
            ble_ = config.ble_session;
            auto status = ble_->connect(config.ultra_ble_token);
            if (status != OK) { close(); return status; }
        }
#if defined(__linux__) || defined(QB_ZERO_PTY_TEST)
        const std::string paths[]{config.hf_i2c, config.uhf_serial, config.ultra_serial};
        std::set<std::string> unique;
        for (auto& path : paths) if (!path.empty() && !unique.insert(path).second) return INVALID_ARGUMENT;
        if (!config.hf_i2c.empty() && (!config.grove_i2c_confirmed || config.hf_address != 0x28))
            return PERMISSION_DENIED;
        for (unsigned i = 0; i < 3; ++i) {
            auto status = endpoint(paths[i], i, i == 1 ? config.uhf_baud : 115200);
            if (status != OK) { close(); return status; }
        }
        address_ = config.hf_address;
#if defined(__linux__)
        if (fds_[0] >= 0) {
            unsigned long functions = 0;
            // Linux timeout units are 10ms. Never use FORCE, scan, or bus retry writes.
            if (ioctl(fds_[0], I2C_FUNCS, &functions) || !(functions & I2C_FUNC_I2C) ||
                ioctl(fds_[0], I2C_TIMEOUT, 10) || ioctl(fds_[0], I2C_RETRIES, 0)) {
                close(); return UNSUPPORTED;
            }
        }
#else
        if (fds_[0] >= 0) { close(); return UNSUPPORTED; }
#endif
        return OK;
#else
        (void)config;
        return UNSUPPORTED;
#endif
    }
    void close() noexcept override {
        if (ble_) {
            try { ble_->disconnect(); } catch (...) { ble_->shutdown(); }
            ble_.reset();
        }
#if defined(__linux__) || defined(QB_ZERO_PTY_TEST)
        for (unsigned i = 0; i < 3; ++i) if (fds_[i] >= 0) {
            if (i && saved_valid_[i]) {
                tcsetattr(fds_[i], TCSANOW, &saved_[i]);
                ioctl(fds_[i], TIOCNXCL);
            }
            ::close(fds_[i]); fds_[i] = -1; saved_valid_[i] = false;
        }
#endif
    }
    Status read_register(uint8_t reg, uint8_t& value) override {
#if defined(__linux__)
        if (fds_[0] < 0 || reg > 0x3f) return UNAVAILABLE;
        i2c_msg messages[2]{{uint16_t(address_), 0, 1, &reg},
                            {uint16_t(address_), I2C_M_RD, 1, &value}};
        i2c_rdwr_ioctl_data data{messages, 2};
        return ioctl(fds_[0], I2C_RDWR, &data) == 2 ? OK : IO_ERROR;
#else
        (void)reg; (void)value; return UNSUPPORTED;
#endif
    }
    Status write_register(uint8_t reg, uint8_t value) override {
#if defined(__linux__)
        if (fds_[0] < 0 || reg > 0x3f) return UNAVAILABLE;
        uint8_t bytes[]{reg, value};
        i2c_msg message{uint16_t(address_), 0, 2, bytes};
        i2c_rdwr_ioctl_data data{&message, 1};
        return ioctl(fds_[0], I2C_RDWR, &data) == 1 ? OK : IO_ERROR;
#else
        (void)reg; (void)value; return UNSUPPORTED;
#endif
    }
    Status exchange(uint8_t channel, const Bytes& request, Bytes& response,
                    unsigned timeout_ms, const Stop& stop) override {
        response.clear();
        if (channel == 3 && ble_) return ble_->exchange(request, response, timeout_ms, stop);
#if defined(__linux__) || defined(QB_ZERO_PTY_TEST)
        if ((channel != 2 && channel != 3) || request.size() < 7 || request.size() > max_payload ||
            !timeout_ms || timeout_ms > 20000) return INVALID_ARGUMENT;
        int fd = fds_[channel - 1];
        if (fd < 0) return UNAVAILABLE;
        if (stop()) return CANCELLED;
        if (tcflush(fd, TCIFLUSH)) return IO_ERROR;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        size_t sent = 0;
        Bytes buffer;
        for (;;) {
            if (stop()) return CANCELLED;
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return TIMEOUT;
            pollfd p{fd, short(sent < request.size() ? POLLOUT : POLLIN), 0};
            int ready = poll(&p, 1, int(std::min<int64_t>(left, 20)));
            if (ready < 0) { if (errno == EINTR) continue; return IO_ERROR; }
            if (!ready) continue;
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return IO_ERROR;
            if (sent < request.size()) {
                ssize_t n = ::write(fd, request.data() + sent, request.size() - sent);
                if (n > 0) sent += size_t(n);
                else if (n < 0 && errno != EAGAIN && errno != EINTR) return IO_ERROR;
                continue;
            }
            uint8_t bytes[256];
            ssize_t n = ::read(fd, bytes, sizeof(bytes));
            if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; return IO_ERROR; }
            if (!n) return IO_ERROR;
            if (buffer.size() + size_t(n) > max_buffer) return BAD_FRAME;
            buffer.insert(buffer.end(), bytes, bytes + n);
            if (channel == 2) {
                for (;;) {
                    Bytes frame;
                    auto status = take_uhf_frame(buffer, frame);
                    if (status == BUSY) break;
                    if (status != OK) return status;
                    if (frame[2] != request[2] && !(frame[1] == 1 && frame[2] == 0xff)) return BAD_FRAME;
                    if (response.size() + frame.size() > max_buffer) return BAD_FRAME;
                    response.insert(response.end(), frame.begin(), frame.end());
                }
                if (!response.empty() && buffer.empty()) return stop() ? CANCELLED : OK;
            } else if (buffer.size() >= 9) {
                const size_t total = (size_t(buffer[6]) << 8 | buffer[7]) + 10;
                if (total > max_payload || buffer[0] != 0x11 || buffer[1] != 0xef ||
                    buffer[2] != request[2] || buffer[3] != request[3]) return BAD_FRAME;
                uint8_t sum = 0;
                for (unsigned i = 0; i < 9; ++i) sum += buffer[i];
                if (sum || buffer.size() > total) return BAD_FRAME;
                if (buffer.size() < total) continue;
                for (size_t i = 9; i < total; ++i) sum += buffer[i];
                if (sum) return BAD_FRAME;
                response = buffer; return stop() ? CANCELLED : OK;
            }
        }
#else
        (void)channel; (void)request; (void)timeout_ms; (void)stop;
        return UNSUPPORTED;
#endif
    }
};
}
std::unique_ptr<Wire> make_posix_wire() { return std::make_unique<PosixWire>(); }
}
