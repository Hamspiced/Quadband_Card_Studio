#include "platform/discovery.hpp"
#include "platform/wire.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace qb;
using namespace qb::zero;
using namespace qb::zero::platform;
namespace {
namespace fs = std::filesystem;
struct Sysfs {
    fs::path root = fs::temp_directory_path() /
        ("quadband-usb-" + std::to_string(DiscoveryClock::now().time_since_epoch().count()));
    Sysfs() { assert(fs::create_directory(root)); fs::create_directories(root/"class/tty"); }
    ~Sysfs() { fs::remove_all(root); }
    void node(const std::string& name, const std::string& metadata) {
        auto path = root/"class/tty"/name/"device";
        fs::create_directories(path);
        std::ofstream(path/"uevent") << metadata;
    }
    std::vector<std::string> candidates() { return ultra_usb_endpoints((root/"class/tty").string(),"/mock/dev"); }
};
void metadata_filtering() {
    Sysfs tree;
    const std::string valid = "DEVTYPE=usb_interface\nDRIVER=cdc_acm\nPRODUCT=6868/8686/200\n";
    tree.node("ttyUSB0",valid); tree.node("ttyACM",valid); tree.node("ttyACMbad",valid);
    tree.node("ttyACM0","DRIVER=cdc_acm\nPRODUCT=1915/521f/100\n");
    tree.node("ttyACM1","DRIVER=cdc_acm\nPRODUCT=6868/8687/200\n");
    tree.node("ttyACM2","DRIVER=other\nPRODUCT=6868/8686/200\n");
    tree.node("ttyACM3","DRIVER=cdc_acm\n");
    tree.node("ttyACM4",std::string(2048,'X')+valid);
    assert(tree.candidates().empty());
    for (const auto& product : {"", "6868/8686", "6868/8686/", "6868/8686/200/1",
                                "6868/8686/nothex", "16868/8686/200", "6868/ 8686/200",
                                "6868/0x8686/200", "6868/8686/+200"}) {
        tree.node("ttyACM5","DRIVER=cdc_acm\nPRODUCT="+std::string(product)+"\n");
        assert(tree.candidates().empty());
    }
    tree.node("ttyACM5","DRIVER=cdc_acm\nPRODUCT=\nPRODUCT=6868/8686/200\n");
    assert(tree.candidates().empty());
    tree.node("ttyACM5",valid);
    assert(tree.candidates() == std::vector<std::string>{"/mock/dev/ttyACM5"});
    tree.node("ttyACM6",valid);
    assert(tree.candidates().size() == 2);
    assert(ultra_usb_endpoints((tree.root/"missing").string(),"/mock/dev").empty());
    assert(ultra_usb_endpoints((tree.root/"class/tty").string(),"/mock/dev",[]{return true;}).empty());
    assert(ultra_usb_endpoints((tree.root/"class/tty").string(),"/mock/dev",{},
        DiscoveryClock::now()).empty());
}
void nested_relative_symlinks() {
    Sysfs tree;
    const auto interface = tree.root/"devices/usb1/1-4/1-4:1.0";
    fs::create_directories(interface/"tty/ttyACM7");
    std::ofstream(interface/"uevent") << "DRIVER=cdc_acm\nPRODUCT=6868/8686/203\n";
    fs::create_directory_symlink("../../../1-4:1.0",interface/"tty/ttyACM7/device");
    fs::create_directory_symlink("../../devices/usb1/1-4/1-4:1.0/tty/ttyACM7",
        tree.root/"class/tty/ttyACM7");
    assert(tree.candidates() == std::vector<std::string>{"/mock/dev/ttyACM7"});
}
Bytes reply(unsigned command, const Bytes& payload) {
    auto frame = ultra_frame(command,payload);
    frame[5] = 0x68; frame[8] = uint8_t(frame[8]-0x68);
    return frame;
}
struct IdentityWire final : Wire {
    std::array<Bytes,2> replies{{reply(1000,{2,3}),reply(1033,{0})}};
    Status status = OK;
    unsigned calls = 0;
    bool* cancel_after_reply = nullptr;
    Status open(const ReaderConfig&) override { assert(false); return IO_ERROR; }
    void close() noexcept override {}
    Status read_register(uint8_t,uint8_t&) override { assert(false); return IO_ERROR; }
    Status write_register(uint8_t,uint8_t) override { assert(false); return IO_ERROR; }
    Status exchange(uint8_t channel,const Bytes& request,Bytes& response,unsigned timeout,
                    const Stop& stop) override {
        assert(channel == 3 && calls < 2 && !stop() && timeout > 0 && timeout <= 500);
        assert(request == ultra_frame(calls ? 1033 : 1000));
        response = replies[calls++];
        if (cancel_after_reply) *cancel_after_reply = true;
        return status;
    }
};
void identity_only() {
    const auto deadline = [] { return DiscoveryClock::now()+std::chrono::seconds(1); };
    Bytes data;
    IdentityWire good;
    assert(identify_ultra(good,data,[]{return false;},deadline()) == OK);
    assert(good.calls == 2 && data == (Bytes{2,3,0}));
    IdentityWire wrong_version; wrong_version.replies[0] = reply(1000,{1,9});
    assert(identify_ultra(wrong_version,data,[]{return false;},deadline()) == UNSUPPORTED);
    assert(data.empty() && wrong_version.calls == 1);
    IdentityWire lite; lite.replies[1] = reply(1033,{1});
    assert(identify_ultra(lite,data,[]{return false;},deadline()) == UNSUPPORTED && data.empty());
    IdentityWire malformed; malformed.replies[0] = reply(1000,{2});
    assert(identify_ultra(malformed,data,[]{return false;},deadline()) == UNSUPPORTED && data.empty());
    IdentityWire rejected; rejected.replies[0] = ultra_frame(1000,{2,3});
    assert(identify_ultra(rejected,data,[]{return false;},deadline()) == UNSUPPORTED && data.empty());
    IdentityWire expired;
    assert(identify_ultra(expired,data,[]{return false;},DiscoveryClock::now()) == TIMEOUT);
    assert(expired.calls == 0 && data.empty());
    for (auto status : {BUSY,PERMISSION_DENIED,TIMEOUT,IO_ERROR,BAD_FRAME,CANCELLED}) {
        IdentityWire failed; failed.status = status;
        assert(identify_ultra(failed,data,[]{return false;},deadline()) == status);
        assert(failed.calls == 1 && data.empty());
    }
    IdentityWire canceled; bool stop = true;
    assert(identify_ultra(canceled,data,[&]{return stop;},deadline()) == CANCELLED && canceled.calls == 0);
    stop = false; canceled.cancel_after_reply = &stop;
    assert(identify_ultra(canceled,data,[&]{return stop;},deadline()) == CANCELLED);
    assert(canceled.calls == 1 && data.empty());
}
}
int main() {
    metadata_filtering(); nested_relative_symlinks(); identity_only();
    std::cout << "USB discovery passed: exact application VID/PID/CDC filtering, nested sysfs links, "
        "bounded firmware/model-only queries, unsupported firmware/model, cancellation and deadlines.\n";
}
