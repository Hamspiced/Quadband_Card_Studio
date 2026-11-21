#include "bridge.hpp"
#include <cassert>
#include "golden.hpp"
#include <cstring>
#include <iostream>
using namespace qb;
static Bytes raw(unsigned cmd, Bytes p = {}, unsigned kind = 0) {
    Bytes b{0xbb, uint8_t(kind), uint8_t(cmd), uint8_t(p.size() >> 8), uint8_t(p.size())};
    b.insert(b.end(), p.begin(), p.end()); uint8_t s = 0;
    for (size_t i = 1; i < b.size(); ++i) s += b[i];
    b.push_back(s); b.push_back(0x7e); return b;
}
static Bytes usb(unsigned cmd, Bytes p = {}) {
    Bytes b{0x11, 0xef, uint8_t(cmd >> 8), uint8_t(cmd), 0, 0, uint8_t(p.size() >> 8), uint8_t(p.size()), 0};
    uint8_t s = 0; for (auto v : b) s += v; b[8] = uint8_t(-s);
    b.insert(b.end(), p.begin(), p.end()); s = 0; for (auto v : p) s += v;
    b.push_back(uint8_t(-s)); return b;
}
static void frames() {
    for (auto& golden : golden_frames) {
        for (size_t split = 0; split <= golden.size(); ++split) {
            Decoder d; unsigned calls = 0;
            auto check = [&](const Frame& f) { ++calls; assert(f.encode() == golden); };
            assert(d.feed(golden.data(), split, check));
            assert(d.feed(golden.data() + split, golden.size() - split, check));
            assert(calls == 1);
        }
        Bytes multiple = golden; multiple.insert(multiple.end(), golden.begin(), golden.end());
        Decoder d; unsigned calls = 0; assert(d.feed(multiple.data(), multiple.size(), [&](const Frame&) { ++calls; }));
        assert(calls == 2);
        auto bad = golden; bad.back() ^= 1;
        d.reset(); calls = 0;
        assert(!d.feed(bad.data(), bad.size(), [&](const Frame&) { ++calls; }));
        assert(calls == 0);
        assert(!d.feed(golden.data(), golden.size(), [](const Frame&) {}));
        for (unsigned index : {0u, 2u, 3u, 25u}) {
            bad = golden; bad[index] = 0xff; d.reset();
            assert(!d.feed(bad.data(), bad.size(), [](const Frame&) {}));
        }
    }
    Decoder d; Bytes overflow(max_buffer + 1);
    assert(!d.feed(overflow.data(), overflow.size(), [](const Frame&) {}));
}
static void sessions() {
    Session s; std::array<Slot, 3> slots;
    Frame f; f.id = 1; f.op = 1; f.payload.resize(8);
    assert(s.admit(f, false, slots) == NOT_AUTHORIZED);
    assert(s.admit(f, true, slots) == OK);
    s.nonce = 123;
    f.session = 123; f.id = 2; f.channel = 1; f.op = 17; f.generation = 1;
    assert(s.admit(f, true, slots) == UNAVAILABLE);
    f.id = 3; slots[0].present = true;
    assert(s.admit(f, true, slots) == OK && s.pending == 3);
    assert(s.admit(f, true, slots) == BAD_SESSION);
    f.id = 4; assert(s.admit(f, true, slots) == BUSY);
    s.pending = 0; f.id = 5; slots[0].generation = 2;
    assert(s.admit(f, true, slots) == STALE_GENERATION);
    f.id = 6; f.generation = 2; f.session = 999;
    assert(s.admit(f, true, slots) == BAD_SESSION);
    f.session = 123; f.id = UINT32_MAX; assert(s.admit(f, true, slots) == OK);
    s.pending = 0; f.id = 1; assert(s.admit(f, true, slots) == BAD_SESSION);
    auto epoch = s.epoch; s.clear();
    assert(!s.nonce && !s.pending && s.cancelled && s.epoch != epoch);
}
static void geometry() {
    for (unsigned count : {64, 256}) {
        unsigned eligible = 0;
        for (unsigned i = 0; i < count; ++i) if (classic_data(count, i)) ++eligible;
        assert(eligible == (count == 64 ? 47u : 215u));
    }
    assert(trailer(127) == 127 && trailer(128) == 143 && trailer(255) == 255);
    assert(access_group(128) == 0 && access_group(132) == 0 && access_group(133) == 1);
    assert(access_group(137) == 1 && access_group(138) == 2 && access_group(142) == 2);
    assert(!classic_data(256, 0) && !classic_data(256, 143) && !classic_data(64, 128));
    uint8_t t[16]{}; t[6] = 0xff; t[7] = 0x07; t[8] = 0x80;
    assert(classic_write_allowed(128, 0x60, t));
    assert(!classic_write_allowed(128, 0x61, t));
    t[6] ^= 1; assert(!classic_write_allowed(128, 0x60, t));
    const uint8_t a1[]{4, 0}, a4[]{2, 0}, a7[]{0x44, 0}, wrong[]{0, 0};
    assert(classic_geometry(8, a1, 4) == 64 && classic_geometry(0x18, a4, 4) == 256);
    assert(classic_geometry(8, a7, 7) == 64 && !classic_geometry(8, a7, 4));
    assert(!classic_geometry(8, wrong, 4) && !classic_geometry(0x20, a1, 4));
}
static void ntag_policy() {
    for (unsigned total : {45, 135, 231}) {
        auto l = ntag(total); uint8_t initial[16]{}, dyn[4]{}, cfg[8]{};
        initial[12] = 0xe1; initial[13] = 0x10;
        initial[14] = total == 45 ? 18 : total == 135 ? 62 : 111;
        cfg[3] = 0xff;
        assert(ntag_write_allowed(total, 4, initial, dyn, cfg, false));
        assert(ntag_write_allowed(total, l.last_user, initial, dyn, cfg, false));
        assert(!ntag_write_allowed(total, 3, initial, dyn, cfg, true));
        assert(!ntag_write_allowed(total, l.dynamic, initial, dyn, cfg, true));
        initial[10] = 7; dyn[3] = 0xbd;
        assert(ntag_write_allowed(total, 4, initial, dyn, cfg, false));
        initial[10] |= 1 << 4; assert(!ntag_write_allowed(total, 4, initial, dyn, cfg, true));
        initial[10] = 0; initial[11] = 0x80;
        assert(!ntag_write_allowed(total, 15, initial, dyn, cfg, true)); initial[11] = 0;
        dyn[0] = 2;
        unsigned first = total == 45 ? 18 : 32, last = total == 45 ? 19 : 47;
        assert(ntag_write_allowed(total, first - 1, initial, dyn, cfg, false));
        assert(!ntag_write_allowed(total, first, initial, dyn, cfg, true));
        assert(!ntag_write_allowed(total, last, initial, dyn, cfg, true));
        assert(ntag_write_allowed(total, last + 1, initial, dyn, cfg, false));
        dyn[0] = 0; cfg[3] = 4;
        assert(!ntag_write_allowed(total, 4, initial, dyn, cfg, false));
        assert(ntag_write_allowed(total, 4, initial, dyn, cfg, true));
        initial[14] = 255; assert(!ntag_write_allowed(total, 4, initial, dyn, cfg, true));
        initial[14] = 18; initial[13] = 0x11;
        assert(!ntag_write_allowed(total, 4, initial, dyn, cfg, true));
        initial[13] = 0x10; initial[15] = 0xf0;
        assert(!ntag_write_allowed(total, 4, initial, dyn, cfg, true));
    }
    assert(!ntag(48).pages);
}
static void raw_policy() {
    assert(usb_timeout_ms(1000) == 2500 && usb_timeout_ms(2009) == 2500);
    assert(usb_timeout_ms(3000) == 4200 && usb_timeout_ms(3016) == 4200 && usb_timeout_ms(3021) == 4200);
    assert(validate_uhf(raw(3, {0})) == OK);
    assert(validate_uhf(raw(0xb6, {7, 8})) == OK);
    assert(validate_uhf(raw(0xb6, {10, 40})) == OK);
    assert(validate_uhf(raw(0xb6, {7, 7})) == PERMISSION_DENIED);
    assert(validate_uhf(raw(7, {1})) == PERMISSION_DENIED);
    Bytes read{0,0,0,0,3,0,0,0,64};
    assert(validate_uhf(raw(0x39, read)) == OK);
    read[8] = 65; assert(validate_uhf(raw(0x39, read)) == INVALID_ARGUMENT);
    Bytes write{0,0,0,0,3,0,0,0,32}; write.resize(73);
    assert(validate_uhf(raw(0x49, write)) == OK);
    write[4] = 0; assert(validate_uhf(raw(0x49, write)) == PERMISSION_DENIED);
    write[4] = 1; assert(validate_uhf(raw(0x49, write)) == PERMISSION_DENIED);
    write[6] = 1; assert(validate_uhf(raw(0x49, write)) == OK);
    Bytes p{1,0,0,0,16,32,0,8,0,0x12,0x34};
    assert(exact_uhf_selection(p));
    UhfGuard g; g.observe(raw(0x0c, p), raw(0x0c, {0}, 1));
    assert(!g.writable()); g.observe(raw(0x0b), raw(0x0b, p, 1));
    assert(!g.writable()); g.observe(raw(0x12, {0}), raw(0x12, {0}, 1)); assert(g.writable());
    p[4] = 32; assert(!exact_uhf_selection(p));
    g.observe(raw(0x0b), raw(0x0b, p, 1)); assert(!g.writable());
    auto first = raw(0x22, {1,2,3}, 2), second = raw(0x22, {4,5,6}, 2);
    Bytes joined = first; joined.insert(joined.end(), second.begin(), second.end()); Bytes frame;
    assert(take_uhf_frame(joined, frame) == OK && frame == first);
    assert(take_uhf_frame(joined, frame) == OK && frame == second && joined.empty());
    assert(take_uhf_frame(joined, frame) == BUSY);
    first.back() = 0; assert(take_uhf_frame(first, frame) == IO_ERROR);
    for (unsigned cmd : {3001,3003,3005,3006,3011,3015,3018,3020,2010,4000})
        assert(validate_chameleon(usb(cmd)) == PERMISSION_DENIED);
    Bytes lf(11); assert(validate_chameleon(usb(3016, lf)) == OK);
    lf[4] = 0x10; assert(validate_chameleon(usb(3016, lf)) == PERMISSION_DENIED);
    lf[4] = 0; lf[0] = 7; assert(validate_chameleon(usb(3016, lf)) == PERMISSION_DENIED);
    lf[0] = 6; lf[5] = 1; assert(validate_chameleon(usb(3016, lf)) == PERMISSION_DENIED);
    assert(validate_chameleon(usb(3002, {42})) == OK && validate_chameleon(usb(3010, {0})) == OK);
    Bytes hf(24); hf[0] = 0x60; hf[1] = 128;
    assert(validate_chameleon(usb(2009, hf)) == OK);
    hf[1] = 143; assert(validate_chameleon(usb(2009, hf)) == PERMISSION_DENIED);
    Frame typed; typed.op = 17; typed.payload = {4,1,2,3,4,128,0,0x60,255,255,255,255,255,255};
    assert(validate_hf(typed) == OK);
    typed.payload.pop_back(); assert(validate_hf(typed) == INVALID_ARGUMENT);
    typed.op = 20; typed.payload = {4,1,2,3,4,4,0,0,0,0,0,0,0,0,0,0};
    assert(validate_hf(typed) == OK);
    typed.payload[7] = 1; assert(validate_hf(typed) == INVALID_ARGUMENT);
}
int main() {
    frames(); sessions(); geometry(); ntag_policy(); authoritative_locks(); raw_policy();
    std::cout << "Firmware host tests passed: golden framing, session admission, geometry, locks, raw guards.\n";
}
