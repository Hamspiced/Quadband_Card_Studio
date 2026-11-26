#include "bluetooth.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;

namespace qb::zero::platform {
namespace {
void put(Bytes& b, uint32_t n) {
    for (int shift : {24, 16, 8, 0}) b.push_back(uint8_t(n >> shift));
}
uint32_t get(const uint8_t* b) {
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
}
std::string helper_path() {
    char executable[4096]{};
#if defined(__linux__)
    auto n = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (n <= 0 || size_t(n) >= sizeof(executable) - 1) return {};
    executable[n] = 0;
#elif defined(__APPLE__)
    uint32_t n = sizeof(executable);
    if (_NSGetExecutablePath(executable, &n)) return {};
#else
    return {};
#endif
    std::string path(executable);
    return path.substr(0, path.find_last_of('/')) + "/helper/ble_helper.py";
}
bool token_valid(const std::string& s) {
    return s.size() == 16 && std::all_of(s.begin(), s.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string failure_detail(const std::string& payload) {
    const std::pair<const char*, const char*> phases[]{
        {"preflight", "runtime check"}, {"scanning", "scan"}, {"select", "selection"},
        {"pairing", "pairing before PIN"}, {"pin", "PIN entry"}, {"connecting", "GATT connection"},
        {"subscribing", "notification subscription"}, {"validating", "validation"},
        {"exchange", "transaction"}, {"disconnect", "disconnect"}};
    const std::pair<const char*, const char*> reasons[]{
        {"timeout", "Reader did not respond; keep it awake and close competing reader apps."},
        {"authentication", "Pairing was refused or canceled; check the reader's configured pairing policy/PIN."},
        {"busy", "Another connection or pairing attempt is active; close the competing client."},
        {"permission", "BlueZ denied access; check normal-user Bluetooth permissions."},
        {"unavailable", "Reader or adapter is unavailable; check power and scan again."},
        {"dependencies", "Requires normal-user Python, BlueZ, Bleak >=0.22.3 and dbus-fast."},
        {"cleanup", "The backend could not confirm disconnect; restart the application."},
        {"protocol", "Selection or helper protocol was invalid; no automatic retry."},
        {"compatibility", "Helper Bluetooth API mismatch; report this phase and category."},
        {"transport", "Helper transport failed; no backend text or PIN was recorded."}};
    for (const auto& phase : phases) for (const auto& reason : reasons)
        if (payload == std::string("ble:") + phase.first + ":" + reason.first)
            return std::string("Bluetooth ") + phase.second + " failed (" + reason.first + "). " + reason.second;
    return "Bluetooth helper failed; unrecognized backend details withheld.";
}
}
BleSession::BleSession(Observer observer, Stop stop, const std::string& supplied)
    : observer_(std::move(observer)), stop_(std::move(stop)) {
    static std::once_flag signals;
    std::call_once(signals, [] { std::signal(SIGPIPE, SIG_IGN); });
    // All non-pipe descriptors are close-on-exec; spawn never invokes a shell.
    std::string path = supplied.empty() ? helper_path() : supplied;
    if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos) return;
    int a[2]{-1,-1}, b[2]{-1,-1};
    if (pipe(a)) return;
    if (pipe(b)) { ::close(a[0]); ::close(a[1]); return; }
    int child_in = fcntl(a[0], F_DUPFD_CLOEXEC, 10);
    int child_out = fcntl(b[1], F_DUPFD_CLOEXEC, 10);
    ::close(a[0]); ::close(b[1]);
    input_ = b[0]; output_ = a[1];
    fcntl(input_, F_SETFD, FD_CLOEXEC); fcntl(output_, F_SETFD, FD_CLOEXEC);
    fcntl(input_, F_SETFL, O_NONBLOCK); fcntl(output_, F_SETFL, O_NONBLOCK);
    if (child_in < 0 || child_out < 0) {
        if (child_in >= 0) ::close(child_in);
        if (child_out >= 0) ::close(child_out);
        shutdown(); return;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, child_in, 3);
    posix_spawn_file_actions_adddup2(&actions, child_out, 4);
    posix_spawn_file_actions_addclose(&actions, child_in);
    posix_spawn_file_actions_addclose(&actions, child_out);
    if (auto* directory = opendir("/dev/fd")) {
        while (auto* entry = readdir(directory)) {
            char* end = nullptr;
            long fd = std::strtol(entry->d_name, &end, 10);
            if (end && !*end && fd >= 5 && fd != child_in && fd != child_out && fd != dirfd(directory))
                posix_spawn_file_actions_addclose(&actions, int(fd));
        }
        closedir(directory);
    } else {
        posix_spawn_file_actions_destroy(&actions);
        ::close(child_in); ::close(child_out); shutdown(); return;
    }
    for (int fd : {0, 1, 2}) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null",
        fd ? O_WRONLY : O_RDONLY, 0);
    char python[] = "/usr/bin/python3", isolated[] = "-I", readfd[] = "3", writefd[] = "4";
    char* argv[]{python, isolated, path.data(), readfd, writefd, nullptr};
    pid_t pid = -1;
    int result = posix_spawn(&pid, python, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(child_in); ::close(child_out);
    if (result) { shutdown(); return; }
    child_ = pid;
    static std::atomic<uint32_t> next{0};
    session_ = ++next;
    if (!session_) session_ = ++next;
}
BleSession::~BleSession() { shutdown(); }
void BleSession::shutdown() noexcept {
    if (connected_) cleanup_unverified_ = true;
    { std::lock_guard<std::mutex> lock(pin_mutex_);
      local::secure_zero(pin_.data(), pin_.size()); prompt_ = answered_ = false; }
    if (output_ >= 0) { ::close(output_); output_ = -1; }
    if (input_ >= 0) { ::close(input_); input_ = -1; }
    if (child_ > 0) {
        bool reaped = false;
        for (unsigned i = 0; i < 20; ++i) {
            auto result = waitpid(child_, nullptr, WNOHANG);
            if (result == child_ || (result < 0 && errno == ECHILD)) { reaped = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!reaped) {
            kill(child_, SIGTERM);
            for (unsigned i = 0; i < 20; ++i) {
                auto result = waitpid(child_, nullptr, WNOHANG);
                if (result == child_ || (result < 0 && errno == ECHILD)) { reaped = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (!reaped) { kill(child_, SIGKILL); while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {} }
        child_ = -1;
    }
}
bool BleSession::healthy() {
    if (child_ <= 0 || input_ < 0) return false;
    pid_t result;
    do { result = waitpid(child_, nullptr, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result != 0) {
        if (last_error_.empty()) last_error_ = "Bluetooth helper exited between requests.";
        if (result == child_ || (result < 0 && errno == ECHILD)) child_ = -1;
        shutdown(); return false;
    }
    pollfd p{input_, POLLIN, 0};
    if (poll(&p, 1, 0) > 0 && p.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
        if (last_error_.empty()) last_error_ = p.revents & POLLIN ?
            "Bluetooth helper sent unexpected data between requests." :
            "Bluetooth helper pipe closed between requests.";
        shutdown(); return false;
    }
    return true;
}
bool BleSession::pin(const std::string& value) {
    std::lock_guard<std::mutex> lock(pin_mutex_);
    if (!prompt_ || answered_ || value.size() != 6 ||
        !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    std::copy(value.begin(), value.end(), pin_.begin()); answered_ = true; return true;
}
bool BleSession::transfer(int fd, uint8_t* data, size_t count, bool writing, Deadline due, const Stop& stop) {
    while (count) {
        if ((stop && stop()) || Clock::now() >= due) return false;
        pollfd p{fd, short(writing ? POLLOUT : POLLIN), 0};
        int ready = poll(&p, 1, 20);
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (!ready) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        auto n = writing ? ::write(fd, data, count) : ::read(fd, data, count);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return false;
        count -= size_t(n); data += n;
    }
    return true;
}
bool BleSession::send(uint8_t kind, const Bytes& data, Deadline due, const Stop& stop) {
    if (data.size() > 8182) return false;
    Bytes envelope; put(envelope, uint32_t(data.size() + 10));
    envelope.push_back(1); envelope.push_back(kind); put(envelope, session_); put(envelope, request_);
    envelope.insert(envelope.end(), data.begin(), data.end());
    auto ok = transfer(output_, envelope.data(), envelope.size(), true, due, stop);
    local::secure_zero(envelope.data(), envelope.size());
    return ok;
}
Status BleSession::request(uint8_t kind, const Bytes& data, Bytes& response, unsigned timeout, const Stop& stop) {
    response.clear();
    if (!healthy() || ++request_ == 0) {
        if (last_error_.empty()) last_error_ = "Bluetooth helper is unavailable before the request.";
        shutdown(); return UNAVAILABLE;
    }
    const auto due = Clock::now() + std::chrono::milliseconds(timeout);
    const Stop canceled = [this, kind, &stop] {
        return ((kind == 1 || kind == 2 || kind == 5) && stop_ && stop_()) || (stop && stop());
    };
    if (!send(kind, data, due, canceled)) { shutdown(); return canceled() ? CANCELLED : IO_ERROR; }
    bool pin_seen = false;
    std::string phase = kind == 1 ? "scanning" : kind == 2 ? "select" :
        kind == 3 ? "exchange" : kind == 4 ? "disconnect" : "preflight";
    for (unsigned events = 0; events < 32;) {
        Bytes pin;
        { std::lock_guard<std::mutex> lock(pin_mutex_);
          if (prompt_ && answered_) {
              pin.assign(pin_.begin(), pin_.end());
              local::secure_zero(pin_.data(), pin_.size()); prompt_ = answered_ = false;
          } }
        if (!pin.empty()) {
            bool ok = send(6, pin, due, canceled); local::secure_zero(pin.data(), pin.size());
            if (!ok) break;
        }
        if (canceled() || Clock::now() >= due) break;
        pollfd p{input_, POLLIN, 0};
        int ready = poll(&p, 1, 20);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (ready && p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (!ready) continue;
        uint8_t length[4];
        if (!transfer(input_, length, 4, false, due, canceled)) break;
        auto size = get(length);
        if (size < 10 || size > 8192) break;
        Bytes envelope(size);
        if (!transfer(input_, envelope.data(), size, false, due, canceled) ||
            envelope[0] != 1 || get(envelope.data() + 2) != session_ ||
            get(envelope.data() + 6) != request_) break;
        if (envelope[1] == 128) {
            response.assign(envelope.begin() + 10, envelope.end());
            { std::lock_guard<std::mutex> lock(pin_mutex_); prompt_ = answered_ = false; }
            return OK;
        }
        if (envelope[1] == 130) {
            if (last_error_.empty())
                last_error_ = failure_detail(std::string(envelope.begin() + 10, envelope.end()));
            break;
        }
        if (envelope[1] != 129) break;
        std::string state(envelope.begin() + 10, envelope.end());
        if (state == "pin") {
            if (kind != 2 || pin_seen) break;
            pin_seen = true;
        }
        const bool valid_event = (kind == 1 && (state == "scanning" || state == "select")) ||
            (kind == 2 && (state == "pairing" || state == "pin" || state == "connecting" ||
                           state == "subscribing" || state == "validating"));
        if (!valid_event) break;
        phase = state;
        { std::lock_guard<std::mutex> lock(pin_mutex_); prompt_ = state == "pin"; }
        if (observer_) observer_(state);
        ++events;
    }
    if (last_error_.empty())
        last_error_ = canceled() ? "Bluetooth request canceled." :
            failure_detail("ble:" + phase + (Clock::now() >= due ? ":timeout" : ":transport"));
    shutdown();
    if (observer_) observer_("disconnected");
    return canceled() ? CANCELLED : Clock::now() >= due ? TIMEOUT : IO_ERROR;
}
Status BleSession::preflight(std::string& detail) {
    Bytes response; auto status = request(5, {}, response, 4000, {});
    detail = status == OK ? "Bluetooth runtime available (no scan performed)" :
        !last_error_.empty() ? last_error_ :
            "Bluetooth requires unprivileged Linux, python3, bluez, python3-bleak >=0.22.3, python3-dbus-fast";
    return status;
}
Status BleSession::scan(std::vector<BlePeer>& peers) {
    peers.clear(); Bytes response;
    auto status = request(1, {}, response, 10000, {});
    if (status != OK) return status;
    if (response.empty() || response[0] > 16) { shutdown(); return BAD_FRAME; }
    size_t at = 1;
    for (unsigned i = 0; i < response[0]; ++i) {
        if (response.size() - at < 17) { shutdown(); return BAD_FRAME; }
        std::string token(response.begin() + at, response.begin() + at + 16); at += 16;
        unsigned size = response[at++];
        if (!token_valid(token) || size > 64 || response.size() - at < size) { shutdown(); return BAD_FRAME; }
        std::string name(response.begin() + at, response.begin() + at + size); at += size;
        if (!std::all_of(name.begin(), name.end(), [](char c) { return c >= 32 && c < 127; })) {
            shutdown(); return BAD_FRAME;
        }
        for (auto& peer : peers) if (peer.token == token) { shutdown(); return BAD_FRAME; }
        peers.push_back({token, name});
    }
    if (at != response.size()) { shutdown(); peers.clear(); return BAD_FRAME; }
    return OK;
}
Status BleSession::connect(const std::string& token) {
    if (!token_valid(token)) return INVALID_ARGUMENT;
    connected_ = true;
    Bytes response; auto status = request(2, Bytes(token.begin(), token.end()), response, 60000, {});
    if (status == OK && !response.empty()) { shutdown(); return BAD_FRAME; }
    return status;
}
Status BleSession::disconnect() {
    if (cleanup_unverified_) { shutdown(); return IO_ERROR; }
    if (!connected_) return OK;
    Bytes response;
    auto status = request(4, {}, response, 2000, {});
    if (status == OK && response.empty()) connected_ = false;
    else { status = IO_ERROR; cleanup_unverified_ = true; }
    shutdown();
    return status;
}
Status BleSession::exchange(const Bytes& frame, Bytes& response, unsigned timeout, const Stop& stop) {
    if (frame.size() < 10 || frame.size() > 4096 || timeout == 0 || timeout > 20000) return INVALID_ARGUMENT;
    Bytes payload; put(payload, timeout); payload.insert(payload.end(), frame.begin(), frame.end());
    auto status = request(3, payload, response, timeout, stop);
    if (status != OK) return status;
    if (response.size() < 10 || response.size() > 4096 || response[0] != 0x11 || response[1] != 0xef ||
        response[2] != frame[2] || response[3] != frame[3] ||
        (size_t(response[6]) << 8 | response[7]) + 10 != response.size()) {
        shutdown(); return BAD_FRAME;
    }
    uint8_t header = 0, body = 0;
    for (unsigned i = 0; i < 9; ++i) header += response[i];
    for (size_t i = 9; i < response.size(); ++i) body += response[i];
    if (header || body) { shutdown(); return BAD_FRAME; }
    return OK;
}
}
