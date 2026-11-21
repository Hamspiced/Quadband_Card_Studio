#include "platform/linux_port.hpp"
#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
using namespace qb;
using namespace qb::local;
using namespace qb::zero;
using namespace qb::zero::platform;
namespace {
Bytes uhf_reply(unsigned cmd, const Bytes& data) {
    auto out = uhf_frame(uint8_t(cmd), data); out[1] = 1; ++out[out.size() - 2]; return out;
}
Bytes ultra_reply(unsigned cmd, const Bytes& data = {}, unsigned status = 0x68) {
    auto out = ultra_frame(cmd, data);
    out[4] = uint8_t(status >> 8); out[5] = uint8_t(status);
    uint8_t sum = 0; out[8] = 0;
    for (unsigned i = 0; i < 9; ++i) sum += out[i];
    out[8] = uint8_t(-sum); return out;
}
struct FakeWire : Wire {
    struct Step { unsigned channel; Bytes request, reply; Status status = OK; bool throws = false; unsigned delay_ms = 0; };
    std::deque<Step> script;
    std::vector<unsigned> timeouts;
    std::shared_ptr<unsigned> total = std::make_shared<unsigned>(0), closed = std::make_shared<unsigned>(0);
    unsigned opens = 0, closes = 0, calls = 0, register_reads = 0, register_writes = 0;
    uint8_t version = 0x92;
    std::function<void()> after;
    Status open(const ReaderConfig&) override { ++opens; return OK; }
    void close() noexcept override { ++closes; ++*closed; }
    Status read_register(uint8_t reg, uint8_t& value) override {
        assert(reg == 0x37); ++register_reads; value = version; return OK;
    }
    Status write_register(uint8_t, uint8_t) override { ++register_writes; assert(false); return IO_ERROR; }
    Status exchange(uint8_t channel, const Bytes& req, Bytes& out, unsigned timeout, const Stop& stop) override {
        assert(!stop()); assert(timeout && timeout <= 20000); assert(!script.empty());
        auto step = script.front(); script.pop_front(); ++calls; ++*total; timeouts.push_back(timeout);
        assert(channel == step.channel); assert(req == step.request);
        if (step.delay_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(step.delay_ms, timeout)));
            if (step.delay_ms >= timeout) return TIMEOUT;
        }
        out = step.reply;
        if (after) { auto fn = std::move(after); after = {}; fn(); }
        if (step.throws) throw std::runtime_error("simulated post-send exception");
        return step.status;
    }
    void uhf(unsigned cmd, const Bytes& request, const Bytes& reply, Status status = OK) {
        script.push_back({2, uhf_frame(uint8_t(cmd), request), uhf_reply(cmd, reply), status});
    }
    void ultra(unsigned cmd, const Bytes& request, const Bytes& reply, Status status = OK) {
        script.push_back({3, ultra_frame(cmd, request), ultra_reply(cmd, reply), status});
    }
    void handshake(const Bytes& capabilities = {3, 0xe9, 3, 0xea, 0x0b, 0xb8, 0x0b, 0xc8}, unsigned mode = 0) {
        ultra(1000, {}, {2, 0}); ultra(1033, {}, {0}); ultra(1035, {}, capabilities); ultra(1002, {}, {uint8_t(mode)});
    }
};
struct FakeRadio : Radio {
    HfIdentity card;
    std::map<unsigned, Bytes> data;
    unsigned scans = 0, writes = 0, releases = 0;
    bool throw_write = false, bad_verify = false, cleanup_fails = false;
    bool enforce_keys = false, ntag_memory = false;
    std::vector<std::array<uint8_t,4>> pages;
    std::vector<unsigned> written;
    unsigned cleanup_delay_ms = 0;
    std::function<void()> before_write, after_write, after_read;
    FakeRadio() {
        card.family = HfFamily::Classic1K; card.uid_size = 4; card.uid[0] = 1; card.atqa = {4, 0}; card.sak = 8;
        data[4] = Bytes(16, 0); data[7] = Bytes(16, 0);
        data[7][6] = 0xff; data[7][7] = 7; data[7][8] = 0x80;
    }
    Status initialize() override { return OK; }
    Status identify(HfIdentity& out) override { ++scans; out = card; return OK; }
    Status authenticate(const HfIdentity&, unsigned block, const ClassicKey& key) override {
        if (enforce_keys && !std::equal(key.bytes.begin(),key.bytes.end(),data[trailer(block)].begin())) return AUTH_FAILED;
        return OK;
    }
    Status authenticate(const NtagAuth& auth, bool& good) override { good = auth.mode != 0; return OK; }
    Status read(unsigned block, Bytes& out) override {
        if (ntag_memory) {
            out.clear();
            for (unsigned i = 0; i < 4; ++i) {
                const auto& page = pages[(block+i)%pages.size()];
                out.insert(out.end(),page.begin(),page.end());
            }
            return OK;
        }
        auto found = data.find(block); assert(found != data.end()); out = found->second;
        if (after_read) { auto fn = std::move(after_read); after_read = {}; fn(); }
        return OK;
    }
    Status write(unsigned block, const Bytes& in) override {
        if (before_write) before_write();
        ++writes;
        written.push_back(block);
        if (!bad_verify) {
            if (ntag_memory) std::copy(in.begin(),in.end(),pages[block].begin());
            else std::copy(in.begin(), in.end(), data[block].begin());
        }
        if (after_write) after_write();
        if (throw_write) throw std::runtime_error("mutation happened");
        return OK;
    }
    Status release(Deadline deadline) override {
        ++releases;
        if (cleanup_delay_ms) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) return TIMEOUT;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(cleanup_delay_ms, remaining)));
            if (cleanup_delay_ms >= remaining) return TIMEOUT;
        }
        return cleanup_fails ? IO_ERROR : OK;
    }
};
struct Fixture {
    FakeWire* wire;
    FakeRadio* radio;
    std::unique_ptr<ReaderPort> port;
    explicit Fixture(ReaderConfig config) {
        auto w = std::make_unique<FakeWire>(); wire = w.get();
        auto r = std::make_unique<FakeRadio>(); radio = r.get();
        port = make_port(config, {}, std::move(w), std::move(r));
    }
};
ReaderConfig hf_config() { ReaderConfig c; c.hf_i2c = "/fake/i2c"; c.grove_i2c_confirmed = true; return c; }
ReaderConfig uhf_config() { ReaderConfig c; c.uhf_serial = "/fake/uhf"; return c; }
ReaderConfig ultra_config() { ReaderConfig c; c.ultra_serial = "/fake/ultra"; return c; }
void configuration_and_cancellation() {
    Fixture empty({}); assert(empty.port->connect() == INVALID_ARGUMENT); assert(!empty.wire->opens);
    auto c = hf_config(); c.grove_i2c_confirmed = false;
    Fixture no_grove(c); assert(no_grove.port->connect() == PERMISSION_DENIED); assert(!no_grove.wire->opens);
    assert(no_grove.port->detail().find("chosen HF I2C connector") != std::string::npos);
    c = hf_config(); c.hf_address = 0x24;
    Fixture bad_address(c); assert(bad_address.port->connect() == PERMISSION_DENIED);
    c = uhf_config(); c.ultra_serial = c.uhf_serial;
    Fixture duplicate(c); assert(duplicate.port->connect() == INVALID_ARGUMENT); assert(!duplicate.wire->opens);
    Fixture f(hf_config()); assert(f.port->connect() == OK); assert(f.radio->scans == 0);
    const auto generation = f.port->generation(1); assert(generation);
    std::thread cancel([&] { for (unsigned i = 0; i < 10000; ++i) f.port->request_cancel(); });
    for (unsigned i = 0; i < 10000; ++i) {
        (void)f.port->cancelled(); assert(f.port->generation(1) == generation);
    }
    cancel.join();
    assert(f.port->cancelled()); assert(f.port->generation(1) == generation);
    HfIdentity out; assert(f.port->identify(out) == CANCELLED); assert(f.port->release() == OK);
    assert(f.port->generation(1) == generation); f.port->resume(); assert(!f.port->cancelled());
    assert(f.port->connect() == OK); assert(f.port->generation(1) != generation);
#if !defined(__linux__)
    auto host = make_linux_port(hf_config(), {});
    assert(host->connect() == UNSUPPORTED && !host->generation(1));
#endif
}
void hf_classic_guards() {
    ClassicKey key;
    std::array<uint8_t, 16> old{}, desired{}, verified{}; desired.fill(0xa5);
    Fixture f(hf_config()); assert(f.port->connect() == OK); auto card = f.radio->card;
    for (unsigned protected_block : {0u, 3u, 7u, 63u, 64u, 65535u})
        assert(f.port->write_classic(card, protected_block, key, old, desired, verified) == PERMISSION_DENIED);
    assert(f.radio->writes == 0 && f.radio->scans == 0);
    card.uid[0] ^= 1;
    assert(f.port->write_classic(card, 4, key, old, desired, verified) == STALE_DATA);
    card = f.radio->card; f.radio->data[4][0] = 1;
    assert(f.port->write_classic(card, 4, key, old, desired, verified) == STALE_DATA);
    f.radio->data[4][0] = 0; f.radio->data[7][6] = 0;
    assert(f.port->write_classic(card, 4, key, old, desired, verified) == PERMISSION_DENIED);
    f.radio->data[7][6] = 0xff;
    assert(f.port->write_classic(card, 4, key, old, desired, verified) == OK);
    assert(verified == desired && f.radio->writes == 1);
    f.radio->bad_verify = true;
    assert(f.port->write_classic(card, 4, key, desired, old, verified) == WRITE_UNVERIFIED);
    assert(f.radio->writes == 2);
    f.radio->bad_verify = false; f.radio->throw_write = true;
    assert(f.port->write_classic(card, 4, key, desired, old, verified) == WRITE_UNVERIFIED);
    assert(f.radio->writes == 3 && !f.port->generation(1));
    Fixture stopped(hf_config()); assert(stopped.port->connect() == OK);
    stopped.radio->after_read = [&] { stopped.port->request_cancel(); };
    assert(stopped.port->write_classic(stopped.radio->card, 4, key, old, desired, verified) == CANCELLED);
    assert(!stopped.radio->writes);
    stopped.port->resume(); stopped.radio->after_write = [&] { stopped.port->request_cancel(); };
    assert(stopped.port->write_classic(stopped.radio->card, 4, key, old, desired, verified) == WRITE_UNVERIFIED);
    assert(stopped.radio->writes == 1);
    Fixture cleanup(hf_config()); assert(cleanup.port->connect() == OK);
    cleanup.radio->cleanup_fails = true;
    assert(cleanup.port->write_classic(cleanup.radio->card, 4, key, old, desired, verified) == WRITE_UNVERIFIED);
    assert(cleanup.radio->writes == 1 && !cleanup.port->supports(1, 18));
}
void hf_ntag_guards() {
    Fixture f(hf_config()); assert(f.port->connect() == OK);
    auto& card = f.radio->card;
    card.family = HfFamily::Ntag213; card.uid_size = 7; card.atqa = {0x44, 0}; card.sak = 0;
    card.version = {0, 4, 4, 2, 1, 0, 0x0f, 3};
    f.radio->data[0] = Bytes(16, 0); f.radio->data[40] = Bytes(16, 0); f.radio->data[41] = Bytes(16, 0);
    f.radio->data[0][12] = 0xe1; f.radio->data[0][13] = 0x10; f.radio->data[0][14] = 18;
    f.radio->data[41][3] = 0xff;
    NtagAuth auth; std::array<uint8_t, 4> old{}, desired{{1, 2, 3, 4}}, verified{};
    for (unsigned protected_page : {0u, 1u, 2u, 3u, 40u, 41u, 44u, 65535u})
        assert(f.port->write_page(card, protected_page, auth, old, desired, verified) == PERMISSION_DENIED);
    f.radio->data[0][10] = 0xff;
    assert(f.port->write_page(card, 4, auth, old, desired, verified) == PERMISSION_DENIED);
    f.radio->data[0][10] = 0;
    assert(f.port->write_page(card, 4, auth, old, desired, verified) == OK);
    assert(std::equal(desired.begin(), desired.end(), f.radio->data[4].begin()));
    Bytes output; assert(f.port->read_pages(card, 0xfffffff0, 16, auth, output) == INVALID_ARGUMENT);
    assert(f.port->read_pages(card, 44, 2, auth, output) == INVALID_ARGUMENT);
    auto expected = card; card.version[0] = 1;
    assert(f.port->read_pages(expected, 4, 1, auth, output) == STALE_DATA);
}
void canonical_operations() {
    auto factory = [](Fixture& f) {
        f.radio->enforce_keys = true;
        for (unsigned b = 0; b < 64; ++b) {
            f.radio->data[b] = Bytes(16,0);
            if (b%4 == 3) {
                std::fill(f.radio->data[b].begin(),f.radio->data[b].end(),255);
                f.radio->data[b][7] = 7; f.radio->data[b][8] = 0x80; f.radio->data[b][9] = 0x69;
            }
        }
    };
    auto classic_snapshot = [](Fixture& f) {
        Bytes bytes;
        for (unsigned b = 0; b < 64; ++b) bytes.insert(bytes.end(),f.radio->data[b].begin(),f.radio->data[b].end());
        return bytes;
    };
    Fixture f(hf_config()); assert(f.port->connect() == OK); factory(f);
    assert(f.port->supports(1,22) && f.port->supports(1,23));
    auto expected = classic_snapshot(f);
    f.radio->data[63][10] = 0;
    assert(f.port->format_ndef(f.radio->card,1,classic_snapshot(f)) == PERMISSION_DENIED && !f.radio->writes);
    factory(f);
    assert(f.port->format_ndef(f.radio->card,0,expected) == OK);
    assert(f.radio->written.back() == 3 && f.radio->data[3][9] == 0xc1 && f.radio->data[7][9] == 0x40);
    assert(std::find(f.radio->written.begin(),f.radio->written.end(),0) == f.radio->written.end());
    Fixture cancel(hf_config()); assert(cancel.port->connect() == OK); factory(cancel);
    cancel.radio->after_write = [&] { cancel.port->request_cancel(); };
    assert(cancel.port->format_ndef(cancel.radio->card,0,classic_snapshot(cancel)) == WRITE_UNVERIFIED);
    assert(cancel.radio->writes == 1);
    Fixture throws(hf_config()); assert(throws.port->connect() == OK); factory(throws);
    throws.radio->throw_write = true;
    assert(throws.port->format_ndef(throws.radio->card,0,classic_snapshot(throws)) == WRITE_UNVERIFIED && throws.radio->writes == 1);
    for (auto family : {HfFamily::Ntag213,HfFamily::Ntag215,HfFamily::Ntag216}) {
        Fixture tag(hf_config()); assert(tag.port->connect() == OK);
        auto& card = tag.radio->card; card.family = family; card.uid_size = 7; card.sak = 0; card.atqa = {{0x44,0}};
        card.version = {{0,4,4,2,1,0,uint8_t(family == HfFamily::Ntag213 ? 15 : family == HfFamily::Ntag215 ? 17 : 19),3}};
        tag.radio->ntag_memory = true; tag.radio->pages.resize(card.units()); tag.radio->pages[card.units()-4][3] = 255;
        unsigned cc = family == HfFamily::Ntag213 ? 18 : family == HfFamily::Ntag215 ? 62 : 109;
        for (unsigned b = 4+cc*2; b < card.units()-5; ++b) tag.radio->pages[b][0] = 0x5a;
        auto original = tag.radio->pages;
        Bytes snapshot;
        NtagAuth auth;
        for (unsigned b = 0; b < card.units()-2; b += 16) {
            Bytes chunk; unsigned count = std::min(16u,card.units()-2-b);
            assert(tag.port->read_pages(card,b,count,auth,chunk) == OK && chunk.size() == count*4);
            snapshot.insert(snapshot.end(),chunk.begin(),chunk.end());
        }
        assert(tag.port->format_ndef(card,1,snapshot) == OK);
        assert((tag.radio->pages[3] == std::array<uint8_t,4>{{0xe1,0x10,uint8_t(cc),0}}));
        for (unsigned b : tag.radio->written) assert(b == 3 || (b >= 4 && b < 4+cc*2));
        for (unsigned b = 4+cc*2; b < card.units(); ++b) assert(tag.radio->pages[b] == original[b]);
    }
}
void module_identity_and_release() {
    for (unsigned version : {0x91u, 0x92u, 0x15u, 0x90u, 0x88u, 0u}) {
        auto wire = std::make_unique<FakeWire>(); auto* w = wire.get(); w->version = uint8_t(version);
        auto port = make_port(hf_config(), {}, std::move(wire));
        assert(port->connect() == (version == 0x91 || version == 0x92 || version == 0x15 ? OK : UNSUPPORTED));
        assert(w->register_reads == 1 && w->register_writes == 0);
    }
    Fixture unknown(uhf_config()); unknown.wire->uhf(3, {0}, {0, 'O', 'T', 'H', 'E', 'R'});
    assert(unknown.port->connect() == UNSUPPORTED); assert(!unknown.port->generation(2));
    Fixture uhf(uhf_config()); uhf.wire->uhf(3, {0}, {0, 'M', '1', '0', '0'});
    assert(uhf.port->connect() == OK); assert(uhf.wire->calls == 1);
    Bytes response;
    assert(uhf.port->transact(2, uhf_frame(0x82), response) == PERMISSION_DENIED);
    uhf.wire->uhf(0x22, {}, {}, NO_CARD);
    assert(uhf.port->transact(2, uhf_frame(0x22), response) == NO_CARD);
    uhf.port->request_cancel(); auto generation = uhf.port->generation(2);
    uhf.wire->uhf(0x28, {}, {0});
    assert(uhf.port->release() == OK); assert(uhf.port->generation(2) == generation);
    uhf.port->resume(); uhf.wire->uhf(0x22, {}, {});
    assert(uhf.port->transact(2, uhf_frame(0x22), response) == OK);
    uhf.wire->uhf(0x28, {}, {1});
    assert(uhf.port->release() == IO_ERROR); assert(!uhf.port->supports(2, 0x22));
}
void ultra_negotiation_cleanup() {
    Bytes response;
    Fixture f(ultra_config()); f.wire->handshake();
    assert(f.port->connect() == OK); assert(f.wire->calls == 4);
    assert(f.port->supports(3, 3000) && f.port->supports(3, 3016));
    assert(!f.port->supports(3, 3002) && !f.port->supports(3, 2009));
    f.wire->ultra(1001, {1}, {});
    assert(f.port->transact(3, ultra_frame(1001, {1}), response) == OK);
    f.port->request_cancel(); auto generation = f.port->generation(3);
    f.wire->ultra(1001, {0}, {}); f.wire->ultra(1002, {}, {0});
    assert(f.port->release() == OK); assert(f.port->generation(3) == generation);
    f.port->resume();
    f.wire->ultra(1001, {1}, {}); assert(f.port->transact(3, ultra_frame(1001, {1}), response) == OK);
    f.wire->ultra(1001, {0}, {}); f.wire->ultra(1002, {}, {1});
    assert(f.port->release() == IO_ERROR); assert(!f.port->supports(3, 3000));
    Fixture missing(ultra_config());
    missing.wire->ultra(1000, {}, {2, 0}); missing.wire->ultra(1033, {}, {0});
    missing.wire->ultra(1035, {}, {3, 0xe9, 0x0b, 0xb8});
    assert(missing.port->connect() == UNSUPPORTED); assert(!missing.port->generation(3));
    Fixture unknown(ultra_config()); unknown.wire->ultra(1000, {}, {1, 0});
    assert(unknown.port->connect() == UNSUPPORTED);
    Fixture interrupted(ultra_config()); interrupted.wire->handshake({}, 1);
    // An empty capabilities list is malformed and never attaches.
    assert(interrupted.port->connect() == BAD_FRAME);
    interrupted.wire->script.clear();
    Fixture mutation(ultra_config()); mutation.wire->handshake();
    assert(mutation.port->connect() == OK);
    Bytes payload{1, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0};
    mutation.wire->ultra(3016, payload, {}); mutation.wire->script.back().throws = true;
    assert(mutation.port->transact(3, ultra_frame(3016, payload), response) == WRITE_UNVERIFIED);
    assert(mutation.wire->calls == 5);
    Fixture independent(ultra_config()); independent.wire->handshake({3, 0xe9, 3, 0xea, 0x0b, 0xb8}, 1);
    assert(independent.port->connect() == OK);
    independent.wire->ultra(1001, {0}, {});
    assert(independent.port->transact(3, ultra_frame(1001, {0}), response) == OK);
    independent.radio->cleanup_fails = true;
    independent.wire->ultra(1001, {1}, {}); independent.wire->ultra(1002, {}, {1});
    assert(independent.port->release() == IO_ERROR);
    assert(independent.wire->script.empty() && !independent.port->generation(3));
}
void uhf_selection_preflight() {
    Fixture f(uhf_config()); f.wire->uhf(3, {0}, {0, 'M', '1', '0', '0'}); assert(f.port->connect() == OK);
    Bytes out, selection{1, 0, 0, 0, 16, 32, 0, 8, 0, 0xaa, 0xbb};
    Bytes write{0, 0, 0, 0, 3, 0, 0, 0, 1, 0xca, 0xfe};
    assert(f.port->transact(2, uhf_frame(0x49, write), out) == PERMISSION_DENIED);
    auto select = [&] {
        f.wire->uhf(0x0c, selection, {0}); f.wire->uhf(0x0b, {}, selection); f.wire->uhf(0x12, {0}, {0});
        assert(f.port->transact(2, uhf_frame(0x0c, selection), out) == OK);
        assert(f.port->transact(2, uhf_frame(0x0b), out) == OK);
        assert(f.port->transact(2, uhf_frame(0x12, {0}), out) == OK);
    };
    select();
    f.wire->uhf(0x0b, {}, selection);
    f.wire->uhf(0x39, {0, 0, 0, 0, 1, 0, 1, 0, 1}, {4, 8, 0, 0xaa, 0xbc, 8, 0});
    assert(f.port->transact(2, uhf_frame(0x49, write), out) == STALE_DATA);
    select();
    f.wire->uhf(0x0b, {}, selection);
    f.wire->uhf(0x39, {0, 0, 0, 0, 1, 0, 1, 0, 1}, {4, 8, 0, 0xaa, 0xbb, 8, 0});
    f.wire->uhf(0x49, write, {}, TIMEOUT);
    assert(f.port->transact(2, uhf_frame(0x49, write), out) == WRITE_UNVERIFIED);
    assert(f.wire->script.empty()); assert(!f.port->supports(2, 0x49));
}
void bounded_cleanup() {
    auto config = uhf_config(); config.ultra_serial = "/fake/ultra";
    Fixture f(config);
    f.wire->uhf(3, {0}, {0, 'M', '1', '0', '0'});
    f.wire->handshake({3, 0xe9, 3, 0xea, 0x0b, 0xb8}, 1);
    assert(f.port->connect() == OK);
    Bytes response;
    f.wire->uhf(0x22, {}, {});
    assert(f.port->transact(2, uhf_frame(0x22), response) == OK);
    f.wire->ultra(1001, {0}, {});
    assert(f.port->transact(3, ultra_frame(1001, {0}), response) == OK);
    f.radio->cleanup_delay_ms = 300;
    f.wire->uhf(0x28, {}, {0}); f.wire->script.back().delay_ms = 500;
    f.wire->ultra(1001, {1}, {}); f.wire->script.back().delay_ms = 600;
    f.wire->ultra(1002, {}, {1}); f.wire->script.back().delay_ms = 1600;
    f.port->request_cancel();
    std::thread repeated_stop([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); f.port->request_cancel();
    });
    auto start = Clock::now();
    assert(f.port->release() != OK);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    repeated_stop.join();
    assert(elapsed >= 1900 && elapsed < 2800);
    assert(!f.port->generation(2) && !f.port->supports(3, 1001));
    const auto n = f.wire->timeouts.size();
    assert(f.wire->timeouts[n - 3] <= 1700);
    assert(f.wire->timeouts[n - 2] <= 1200);
    assert(f.wire->timeouts[n - 1] <= 600);
    // Repeated release while still cancelled does not renew the first Stop's budget.
    f.radio->cleanup_delay_ms = 0;
    auto before = f.wire->calls;
    assert(f.port->release() != OK && f.wire->calls == before);
    // An explicit worker-owned resume/reconnect attempt can retry retained original mode.
    f.port->resume();
    f.wire->ultra(1001, {1}, {}); f.wire->ultra(1002, {}, {1});
    assert(f.port->release() == OK); assert(f.wire->script.empty());
    std::cout << "bounded cleanup across HF/UHF/Ultra: " << elapsed << "ms, failed closed; original-mode retry verified\n";
    Fixture failed(ultra_config()); failed.wire->handshake(); assert(failed.port->connect() == OK);
    failed.wire->ultra(1001, {1}, {});
    assert(failed.port->transact(3, ultra_frame(1001, {1}), response) == OK);
    failed.wire->ultra(1001, {0}, {}, TIMEOUT);
    assert(failed.port->release() != OK);
    auto total = failed.wire->total, closed = failed.wire->closed;
    auto calls = *total, closes = *closed;
    failed.port.reset();
    assert(*total == calls && *closed == closes + 1);
}
}
int main() {
    configuration_and_cancellation(); hf_classic_guards(); hf_ntag_guards();
    canonical_operations();
    module_identity_and_release(); ultra_negotiation_cleanup(); uhf_selection_preflight(); bounded_cleanup();
    std::cout << "platform_port_test: configuration, cancellation/generation, HF guards, UHF preflight, Ultra negotiation/cleanup passed\n";
}
