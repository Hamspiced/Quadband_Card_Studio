#include "../main/standalone/uhf.hpp"
#include <cassert>
#include <cstdio>
#include <functional>

using namespace qb;
using namespace qb::local;
namespace {
Bytes hex(const char* text) {
    Bytes b;
    while (*text) {
        unsigned v = 0; assert(std::sscanf(text, "%2x", &v) == 1);
        b.push_back(uint8_t(v)); text += 2;
    }
    return b;
}
Bytes packet(uint8_t cmd, const Bytes& p, uint8_t kind = 1) {
    Bytes b{0xbb, kind, cmd, uint8_t(p.size() >> 8), uint8_t(p.size())};
    b.insert(b.end(), p.begin(), p.end());
    unsigned sum = 0;
    for (size_t i = 1; i < b.size(); ++i) sum += b[i];
    b.push_back(uint8_t(sum)); b.push_back(0x7e); return b;
}
uint16_t crc(const Bytes& b) {
    uint16_t c = 0xffff;
    for (uint8_t v : b) {
        c ^= uint16_t(v) << 8;
        for (unsigned i = 0; i < 8; ++i)
            c = uint16_t((unsigned(c) << 1) ^ ((c & 0x8000) ? 0x1021 : 0));
    }
    return c ^ 0xffff;
}
Bytes report(const Bytes& epc, std::array<uint8_t, 2> pc) {
    Bytes b(pc.begin(), pc.end()); b.insert(b.end(), epc.begin(), epc.end());
    uint16_t c = crc(b); b.insert(b.begin(), 0xc9);
    b.push_back(uint8_t(c >> 8)); b.push_back(uint8_t(c)); return packet(0x22, b, 2);
}
struct Mock : Port {
    uint32_t gen = 7;
    bool stop = false, capable = true, no_tags = false, bad_filter = false, wrong_tag = false;
    bool ignore_write = false, cancel_write = false, unplug_write = false, new_identity_reply = false;
    bool partial_write = false, bad_ack = false, ignore_power = false;
    uint8_t corrupt_command = 0;
    Status transport = OK;
    uint8_t error = 0, write_error = 0;
    unsigned writes = 0, calls = 0, power_writes = 0;
    Bytes epc = hex("30751FEB705C5904E3D50D70"), filter, last_write;
    std::array<uint8_t, 2> pc{0x34, 0};
    std::array<uint8_t, 512> user{};
    uint16_t rf = 2000;
    std::function<void(uint8_t)> hook;
    bool cancelled() const override { return stop; }
    uint32_t generation(uint8_t c) const override { assert(c == 2); return gen; }
    bool supports(uint8_t c, uint16_t) const override { assert(c == 2); return capable; }
    void progress(unsigned, unsigned, const char*) override {}
    void wait_ms(unsigned) override { assert(false); }
    Status identify(HfIdentity&) override { return UNSUPPORTED; }
    Status read_classic(const HfIdentity&, unsigned, const ClassicKey&, std::array<uint8_t, 16>&) override { return UNSUPPORTED; }
    Status write_classic(const HfIdentity&, unsigned, const ClassicKey&, const std::array<uint8_t, 16>&,
                         const std::array<uint8_t, 16>&, std::array<uint8_t, 16>&) override { return UNSUPPORTED; }
    Status read_pages(const HfIdentity&, unsigned, unsigned, const NtagAuth&, Bytes&) override { return UNSUPPORTED; }
    Status write_page(const HfIdentity&, unsigned, const NtagAuth&, const std::array<uint8_t, 4>&,
                      const std::array<uint8_t, 4>&, std::array<uint8_t, 4>&) override { return UNSUPPORTED; }
    Status reset_hf() override { return UNSUPPORTED; }
    Bytes identity() const {
        Bytes b{uint8_t(epc.size() + 2), pc[0], pc[1]};
        b.insert(b.end(), epc.begin(), epc.end()); return b;
    }
    Status transact(uint8_t channel, const Bytes& request, Bytes& response) override {
        assert(channel == 2); ++calls;
        const uint8_t cmd = request[2];
        const Bytes p(request.begin() + 5, request.end() - 2);
        assert(request == packet(cmd, p, 0));
        assert(validate_uhf(request) == OK);
        if (hook) hook(cmd);
        if (transport != OK) return transport;
        if (error) { response = packet(0xff, {error}); return OK; }
        Bytes data{0};
        switch (cmd) {
        case 0x12: assert(p == Bytes{0} || p == Bytes{1}); break;
        case 0x28: assert(p.empty()); break;
        case 0x22:
            response = no_tags ? packet(0xff, {0x15}) : report(epc, pc); return OK;
        case 0x0c: filter = p; break;
        case 0x0b: data = filter; if (bad_filter && !data.empty()) data.back() ^= 1; break;
        case 0x39: {
            assert(p.size() == 9);
            const unsigned bank = p[4], word = unsigned(p[5]) * 256 + p[6];
            const unsigned count = unsigned(p[7]) * 256 + p[8];
            Bytes memory;
            if (bank == 1) {
                memory = {0, 0, pc[0], pc[1]}; memory.insert(memory.end(), epc.begin(), epc.end());
                memory.resize(512);
            } else memory.assign(user.begin(), user.end());
            data = identity();
            data.insert(data.end(), memory.begin() + word * 2, memory.begin() + (word + count) * 2);
            if (wrong_tag) data[3] ^= 1;
            break;
        }
        case 0x49: {
            ++writes; last_write = p;
            if (write_error) { response = packet(0xff, {write_error}); return OK; }
            data = identity(); data.push_back(0);
            if (!ignore_write) {
                if (p[4] == 1) {
                    pc = {p[9], p[10]}; epc.assign(p.begin() + 11, p.end());
                } else {
                    const size_t offset = (unsigned(p[5]) * 256 + p[6]) * 2;
                    std::copy(p.begin() + 9, partial_write ? p.begin() + 10 : p.end(), user.begin() + offset);
                }
            }
            if (new_identity_reply) { data = identity(); data.push_back(0); }
            if (cancel_write) stop = true;
            if (unplug_write) ++gen;
            break;
        }
        case 0xb7: data = {uint8_t(rf >> 8), uint8_t(rf)}; break;
        case 0xb6: ++power_writes; if (!ignore_power) rf = uint16_t(unsigned(p[0]) * 256 + p[1]); break;
        default: assert(false);
        }
        if (bad_ack && (cmd == 0x12 || cmd == 0x0c || cmd == 0xb6)) data = {1};
        response = packet(cmd, data);
        if (cmd == corrupt_command) response[response.size() - 2] ^= 1;
        return OK;
    }
};
UhfTag scan(Mock& m, Uhf& u) {
    assert(u.inventory_start()); assert(u.inventory_poll()); assert(u.tag_count() == 1);
    assert(u.tags()[0].rssi == -55 && u.tags()[0].pc == m.pc);
    UhfTag tag = u.tags()[0]; assert(u.inventory_stop()); return tag;
}
void vendor_and_stream() {
    const Bytes vendor = hex("BB02220011C9340030751FEB705C5904E3D50D703A76EF7E");
    for (size_t split = 1; split < vendor.size(); ++split) {
        Mock m; Uhf u(m); assert(u.inventory_start());
        assert(u.feed(vendor.data(), split)); assert(u.tag_count() == 0);
        assert(u.feed(vendor.data() + split, vendor.size() - split));
        assert(u.tag_count() == 1 && u.tags()[0].reads == 1);
        assert(u.tags()[0].rssi == -55 && u.tags()[0].epc_size == 12);
        assert(u.feed(vendor.data(), vendor.size())); assert(u.tags()[0].reads == 2);
        assert(u.inventory_stop());
    }
    Mock m; Uhf u(m); assert(u.inventory_start());
    Bytes both = vendor; both.insert(both.end(), vendor.begin(), vendor.end());
    assert(u.feed(both.data(), both.size())); assert(u.tags()[0].reads == 2);
    Bytes bad = vendor; bad[bad.size() - 2] ^= 1;
    assert(u.feed(bad.data(), bad.size()).status == BAD_FRAME);
    bad = vendor; bad[8] ^= 1;
    unsigned sum = 0; for (size_t i = 1; i < bad.size() - 2; ++i) sum += bad[i];
    bad[bad.size() - 2] = uint8_t(sum);
    assert(u.feed(bad.data(), bad.size()).status == BAD_FRAME);
    const uint8_t oversized[]{0xbb, 2, 0x22, 4, 1};
    assert(u.feed(oversized, sizeof(oversized)).status == BAD_FRAME);
    const uint8_t junk = 0xa0;
    assert(u.feed(&junk, 1).status == BAD_FRAME);
    assert(u.inventory_stop());
    assert(u.feed(vendor.data(), vendor.size()).status == BUSY);
}
void capacity_and_long_epc() {
    Mock m; Uhf u(m); assert(u.inventory_start());
    for (unsigned i = 0; i < 65; ++i) {
        Bytes b(62, uint8_t(i)); auto p = report(b, {0xf8, 0});
        assert(u.feed(p.data(), p.size()));
    }
    assert(u.tag_count() == 64 && u.dropped_reports() == 1);
    auto tag = u.tags()[0]; assert(tag.epc_size == 62); assert(u.inventory_stop());
    UhfMemory out; UhfPassword pw;
    const unsigned before = m.calls;
    assert(u.read(tag, UhfBank::User, 0, 1, pw, out).status == INVALID_ARGUMENT);
    assert(m.calls == before);
    u.clear(); assert(u.tag_count() == 0);
    for (auto v : u.tags()[0].epc) assert(v == 0);
}
void read_and_errors() {
    Mock m; Uhf u(m); const auto tag = scan(m, u); UhfPassword pw; UhfMemory out;
    for (auto bank : {UhfBank::Reserved, UhfBank::Epc, UhfBank::Tid, UhfBank::User}) {
        assert(u.read(tag, bank, 0, 64, pw, out)); assert(out.valid && out.words == 64);
    }
    unsigned before = m.calls;
    assert(u.read(tag, UhfBank::User, 0xffff, 2, pw, out).status == INVALID_ARGUMENT);
    assert(u.read(tag, UhfBank::User, 0, 65, pw, out).status == INVALID_ARGUMENT);
    assert(u.read(tag, UhfBank::User, 0, 0, pw, out).status == INVALID_ARGUMENT);
    assert(m.calls == before);
    m.bad_filter = true;
    assert(u.read(tag, UhfBank::User, 0, 1, pw, out).status == STALE_DATA);
    m.bad_filter = false; m.wrong_tag = true;
    assert(u.read(tag, UhfBank::User, 0, 1, pw, out).status == STALE_DATA);
    m.wrong_tag = false;
    for (auto pair : {std::pair<uint8_t, Status>{0x15, NO_CARD}, {0x09, NO_CARD},
                      {0x16, AUTH_FAILED}, {0xa4, PERMISSION_DENIED}, {0x10, IO_ERROR}}) {
        m.error = pair.first; auto r = u.read(tag, UhfBank::User, 0, 1, pw, out);
        assert(r.status == pair.second && r.module_error == pair.first);
    }
    m.error = 0; m.transport = TIMEOUT;
    assert(u.read(tag, UhfBank::User, 0, 1, pw, out).status == TIMEOUT);
    m.transport = OK; m.stop = true; before = m.calls;
    assert(u.read(tag, UhfBank::User, 0, 1, pw, out).status == CANCELLED);
    assert(m.calls == before); m.stop = false; ++m.gen;
    assert(u.read(tag, UhfBank::User, 0, 1, pw, out).status == STALE_GENERATION);
    assert(m.calls == before);
    assert(u.inventory_start()); m.no_tags = true; assert(u.inventory_poll());
    assert(u.tag_count() == 0); assert(u.inventory_stop());
}
void user_write() {
    Mock m; Uhf u(m); const auto tag = scan(m, u); UhfPassword pw; UhfMemory prior, out;
    assert(u.read(tag, UhfBank::User, 2, 1, pw, prior));
    const uint8_t data[]{0xaa, 0xbb};
    auto r = u.write_user(tag, prior, data, 2, pw, out);
    assert(r && r.verified && r.mutation_attempted && m.writes == 1);
    assert(out.valid && out.data[0] == 0xaa && out.data[1] == 0xbb);
    assert(m.last_write == hex("000000000300020001AABB"));
    assert(u.write_user(tag, prior, data, 2, pw, out).status == STALE_DATA);
    assert(m.writes == 1);
    assert(u.read(tag, UhfBank::User, 0, 32, pw, prior));
    Bytes many(64, 0x5a); assert(u.write_user(tag, prior, many.data(), many.size(), pw, prior));
    assert(prior.valid && prior.data[63] == 0x5a && m.writes == 2);
    prior.bank = UhfBank::Reserved;
    assert(u.write_user(tag, prior, data, 2, pw, out).status == INVALID_ARGUMENT);
}
void rename() {
    for (bool reply_new : {false, true}) {
        Mock m; m.pc = {0x37, 0xa5}; m.new_identity_reply = reply_new;
        Uhf u(m); const auto tag = scan(m, u); UhfPassword pw; UhfMemory prior;
        assert(u.read(tag, UhfBank::Epc, 1, 7, pw, prior));
        const uint8_t epc[]{1, 2, 3, 4}; UhfTag after;
        auto r = u.rename_epc(tag, prior, epc, sizeof(epc), pw, after);
        assert(r && r.verified && m.writes == 1);
        assert(m.pc[0] == 0x17 && m.pc[1] == 0xa5);
        assert(after.pc == m.pc && after.epc_size == 4 && after.epc[3] == 4);
        assert(m.last_write == hex("00000000010001000317A501020304"));
        assert(m.filter == hex("0100000010300017A501020304"));
    }
    Mock m; Uhf u(m); const auto tag = scan(m, u); UhfPassword pw; UhfMemory prior;
    assert(u.read(tag, UhfBank::Epc, 1, 7, pw, prior));
    Bytes longer(28, 0xab); UhfTag after;
    assert(u.rename_epc(tag, prior, longer.data(), longer.size(), pw, after).status == INVALID_ARGUMENT);
    assert(m.writes == 0);
    assert(u.read(tag, UhfBank::Epc, 1, 15, pw, prior));
    assert(u.rename_epc(tag, prior, longer.data(), longer.size(), pw, after));
    assert(after.epc_size == 28 && after.pc[0] == 0x74 && after.pc[1] == 0);
}
void interrupted_writes() {
    for (unsigned failure = 0; failure < 8; ++failure) {
        Mock m; Uhf u(m); const auto tag = scan(m, u); UhfPassword pw; UhfMemory prior, out;
        assert(u.read(tag, UhfBank::User, 0, 1, pw, prior));
        if (failure == 0) m.ignore_write = true;
        if (failure == 1) m.cancel_write = true;
        if (failure == 2) m.unplug_write = true;
        if (failure == 3) m.write_error = 0xb3;
        if (failure == 4) m.hook = [&](uint8_t cmd) { if (cmd == 0x49) m.transport = TIMEOUT; };
        if (failure == 5) m.pc[0] ^= 1;
        if (failure == 6) m.partial_write = true;
        if (failure == 7) m.corrupt_command = 0x49;
        const uint8_t data[]{1, 2};
        auto r = u.write_user(tag, prior, data, 2, pw, out);
        if (failure == 5) { assert(r.status == STALE_DATA && m.writes == 0); }
        else {
            assert(r.status == WRITE_UNVERIFIED && r.mutation_attempted && !r.verified);
            assert(m.writes == (failure == 4 ? 0U : 1U));
            if (failure == 1) assert(r.cause == CANCELLED);
            if (failure == 2) assert(r.cause == STALE_GENERATION);
            if (failure == 3) assert(r.cause == INVALID_ARGUMENT && r.module_error == 0xb3);
            if (failure == 4) assert(r.cause == TIMEOUT);
            assert(u.tag_count() == 0);
            if (failure == 0 || failure == 6) {
                const unsigned writes = m.writes;
                assert(u.write_user(tag, prior, data, 2, pw, out).status == STALE_DATA);
                assert(m.writes == writes);
            }
        }
        assert(!out.valid);
    }
    // Mid-selection cancellation/hotplug is caught before subsequent physical commands.
    for (bool unplug : {false, true}) {
        Mock m; Uhf u(m); const auto tag = scan(m, u); UhfPassword pw; UhfMemory out;
        m.hook = [&](uint8_t cmd) { if (cmd == 0x0c) { if (unplug) ++m.gen; else m.stop = true; } };
        const unsigned before = m.calls;
        auto r = u.read(tag, UhfBank::User, 0, 1, pw, out);
        assert(r.status == (unplug ? STALE_GENERATION : CANCELLED));
        assert(m.calls == before + 1);
    }
}
void power_and_stale_pc() {
    Mock m; Uhf u(m); assert(u.read_power());
    auto expected = u.power(); assert(expected.centidbm == 2000);
    auto r = u.set_power(expected, 26); assert(r && r.verified && m.rf == 2600 && m.power_writes == 1);
    assert(u.set_power(expected, 20).status == STALE_DATA && m.power_writes == 1);
    expected = u.power(); const unsigned calls = m.calls;
    assert(u.set_power(expected, 17).status == INVALID_ARGUMENT);
    assert(u.set_power(expected, 27).status == INVALID_ARGUMENT); assert(m.calls == calls);
    const auto tag = scan(m, u); assert(u.inventory_start()); m.pc[0] ^= 1;
    assert(u.inventory_poll()); assert(u.inventory_stop());
    UhfPassword pw; UhfMemory out;
    assert(u.read(tag, UhfBank::User, 0, 1, pw, out).status == STALE_DATA);
    m.capable = false; assert(u.read_power().status == UNSUPPORTED);
    for (unsigned fail = 0; fail < 3; ++fail) {
        Mock device; Uhf service(device); assert(service.read_power());
        auto prior = service.power();
        if (fail == 0) device.bad_ack = true;
        if (fail == 1) device.ignore_power = true;
        if (fail == 2) device.corrupt_command = 0xb6;
        auto failure = service.set_power(prior, 26);
        assert(failure.status == WRITE_UNVERIFIED && !service.power().valid);
        assert(device.power_writes == 1);
    }
    Mock device; Uhf service(device); device.bad_ack = true;
    assert(service.inventory_start().status == BAD_FRAME);
    assert(!service.scanning());
    device.bad_ack = false; device.corrupt_command = 0xb7;
    assert(service.read_power().status == BAD_FRAME);
}
}
int main() {
    vendor_and_stream(); capacity_and_long_epc(); read_and_errors(); user_write();
    rename(); interrupted_writes(); power_and_stale_pc();
    std::puts("UHF local tests passed");
}
