#include "runtime.hpp"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <iostream>

using namespace qb;
using namespace qb::zero;
using local::Command;
using local::Field;
namespace {
struct Control {
    std::atomic<bool> fail_release{false}, throw_release{false}, interrupt_write{false};
    std::atomic<bool> require_resume{false};
    std::atomic<bool> throw_after_write{false};
    std::atomic<bool> present{true};
    std::atomic<unsigned> connects{0}, writes{0}, generation{0};
    std::function<void()> interrupt;
};
class Probe final : public ReaderPort {
    Control& control;
    std::unique_ptr<ReaderPort> inner;
public:
    Probe(Control& c, Progress p) : control(c), inner(make_simulated_port(std::move(p))) {}
    Status connect() override { ++control.connects; return inner->connect(); }
    Status release() override {
        if (control.throw_release) throw 1;
        if (control.require_resume && inner->cancelled()) return TIMEOUT;
        return control.fail_release ? IO_ERROR : inner->release();
    }
    void request_cancel() override { inner->request_cancel(); }
    void resume() override { inner->resume(); }
    bool cancelled() const override { return inner->cancelled(); }
    uint32_t generation(uint8_t channel) const override { return inner->generation(channel) + control.generation; }
    bool supports(uint8_t channel, uint16_t command) const override { return inner->supports(channel, command); }
    void progress(unsigned n, unsigned total, const char* text) override { inner->progress(n,total,text); }
    void wait_ms(unsigned n) override { inner->wait_ms(n); }
    std::string detail() const override { return inner->detail(); }
    Status identify(local::HfIdentity& card) override { return control.present ? inner->identify(card) : NO_CARD; }
    Status read_classic(const local::HfIdentity& c, unsigned b, const local::ClassicKey& k, std::array<uint8_t,16>& out) override {
        return inner->read_classic(c,b,k,out);
    }
    Status write_classic(const local::HfIdentity& c, unsigned b, const local::ClassicKey& k, const std::array<uint8_t,16>& old,
                         const std::array<uint8_t,16>& desired, std::array<uint8_t,16>& out) override {
        return inner->write_classic(c,b,k,old,desired,out);
    }
    Status read_pages(const local::HfIdentity& c, unsigned b, unsigned n, const local::NtagAuth& a, Bytes& out) override {
        return inner->read_pages(c,b,n,a,out);
    }
    Status write_page(const local::HfIdentity& c, unsigned b, const local::NtagAuth& a, const std::array<uint8_t,4>& old,
                      const std::array<uint8_t,4>& desired, std::array<uint8_t,4>& out) override {
        ++control.writes;
        const auto status = inner->write_page(c,b,a,old,desired,out);
        if (control.interrupt_write) {
            control.interrupt();
            if (control.throw_after_write) throw 1;
            return WRITE_UNVERIFIED;
        }
        return status;
    }
    Status reset_hf() override { return inner->reset_hf(); }
    Status format_ndef(const local::HfIdentity& card,uint8_t flags,const Bytes& snapshot) override {
        return inner->format_ndef(card,flags,snapshot);
    }
    Status transact(uint8_t channel, const Bytes& in, Bytes& out) override { return inner->transact(channel,in,out); }
};
void command(Runtime& r, Command command, uint64_t token = 0) {
    local::Intent intent; intent.command = command; intent.review_token = token;
    assert(r.submit(intent)); assert(r.wait_idle(3000));
}
void field(Runtime& r, Field field, const char* value) {
    local::Intent intent; intent.command = Command::SetField; intent.field = field;
    std::snprintf(intent.text.data(), intent.text.size(), "%s", value);
    assert(r.submit(intent)); assert(r.wait_idle(3000));
}
uint64_t prepare(Runtime& r) {
    command(r,Command::OpenHf); command(r,Command::HfRead);
    assert(r.snapshot().view.hf_ready);
    field(r,Field::HfUnit,"4"); field(r,Field::HfValue,"01020304"); command(r,Command::HfPrepareUnit);
    const auto state = r.snapshot(); assert(state.view.review && state.view.review_token);
    return state.view.review_token;
}
}
int main() {
    {
        Control bulk;
        Runtime runtime(true,[&](const ReaderConfig&,Progress p) { return std::make_unique<Probe>(bulk,std::move(p)); });
        assert(runtime.connect({}) && runtime.wait_idle(3000));
        command(runtime,Command::OpenHf);
        field(runtime,Field::NdefField0,"Runtime bulk");
        field(runtime,Field::BulkFamily,"3");
        command(runtime,Command::HfAddRecord); command(runtime,Command::HfArmBulk);
        assert(runtime.snapshot().view.bulk_active && !runtime.snapshot().pending && !bulk.writes);
        assert(!runtime.connect({}) && !runtime.detect({}));
        command(runtime,Command::HfRead); command(runtime,Command::HfPrepareBulk);
        assert(!runtime.snapshot().view.review && !bulk.writes);
        bulk.present = false; command(runtime,Command::HfRead);
        assert(runtime.snapshot().view.bulk_phase == 2);
        bulk.present = true; command(runtime,Command::HfRead); command(runtime,Command::HfPrepareBulk);
        assert(runtime.snapshot().view.review && !bulk.writes);
        command(runtime,Command::Confirm,runtime.snapshot().view.review_token);
        assert(runtime.snapshot().view.bulk_written == 1 && runtime.snapshot().view.bulk_active);
        auto writes = bulk.writes.load();
        command(runtime,Command::HfRead); command(runtime,Command::HfPrepareBulk);
        assert(!runtime.snapshot().view.review && bulk.writes == writes);
        runtime.stop(); assert(runtime.wait_idle(3000));
        assert(!runtime.snapshot().view.bulk_active && bulk.writes == writes);
        assert(runtime.connect({}) && runtime.wait_idle(3000));
    }
    Control control;
    {
        Runtime runtime(true, [&](const ReaderConfig&, Progress p) { return std::make_unique<Probe>(control,std::move(p)); });
        assert(runtime.connect({}) && runtime.wait_idle(3000) && runtime.snapshot().connected);
        auto token = prepare(runtime); command(runtime,Command::Confirm,token);
        assert(control.writes == 1 && runtime.snapshot().view.status == OK);
        command(runtime,Command::Confirm,token); assert(control.writes == 1);
        token = prepare(runtime); ++control.generation; command(runtime,Command::Confirm,token);
        assert(control.writes == 1 && runtime.snapshot().view.status == STALE_GENERATION);
        token = prepare(runtime);
        field(runtime,Field::HfValue,"05060708"); command(runtime,Command::HfPrepareUnit);
        token = runtime.snapshot().view.review_token;
        control.interrupt = [&] { runtime.stop(); }; control.interrupt_write = true;
        command(runtime,Command::Confirm,token);
        assert(runtime.snapshot().view.status == WRITE_UNVERIFIED && !runtime.snapshot().view.review);
        control.interrupt_write = false;
        command(runtime,Command::ClearSession); assert(runtime.snapshot().view.tool == local::Tool::Hf);
        token = prepare(runtime); control.interrupt_write = control.throw_after_write = true;
        command(runtime,Command::Confirm,token);
        assert(runtime.snapshot().view.status == WRITE_UNVERIFIED && !runtime.snapshot().view.review);
        control.interrupt_write = control.throw_after_write = false;
        command(runtime,Command::HfRead); assert(runtime.snapshot().view.status == WRITE_UNVERIFIED);
        command(runtime,Command::ClearSession); assert(runtime.snapshot().view.status == OK);
        control.fail_release = true; command(runtime,Command::HfRead);
        assert(runtime.snapshot().cleanup_failed && !runtime.snapshot().connected);
        local::Intent intent; intent.command = Command::HfRead; assert(!runtime.submit(intent));
        control.throw_release = true;
    }
    // Stop during adapter construction must not open the replacement endpoints.
    {
        Control retry;
        Runtime runtime(true, [&](const ReaderConfig&, Progress p) { return std::make_unique<Probe>(retry,std::move(p)); });
        assert(runtime.connect({}) && runtime.wait_idle(3000));
        retry.fail_release = true; runtime.stop(); assert(runtime.wait_idle(3000));
        assert(runtime.snapshot().cleanup_failed);
        retry.fail_release = false; retry.require_resume = true;
        assert(runtime.connect({}) && runtime.wait_idle(3000));
        assert(runtime.snapshot().connected && !runtime.snapshot().cleanup_failed && retry.connects == 2);
    }
    {
        std::mutex mutex; std::condition_variable changed; bool entered = false, proceed = false;
        Control slow;
        Runtime runtime(true, [&](const ReaderConfig&, Progress p) {
            std::unique_lock<std::mutex> lock(mutex); entered = true; changed.notify_all();
            changed.wait(lock,[&] { return proceed; });
            return std::make_unique<Probe>(slow,std::move(p));
        });
        assert(runtime.connect({}));
        { std::unique_lock<std::mutex> lock(mutex); changed.wait(lock,[&] { return entered; }); }
        runtime.stop();
        { std::lock_guard<std::mutex> lock(mutex); proceed = true; }
        changed.notify_all();
        assert(runtime.wait_idle(3000) && slow.connects == 0 && !runtime.snapshot().connected);
    }
    std::cout << "Actual worker tests passed: one-shot review, stale generation, partial-write Stop, cleanup quarantine, factory cancellation and throwing shutdown.\n";
}
