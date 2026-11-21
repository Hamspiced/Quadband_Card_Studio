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
    DiscoveryEnvironment env{true,true,true,true,true};
    std::array<Status,4> statuses{{OK,OK,OK,OK}};
    std::array<Bytes,4> replies{{uhf_frame(3,{0,'M','1','0','0'}),{0x15},{0,0x14},{0x2a}}};
    std::vector<Peripheral> calls;
    Status power = OK;
    unsigned power_calls = 0, baud = 0;
    bool* cancel_on_reply = nullptr;
    Probe() { replies[0][1] = 1; uint8_t sum = 0; for(size_t i=1;i<replies[0].size()-2;++i)sum+=replies[0][i];replies[0][replies[0].size()-2]=sum; }
    DiscoveryEnvironment environment() override { return env; }
    Status grove_power() override { ++power_calls; return power; }
    Status identify(Peripheral p,const ReaderConfig& config,Bytes& out,const std::function<bool()>& stop,
                    DiscoveryClock::time_point deadline) override {
        assert(!stop() && DiscoveryClock::now() < deadline);
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
    for(const auto& item:all.devices) assert(item.state==Detection::Detected);
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
    Probe unknown; unknown.replies[0].back()^=1; unknown.replies[1]={0xff}; unknown.replies[2]={0,0}; unknown.replies[3]={0xff};
    auto unrecognized=run(unknown);
    for(const auto& item:unrecognized.devices) assert(item.state==Detection::Unsupported);
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
