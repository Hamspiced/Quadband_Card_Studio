#include "discovery.hpp"
#include "wire.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#if defined(__linux__)
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace qb::zero::platform {
Bytes identity_request(Peripheral device, unsigned step) {
    if (device == Peripheral::Uhf && !step) return uhf_frame(3,{0});
    if (device == Peripheral::Rfid && !step) return {0x37};
    if (device == Peripheral::Cc1101 && step < 2) return {uint8_t(0xf0 + step),0};
    if (device == Peripheral::CapNfc && !step) return {0x7f,0};
    return {};
}
const char* detection_name(Detection state) {
    switch (state) {
    case Detection::Detected: return "Detected";
    case Detection::Missing: return "Not found";
    case Detection::Busy: return "In use";
    case Detection::Permission: return "Permission needed";
    case Detection::Unsupported: return "Unsupported";
    case Detection::Blocked: return "Power/routing needed";
    case Detection::Cancelled: return "Cancelled";
    case Detection::Manual: return "Manual endpoint";
    default: return "Not checked";
    }
}
namespace {
std::string hex(unsigned value) { char text[12]; std::snprintf(text,sizeof(text),"0x%02X",value); return text; }
Detection failure(Status status) {
    if (status == BUSY) return Detection::Busy;
    if (status == PERMISSION_DENIED || status == NOT_AUTHORIZED) return Detection::Permission;
    if (status == CANCELLED) return Detection::Cancelled;
    if (status == UNSUPPORTED || status == BAD_FRAME) return Detection::Unsupported;
    return Detection::Missing;
}
bool decode(Peripheral device, const Bytes& data, std::string& identity) {
    if (device == Peripheral::Uhf) {
        Bytes buffer = data, frame;
        if (take_uhf_frame(buffer,frame) != OK || !buffer.empty() || frame.size() < 9 ||
            frame.size() > 80 || frame[1] != 1 || frame[2] != 3 || frame[5] != 0) return false;
        identity.assign(frame.begin()+6,frame.end()-2);
        for (auto& c : identity) {
            if (c == 0) c = ' ';
            if (c < 32 || c > 126) return false;
            if (c >= 'a' && c <= 'z') c = char(c-'a'+'A');
        }
        return identity.find("M100") != std::string::npos || identity.find("JRD") != std::string::npos ||
               identity.find("YRM") != std::string::npos;
    }
    if (device == Peripheral::Rfid && data.size() == 1) {
        if (data[0] == 0x15) { identity = "RFID2 / WS1850S"; return true; }
        if (data[0] == 0x91 || data[0] == 0x92) { identity = "RFID / MFRC522"; return true; }
    }
    if (device == Peripheral::Cc1101 && data.size() == 2 && data[0] == 0 &&
        (data[1] == 0x04 || data[1] == 0x14 || data[1] == 0x20)) {
        identity = "CC1101 " + hex(data[1]) + " (identity only)"; return true;
    }
    if (device == Peripheral::CapNfc && data.size() == 1 &&
        ((data[0] & 0xf8) == 0x28 || (data[0] & 0xf8) == 0x30)) {
        identity = std::string((data[0] & 0xf8) == 0x30 ? "ST25R3916B " : "ST25R3916 ") +
            hex(data[0]) + " (tools not implemented)"; return true;
    }
    return false;
}
#if defined(__linux__)
std::string read_text(const char* path) {
    std::ifstream file(path,std::ios::binary);
    std::string value(512,'\0'); file.read(value.data(),std::streamsize(value.size()));
    value.resize(size_t(file.gcount()));
    return value;
}
bool on(const char* path) {
    auto value = read_text(path);
    return !value.empty() && value[0] != '0';
}
std::string pins(const char* range) {
    int descriptors[2];
    if (pipe(descriptors)) return {};
    const pid_t child = fork();
    if (child == 0) {
        dup2(descriptors[1],STDOUT_FILENO);
        close(descriptors[0]); close(descriptors[1]);
        execl("/usr/bin/pinctrl","pinctrl","get",range,static_cast<char*>(nullptr));
        _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) { close(descriptors[0]); return {}; }
    fcntl(descriptors[0],F_SETFL,O_NONBLOCK);
    std::string output;
    const auto deadline = DiscoveryClock::now()+std::chrono::milliseconds(300);
    bool exited = false;
    while (DiscoveryClock::now() < deadline) {
        char bytes[512]; const auto n = read(descriptors[0],bytes,sizeof(bytes));
        if (n > 0 && output.size()+size_t(n) <= 2048) output.append(bytes,size_t(n));
        int status = 0;
        if (waitpid(child,&status,WNOHANG) == child) {
            exited = true;
            ssize_t remaining;
            while ((remaining = read(descriptors[0],bytes,sizeof(bytes))) > 0)
                if (output.size()+size_t(remaining) <= 2048) output.append(bytes,size_t(remaining));
            if (!WIFEXITED(status) || WEXITSTATUS(status)) output.clear();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    close(descriptors[0]);
    if (!exited) { kill(child,SIGKILL); waitpid(child,nullptr,0); output.clear(); }
    return output;
}
bool spi_node(const char* path, uint8_t index) {
    const auto value = read_text(path);
    return value.size() == 4 && value[0] == 0 && value[1] == 0 && value[2] == 0 && uint8_t(value[3]) == index;
}
class LinuxProbe final : public DiscoveryProbe {
public:
    DiscoveryEnvironment environment() override {
        DiscoveryEnvironment env;
        const auto model = read_text("/proc/device-tree/model");
        env.zero = model.find("Raspberry Pi Compute Module 0") != std::string::npos &&
            read_text("/sys/bus/i2c/devices/1-0034/name").find("tca8418") != std::string::npos &&
            !read_text("/sys/class/leds/grove_fun/brightness").empty();
        if (!env.zero) return env;
        const auto uart = pins("14-15");
        env.uart = read_text("/sys/class/leds/grove_fun/brightness").find('0') == 0 &&
            ((uart.find("GPIO14 = TXD1") != std::string::npos && uart.find("GPIO15 = RXD1") != std::string::npos) ||
             (uart.find("GPIO14 = TXD0") != std::string::npos && uart.find("GPIO15 = RXD0") != std::string::npos));
        const auto i2c = pins("2-3");
        env.i2c = i2c.find("GPIO2 = SDA1") != std::string::npos && i2c.find("GPIO3 = SCL1") != std::string::npos;
        const auto cap = pins("26");
        env.cap_power = on("/sys/class/leds/ext_5v_out/brightness") &&
            cap.find("| hi") != std::string::npos;
        env.spi_routes = spi_node("/sys/bus/spi/devices/spi0.1/of_node/reg",1) &&
            spi_node("/sys/bus/spi/devices/spi0.2/of_node/reg",2);
        return env;
    }
    Status grove_power() override {
        const char* path = "/sys/class/leds/grove_5v_out/brightness";
        if (on(path)) return OK;
        int fd = open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
        if (fd < 0) return errno == EBUSY ? BUSY : errno == EACCES || errno == EPERM ? PERMISSION_DENIED : UNAVAILABLE;
        const auto n = write(fd,"255\n",4); close(fd);
        return n == 4 && on(path) ? OK : IO_ERROR;
    }
    Status identify(Peripheral device, const ReaderConfig& overrides, Bytes& data, const std::function<bool()>& stop,
                    DiscoveryClock::time_point deadline) override {
        data.clear();
        if (stop()) return CANCELLED;
        if (device == Peripheral::Uhf || device == Peripheral::Rfid) {
            ReaderConfig config;
            if (device == Peripheral::Uhf) { config.uhf_serial = "/dev/serial0"; config.uhf_baud = overrides.uhf_baud; }
            else { config.hf_i2c = "/dev/i2c-1"; config.grove_i2c_confirmed = true; }
            auto wire = make_posix_wire();
            auto result = wire->open(config);
            if (result != OK) return result;
            if (device == Peripheral::Uhf) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline-DiscoveryClock::now()).count();
                if (remaining <= 0) return TIMEOUT;
                return wire->exchange(2,identity_request(device),data,unsigned(std::min<int64_t>(500,remaining)),stop);
            }
            uint8_t version = 0;
            result = wire->read_register(identity_request(device)[0],version);
            if (stop()) return CANCELLED;
            if (result == OK) data.push_back(version);
            return result;
        }
        const char* path = device == Peripheral::Cc1101 ? "/dev/spidev0.1" : "/dev/spidev0.2";
        int fd = open(path,O_RDWR|O_CLOEXEC);
        if (fd < 0) return errno == EACCES || errno == EPERM ? PERMISSION_DENIED : UNAVAILABLE;
        if (flock(fd,LOCK_EX|LOCK_NB)) { close(fd); return BUSY; }
        uint8_t mode = 0, bits = 0;
        Status result = OK;
        if (ioctl(fd,SPI_IOC_RD_MODE,&mode) || ioctl(fd,SPI_IOC_RD_BITS_PER_WORD,&bits) ||
            mode != 0 || (bits && bits != 8)) result = UNSUPPORTED;
        const auto reg = [&](unsigned step) -> Status {
            if (stop()) return CANCELLED;
            if (DiscoveryClock::now() >= deadline) return TIMEOUT;
            const Bytes tx = identity_request(device,step);
            uint8_t rx[2]{};
            if (tx.size() != 2) return INVALID_ARGUMENT;
            spi_ioc_transfer transfer{};
            transfer.tx_buf = reinterpret_cast<uintptr_t>(tx.data()); transfer.rx_buf = reinterpret_cast<uintptr_t>(rx);
            transfer.len = 2; transfer.speed_hz = 500000; transfer.bits_per_word = 8;
            if (ioctl(fd,SPI_IOC_MESSAGE(1),&transfer) != 2) return IO_ERROR;
            data.push_back(rx[1]); return stop() ? CANCELLED : OK;
        };
        if (result == OK) result = reg(0);
        if (result == OK && device == Peripheral::Cc1101) result = reg(1);
        close(fd); return result;
    }
};
#endif
}
DiscoveryReport discover(const ReaderConfig& overrides, DiscoveryProbe& probe, const std::function<bool()>& cancelled,
                         DiscoveryClock::time_point deadline) {
    DiscoveryReport report; report.config = overrides; report.attempted = true;
    const auto stop = [&] { return cancelled() || DiscoveryClock::now() >= deadline; };
    const char* endpoints[]{"/dev/serial0","/dev/i2c-1","/dev/spidev0.1","/dev/spidev0.2"};
    if (stop()) return report;
    const auto environment = probe.environment();
    for (unsigned i = 0; i < report.devices.size(); ++i) {
        auto& item = report.devices[i]; item.endpoint = endpoints[i];
        if (stop()) { item.state = Detection::Cancelled; continue; }
        const auto device = Peripheral(i);
        const auto manual = i == 0 ? overrides.uhf_serial : i == 1 ? overrides.hf_i2c : std::string();
        if (!manual.empty() && manual != endpoints[i] && !(i == 0 && manual == "/dev/ttyS0")) {
            item.state = Detection::Manual; item.endpoint = manual; item.detail = "Explicit endpoint retained."; continue;
        }
        if (i == 0) report.config.uhf_serial.clear();
        if (i == 1) report.config.hf_i2c.clear();
        if (i == 1 && overrides.hf_address != 0x28) {
            item.state = Detection::Unsupported; item.detail = "RFID/RFID2 discovery only uses address 0x28."; continue;
        }
        if (!environment.zero || (i == 0 && !environment.uart) || (i == 1 && !environment.i2c) ||
            (i >= 2 && (!environment.cap_power || !environment.spi_routes))) {
            item.state = Detection::Blocked;
            item.detail = !environment.zero ? "Unknown board: no automatic probes." :
                i == 0 ? "Keep right Grove in UART mode; exit apps using GPIO14/15." :
                i == 1 ? "HF connector requires I2C1 on GPIO2/3." : "Enable Cap power/OS SPI overlay; no pinmux changes made.";
            continue;
        }
        if (i == 0) {
            const auto power = probe.grove_power();
            if (power != OK) {
                item.state = failure(power); item.detail = "Enable Grove output in OS Settings > ExtPort, or install the narrow power permission rule."; continue;
            }
            for (unsigned n = 0; n < 10 && !stop(); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stop()) { item.state = Detection::Cancelled; continue; }
        Bytes bytes;
        const auto result = probe.identify(device,overrides,bytes,stop,deadline);
        if (stop()) { item.state = Detection::Cancelled; continue; }
        if (result == OK && decode(device,bytes,item.identity)) {
            item.state = Detection::Detected;
            if (i == 0) report.config.uhf_serial = manual.empty() ? endpoints[i] : manual;
            if (i == 1) { report.config.hf_i2c = endpoints[i]; report.config.grove_i2c_confirmed = true; }
        } else {
            item.state = result == OK ? Detection::Unsupported : failure(result);
            item.identity.clear(); item.detail = "No recognized module identity; no RF/card command was sent.";
        }
    }
    report.completed = !stop();
    return report;
}
DiscoveryReport discover_linux(const ReaderConfig& overrides, const std::function<bool()>& stop) {
#if defined(__linux__)
    LinuxProbe probe;
    return discover(overrides,probe,stop,DiscoveryClock::now()+std::chrono::seconds(3));
#else
    (void)stop;
    DiscoveryReport report; report.config = overrides; report.attempted = report.completed = true;
    for (auto& item : report.devices) { item.state = Detection::Blocked; item.detail = "Hardware discovery requires the Zero Linux target."; }
    return report;
#endif
}
}
