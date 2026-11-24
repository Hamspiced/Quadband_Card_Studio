#include "platform/discovery.hpp"
#include "platform/wire.hpp"
#include "runtime.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace qb;
using namespace qb::zero;
using namespace qb::zero::platform;
namespace {
struct Probe final : DiscoveryProbe {
    DiscoveryEnvironment env{true,true,true,true,true,true,{}};
    std::array<Status,5> statuses{{OK,OK,OK,OK,OK}};
    std::array<Bytes,5> replies{{uhf_frame(3,{0,'M','1','0','0'}),{0x15},{0,0x14},{0x2a},{2,3,0}}};
    std::vector<std::string> usb;
    unsigned usb_enumerations = 0;
    std::vector<Peripheral> calls;
    Status power = OK;
    unsigned power_calls = 0, baud = 0;
    bool* cancel_on_reply = nullptr;
    Probe() { replies[0][1] = 1; uint8_t sum = 0; for(size_t i=1;i<replies[0].size()-2;++i)sum+=replies[0][i];replies[0][replies[0].size()-2]=sum; }
    DiscoveryEnvironment environment() override { return env; }
    std::vector<std::string> ultra_endpoints(const std::function<bool()>& stop,
                                            DiscoveryClock::time_point deadline) override {
        assert(!stop() && DiscoveryClock::now() < deadline);
        ++usb_enumerations; return usb;
    }
    Status grove_power() override { ++power_calls; return power; }
    Status identify(Peripheral p,const ReaderConfig& config,Bytes& out,const std::function<bool()>& stop,
                    DiscoveryClock::time_point deadline) override {
        assert(!stop() && DiscoveryClock::now() < deadline);
        if (p == Peripheral::Ultra) assert(usb.size() == 1 && config.ultra_serial == usb[0]);
        calls.push_back(p); baud=config.uhf_baud; out=replies[unsigned(p)];
        if(cancel_on_reply)*cancel_on_reply=true;
        return statuses[unsigned(p)];
    }
};
DiscoveryReport run(Probe& probe,const ReaderConfig& config = {}) {
    return discover(config,probe,[] { return false; },DiscoveryClock::now()+std::chrono::seconds(2));
}
}
int main() {
    assert(identity_request(Peripheral::Uhf) == uhf_frame(3,{0}));
    assert(identity_request(Peripheral::Rfid) == Bytes{0x37});
    assert(identity_request(Peripheral::Cc1101) == (Bytes{0xf0,0}));
    assert(identity_request(Peripheral::Cc1101,1) == (Bytes{0xf1,0}));
    assert(identity_request(Peripheral::CapNfc) == (Bytes{0x7f,0}));
    for (unsigned i=0;i<4;++i) assert(identity_request(Peripheral(i),2).empty());
    Probe good; auto all=run(good);
    assert(all.completed && all.config.uhf_serial=="/dev/serial0" && all.config.hf_i2c=="/dev/i2c-1");
    assert(all.config.grove_i2c_confirmed && good.calls.size()==4 && good.power_calls==1);
    for(unsigned i=0;i<4;++i) assert(all.devices[i].state==Detection::Detected);
    assert(all.devices[4].state==Detection::Missing && all.config.ultra_serial.empty());
    assert(all.devices[4].detail.find("Host")!=std::string::npos);
    assert(all.devices[3].identity.find("tools not implemented")!=std::string::npos);
    Probe missing; missing.statuses[1]=IO_ERROR; auto partial=run(missing);
    assert(partial.config.hf_i2c.empty() && !partial.config.uhf_serial.empty());
    Probe permission; permission.power=PERMISSION_DENIED; auto denied=run(permission);
    assert(denied.devices[0].state==Detection::Permission && denied.devices[1].state==Detection::Detected);
    assert(permission.calls.size()==3);
    Probe wrong; wrong.env.zero=false; auto blocked=run(wrong);
    assert(wrong.calls.empty() && wrong.power_calls==0 && blocked.config.hf_i2c.empty());
    Probe routing; routing.env.uart=false; auto no_uart=run(routing);
    assert(routing.power_calls==0 && no_uart.devices[0].state==Detection::Blocked);
    Probe cap; cap.env.cap_power=false; auto no_cap=run(cap);
    assert(cap.calls.size()==2 && no_cap.devices[2].state==Detection::Blocked);
    assert(no_cap.devices[1].state==Detection::Detected);
    Probe power_detail; power_detail.env.cap_power=false; power_detail.env.cap_power_detail="HAT 5V is OFF.";
    auto detailed=run(power_detail);
    assert(detailed.devices[2].detail==power_detail.env.cap_power_detail);
    assert(detailed.devices[3].detail==power_detail.env.cap_power_detail);
    Probe cc_only; cc_only.env.cap_nfc_spi=false; auto cc_route=run(cc_only);
    assert(cc_only.calls.size()==3 && cc_route.devices[2].state==Detection::Detected);
    assert(cc_route.devices[3].state==Detection::Blocked && cc_route.devices[3].detail.find("SPI0.2")!=std::string::npos);
    Probe nfc_only; nfc_only.env.cc1101_spi=false; auto nfc_route=run(nfc_only);
    assert(nfc_only.calls.size()==3 && nfc_route.devices[3].state==Detection::Detected);
    assert(nfc_route.devices[2].state==Detection::Blocked && nfc_route.devices[2].detail.find("SPI0.1")!=std::string::npos);
    Probe unknown; unknown.replies[0].back()^=1; unknown.replies[1]={0xff}; unknown.replies[2]={0,0}; unknown.replies[3]={0xff};
    auto unrecognized=run(unknown);
    for(unsigned i=0;i<4;++i) assert(unrecognized.devices[i].state==Detection::Unsupported);
    Probe overrides; ReaderConfig manual; manual.uhf_serial="/dev/ttyUSB8";manual.hf_i2c="/dev/i2c-9";
    auto kept=run(overrides,manual);
    assert(kept.config.uhf_serial==manual.uhf_serial && kept.config.hf_i2c==manual.hf_i2c);
    assert(overrides.calls.size()==2 && overrides.power_calls==0);
    Probe alias; ReaderConfig serial; serial.uhf_serial="/dev/ttyS0";serial.uhf_baud=9600;
    assert(run(alias,serial).config.uhf_serial=="/dev/ttyS0" && alias.baud==9600);
    Probe interrupted; bool stop=false; interrupted.cancel_on_reply=&stop;
    auto canceled=discover({},interrupted,[&] {return stop;},DiscoveryClock::now()+std::chrono::seconds(2));
    assert(!canceled.completed && canceled.config.uhf_serial.empty() && interrupted.calls.size()==1);
    Probe deadline;
    assert(!discover({},deadline,[]{return false;},DiscoveryClock::now()).completed && deadline.calls.empty());
    Probe busy; busy.statuses[0]=BUSY; assert(run(busy).devices[0].state==Detection::Busy);
    {
        Probe usb; usb.usb={"/dev/ttyACM7"};
        auto found=run(usb);
        assert(found.devices[4].state==Detection::Detected && found.config.ultra_serial==usb.usb[0]);
        assert(found.config.ultra_autodetected && found.devices[4].identity=="Chameleon Ultra 2.3 (USB)");
        assert(usb.calls.size()==5 && usb.calls.back()==Peripheral::Ultra);
        usb.usb.clear(); usb.calls.clear();
        auto unplugged=run(usb,found.config);
        assert(unplugged.config.ultra_serial.empty() && !unplugged.config.ultra_autodetected);
        assert(unplugged.devices[4].state==Detection::Missing && !unplugged.config.uhf_serial.empty());
        usb.usb={"/dev/ttyACM9"};
        auto replugged=run(usb,found.config);
        assert(replugged.config.ultra_serial==usb.usb[0] && replugged.config.ultra_autodetected);
    }
    {
        Probe multiple; multiple.usb={"/dev/ttyACM0","/dev/ttyACM1"};
        auto ambiguous=run(multiple);
        assert(ambiguous.devices[4].state==Detection::Ambiguous && ambiguous.config.ultra_serial.empty());
        assert(std::string(detection_name(ambiguous.devices[4].state))=="Choose endpoint");
        assert(multiple.calls.size()==4);
        ReaderConfig manual; manual.ultra_serial="/dev/serial/by-id/chameleon";
        multiple.calls.clear(); multiple.usb_enumerations=0;
        auto explicit_usb=run(multiple,manual);
        assert(explicit_usb.devices[4].state==Detection::Manual && multiple.usb_enumerations==0);
        assert(explicit_usb.config.ultra_serial==manual.ultra_serial && !explicit_usb.config.ultra_autodetected);
    }
    for (const auto pair : {std::pair{BUSY,Detection::Busy},std::pair{PERMISSION_DENIED,Detection::Permission},
                           std::pair{TIMEOUT,Detection::Missing},std::pair{UNSUPPORTED,Detection::Unsupported}}) {
        Probe usb; usb.usb={"/dev/ttyACM0"}; usb.statuses[4]=pair.first;
        const auto failed=run(usb);
        assert(failed.devices[4].state==pair.second && failed.config.ultra_serial.empty());
        assert(!failed.config.uhf_serial.empty() && !failed.config.hf_i2c.empty());
    }
    for (const auto& reply : {Bytes{1,9,0},Bytes{2,3,1},Bytes{2,3},Bytes{}}) {
        Probe usb; usb.usb={"/dev/ttyACM0"}; usb.replies[4]=reply;
        assert(run(usb).devices[4].state==Detection::Unsupported);
    }
    {
        Probe independent; independent.env.zero=false; independent.usb={"/dev/ttyACM0"};
        const auto usb_only=run(independent);
        assert(usb_only.config.ultra_serial==independent.usb[0] && independent.power_calls==0);
        assert(independent.calls==std::vector<Peripheral>{Peripheral::Ultra});
    }
    {
        std::atomic<bool> entered{false}; unsigned factories=0;
        Runtime runtime(false,[&](const ReaderConfig&,Progress p) {
            ++factories; return make_simulated_port(std::move(p));
        },[&](const ReaderConfig& config,const std::function<bool()>& stopped) {
            entered=true;
            for(unsigned i=0;i<500 && !stopped();++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            DiscoveryReport report; report.config=config; report.attempted=true; return report;
        });
        assert(runtime.detect({}));
        for(unsigned n=0;n<500 && !entered;++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(entered); runtime.stop();
        assert(runtime.wait_idle(2000) && factories==0 && !runtime.snapshot().connected);
    }
    {
        Runtime runtime(true,{},[](const ReaderConfig& config,const std::function<bool()>& stopped) {
            Probe fake; return discover(config,fake,stopped,DiscoveryClock::now()+std::chrono::seconds(2));
        });
        assert(runtime.detect({}) && runtime.wait_idle(2000));
        auto snapshot=runtime.snapshot();
        assert(snapshot.connected && snapshot.discovery.completed && snapshot.discovery.sequence==1);
        assert(snapshot.view.tool==local::Tool::None && !snapshot.view.review && snapshot.view.tag_count==0);
        assert(runtime.detect(snapshot.config) && runtime.wait_idle(2000));
        assert(runtime.snapshot().discovery.sequence==2);
    }
    std::cout<<"Discovery passed: exact known identities/endpoints, no broad probes, partial readers, permissions, routing, manual overrides, cancellation/deadlines and worker auto-connect without card operations.\n";
}
