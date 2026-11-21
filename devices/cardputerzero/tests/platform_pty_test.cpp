#include "platform/wire.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#if defined(__linux__) || defined(QB_ZERO_PTY_TEST)
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
using namespace qb;
using namespace qb::zero;
using namespace qb::zero::platform;
namespace {
struct Pty {
    int master = -1;
    std::string path;
    Pty() {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        assert(master >= 0 && !grantpt(master) && !unlockpt(master));
        auto* name = ptsname(master); assert(name); path = name;
    }
    ~Pty() { if (master >= 0) ::close(master); }
    void receive(const Bytes& expected) {
        Bytes got;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (got.size() < expected.size()) {
            assert(std::chrono::steady_clock::now() < deadline);
            pollfd p{master, POLLIN, 0}; int ready = poll(&p, 1, 20); assert(ready >= 0);
            if (!ready) continue;
            uint8_t b[256]; auto n = ::read(master, b, sizeof(b)); assert(n > 0);
            got.insert(got.end(), b, b + n);
        }
        assert(got == expected);
    }
    void send(const Bytes& bytes) {
        size_t at = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (at < bytes.size()) {
            assert(std::chrono::steady_clock::now() < deadline);
            pollfd p{master, POLLOUT, 0}; int ready = poll(&p, 1, 20); assert(ready >= 0);
            if (!ready) continue;
            auto n = ::write(master, bytes.data() + at, bytes.size() - at); assert(n > 0);
            at += size_t(n);
        }
    }
};
void serial_frames() {
    Pty pty; ReaderConfig config; config.uhf_serial = pty.path;
    auto wire = make_posix_wire(); assert(wire->open(config) == OK);
    auto other = make_posix_wire(); assert(other->open(config) != OK);
    auto req = uhf_frame(3, {0}); auto reply = uhf_frame(3, {0, 'M', '1', '0', '0'});
    reply[1] = 1; ++reply[reply.size() - 2];
    std::thread module([&] {
        pty.receive(req);
        pty.send(Bytes(reply.begin(), reply.begin() + 4));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pty.send(Bytes(reply.begin() + 4, reply.end()));
    });
    Bytes out; assert(wire->exchange(2, req, out, 500, [] { return false; }) == OK);
    module.join(); assert(out == reply);
    std::thread bad([&] { pty.receive(req); auto corrupt = reply; corrupt[corrupt.size() - 2] ^= 1; pty.send(corrupt); });
    assert(wire->exchange(2, req, out, 500, [] { return false; }) == IO_ERROR); bad.join();
    std::thread wrong([&] { pty.receive(req); auto wrong_cmd = uhf_frame(0x28, {0}); wrong_cmd[1] = 1; ++wrong_cmd[wrong_cmd.size() - 2]; pty.send(wrong_cmd); });
    assert(wire->exchange(2, req, out, 500, [] { return false; }) == BAD_FRAME); wrong.join();
    auto start = std::chrono::steady_clock::now();
    assert(wire->exchange(2, req, out, 100, [] { return false; }) == TIMEOUT);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    pty.receive(req);
    assert(wire->exchange(2, req, out, 500, [] { return true; }) == CANCELLED);
    pollfd check{pty.master, POLLIN, 0}; assert(poll(&check, 1, 30) == 0);
    std::atomic<bool> stop{false};
    std::thread stopped([&] { pty.receive(req); stop.store(true); });
    start = std::chrono::steady_clock::now();
    assert(wire->exchange(2, req, out, 500, [&] { return stop.load(); }) == CANCELLED);
    stopped.join(); assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    wire->close(); assert(other->open(config) == OK); other->close();
}
void ultra_frames() {
    Pty pty; ReaderConfig config; config.ultra_serial = pty.path;
    auto wire = make_posix_wire(); assert(wire->open(config) == OK);
    auto req = ultra_frame(1002); auto reply = ultra_frame(1002, {0});
    reply[5] = 0x68; reply[8] = uint8_t(reply[8] - 0x68);
    Bytes out;
    std::thread module([&] {
        pty.receive(req);
        for (auto byte : reply) { pty.send({byte}); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    });
    assert(wire->exchange(3, req, out, 500, [] { return false; }) == OK);
    module.join(); assert(out == reply);
    std::thread bad([&] { pty.receive(req); auto corrupt = reply; corrupt.back() ^= 1; pty.send(corrupt); });
    assert(wire->exchange(3, req, out, 500, [] { return false; }) == BAD_FRAME); bad.join();
    std::thread oversized([&] {
        pty.receive(req);
        auto corrupt = reply; corrupt[6] = 0x7f; corrupt[7] = 0xff; pty.send(corrupt);
    });
    assert(wire->exchange(3, req, out, 500, [] { return false; }) == BAD_FRAME); oversized.join();
    std::thread disconnected([&] { pty.receive(req); ::close(pty.master); pty.master = -1; });
    assert(wire->exchange(3, req, out, 500, [] { return false; }) == IO_ERROR); disconnected.join();
}
void explicit_endpoints() {
    auto wire = make_posix_wire(); ReaderConfig c; c.uhf_serial = "relative";
    assert(wire->open(c) == INVALID_ARGUMENT);
    c.uhf_serial = "/never/opened"; c.ultra_serial = c.uhf_serial;
    assert(wire->open(c) == INVALID_ARGUMENT);
    c = {}; c.hf_i2c = "/never/opened";
    assert(wire->open(c) == PERMISSION_DENIED);
    Pty pty; c = {}; c.uhf_serial = pty.path; c.uhf_baud = 12345;
    assert(wire->open(c) == INVALID_ARGUMENT);
}
}
int main() {
    explicit_endpoints(); serial_frames(); ultra_frames();
    std::cout << "platform_pty_test: exclusive endpoints, fragmentation, checksum/length/command rejection, deadline, cancellation, disconnect passed\n";
}
#else
int main() {
    std::cout << "platform_pty_test: skipped (explicit QB_ZERO_PTY_TEST build required on non-Linux)\n";
}
#endif
