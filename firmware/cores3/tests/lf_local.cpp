#include "standalone/lf.hpp"
#include <cassert>
#include <iostream>
#include <set>
#include <sstream>
using namespace qb;
using namespace qb::local;

static Bytes reply(unsigned cmd, const Bytes& payload = {}, unsigned status = 0x40) {
    Bytes out{0x11, 0xef, uint8_t(cmd >> 8), uint8_t(cmd), uint8_t(status >> 8),
              uint8_t(status), 0, uint8_t(payload.size()), 0};
    uint8_t sum = 0;
    for (auto b : out) sum += b;
    out[8] = uint8_t(0 - sum);
    out.insert(out.end(), payload.begin(), payload.end());
    sum = 0;
    for (auto b : out) sum += b;
    out.push_back(uint8_t(0 - sum));
    return out;
}
struct FakePort : Port {
    bool stopped = false;
    uint32_t gen = 7;
    std::set<uint16_t> caps{1001, 3000, 3002, 3004, 3010, 3014, 3016, 3019, 3021};
    unsigned calls = 0, waits = 0, writes = 0;
    std::function<Status(unsigned, const Bytes&, Bytes&)> exchange;
    std::function<void(unsigned, unsigned)> progressed;
    std::function<void()> waited;
    bool cancelled() const override { return stopped; }
    uint32_t generation(uint8_t channel) const override { assert(channel == 3); return gen; }
    bool supports(uint8_t channel, uint16_t cmd) const override { assert(channel == 3); return caps.count(cmd); }
    void progress(unsigned done, unsigned total, const char*) override {
        if (progressed) progressed(done, total);
    }
    void wait_ms(unsigned ms) override { assert(ms <= 50); ++waits; if (waited) waited(); }
    Status identify(HfIdentity&) override { assert(false); return UNSUPPORTED; }
    Status read_classic(const HfIdentity&, unsigned, const ClassicKey&, std::array<uint8_t, 16>&) override {
        assert(false); return UNSUPPORTED;
    }
    Status write_classic(const HfIdentity&, unsigned, const ClassicKey&, const std::array<uint8_t, 16>&,
                         const std::array<uint8_t, 16>&, std::array<uint8_t, 16>&) override {
        assert(false); return UNSUPPORTED;
    }
    Status read_pages(const HfIdentity&, unsigned, unsigned, const NtagAuth&, Bytes&) override {
        assert(false); return UNSUPPORTED;
    }
    Status write_page(const HfIdentity&, unsigned, const NtagAuth&, const std::array<uint8_t, 4>&,
                      const std::array<uint8_t, 4>&, std::array<uint8_t, 4>&) override {
        assert(false); return UNSUPPORTED;
    }
    Status reset_hf() override { assert(false); return UNSUPPORTED; }
    Status transact(uint8_t channel, const Bytes& cmd, Bytes& out) override {
        assert(channel == 3 && validate_chameleon(cmd) == OK);
        unsigned command = unsigned(cmd[2]) << 8 | cmd[3];
        ++calls;
        if (command == 3016) ++writes;
        return exchange(command, cmd, out);
    }
};
static LFCard parse(LFFormat format, const std::string& value) {
    LFCard card;
    assert(lf_parse_id(format, value, card) == OK);
    return card;
}
static Bytes bytes(const LFCard& card) { return Bytes(card.data.begin(), card.data.begin() + card.size); }
static void decoders() {
    LFCard card, expected = lf_default_test_card();
    assert(lf_decode_response(LFFormat::EM410x, bytes(expected), card) == OK && expected.matches(card));
    for (unsigned code = 100; code <= 103; ++code) {
        Bytes data{0, uint8_t(code)}; auto id = bytes(expected);
        data.insert(data.end(), id.begin(), id.end());
        assert(lf_decode_response(LFFormat::Electra, data, card) == OK && expected.matches(card));
    }
    Bytes electra{0, 104}; electra.resize(15, 0x23);
    assert(lf_decode_response(LFFormat::EM410x, electra, card) == OK && card.format == LFFormat::Electra);
    electra[1] = 103;
    assert(lf_decode_response(LFFormat::Electra, electra, card) == IO_ERROR && !card.size);
    for (unsigned n = 0; n < 50; ++n) {
        Bytes malformed(n, 0x23);
        if (n != 5) assert(lf_decode_response(LFFormat::EM410x, malformed, card) == IO_ERROR);
    }
    Bytes hid(16, 0xab);
    for (unsigned hint = 0; hint < 256; ++hint) {
        hid[0] = uint8_t(hint);
        const bool allowed = (hint >= 1 && hint <= 30) || hint == 42;
        assert((lf_decode_response(LFFormat::HIDProx, hid, card) == OK) == allowed);
        if (allowed) assert(card.size == 13);
    }
    hid.resize(14); assert(lf_decode_response(LFFormat::HIDProx, hid, card) == IO_ERROR);
    LFCard io = parse(LFFormat::IoProx, "000102030405060708090a0b0c0d0e0f"), other = io;
    other.data[15] ^= 0xff; assert(io.matches(other));
    other.data[11] ^= 1; assert(!io.matches(other));
    for (const auto& d : lf_formats()) {
        const std::string text = d.requires_ascii ? "12345678" :
            d.format == LFFormat::HIDProx ? "0102030405060708090a0b0c0d" : std::string(d.id_length * 2, 'a');
        assert(lf_parse_id(d.format, text, card) == OK && card.valid());
        assert(lf_parse_id(d.format, text + "0", card) == INVALID_ARGUMENT && !card.size);
        assert(lf_parse_id(d.format, text.substr(1), card) == INVALID_ARGUMENT);
    }
    assert(lf_parse_id(LFFormat::PACStanley, "abc\n1234", card) == INVALID_ARGUMENT);
    assert(lf_parse_id(LFFormat::EM410x, "01020304zz", card) == INVALID_ARGUMENT);
    assert(lf_parse_id(static_cast<LFFormat>(255), "", card) == INVALID_ARGUMENT);
    LFEncoding enc;
    assert(lf_encode_t5577(parse(LFFormat::HIDProx, "0102030405060708090a0b0c0d"), enc) == UNSUPPORTED);
    assert(lf_encode_t5577(parse(LFFormat::Pyramid, std::string(32, '1')), enc) == UNSUPPORTED);
    card = expected; card.clear();
    for (auto b : card.data) assert(!b);
}
static void scans() {
    for (const auto& d : lf_formats()) {
        FakePort port;
        LFCard expected;
        expected.format = d.format; expected.size = d.id_length;
        expected.data.fill(d.requires_ascii ? 'A' : 0x23);
        if (d.format == LFFormat::HIDProx) expected.data[0] = 42;
        port.exchange = [&](unsigned cmd, const Bytes& in, Bytes& out) {
            if (cmd == 1001) { assert(in.size() == 11 && in[9] == 1); out = reply(cmd, {}, 0x68); }
            else {
                assert(cmd == d.scan_command);
                if (d.format == LFFormat::HIDProx || d.format == LFFormat::IoProx)
                    assert(in.size() == 11 && in[9] == (d.format == LFFormat::HIDProx ? 42 : 0));
                else assert(in.size() == 10);
                Bytes data = bytes(expected);
                if (d.format == LFFormat::Electra) data.insert(data.begin(), {0,104});
                out = reply(cmd, data);
            }
            return OK;
        };
        LFSnapshot snapshot;
        assert(lf_scan(port, d.format, snapshot, d.format == LFFormat::HIDProx ? 42 : 0) == OK);
        assert(snapshot.present && snapshot.card.matches(expected) && snapshot.generation == port.gen);
        assert(lf_writable(port, d.format) == d.writable);
        port.caps.erase(d.scan_command);
        assert(!lf_scannable(port, d.format));
        assert(lf_scan(port, d.format, snapshot) == UNSUPPORTED && !snapshot.generation);
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        FakePort port;
        port.exchange = [&](unsigned cmd, const Bytes&, Bytes& out) {
            out = reply(cmd, cmd == 1001 ? Bytes{} : bytes(lf_default_test_card()), cmd == 1001 ? 0x68 : 0x40);
            if (cmd == 3000) {
                if (fault == 0) out.back() ^= 1;
                if (fault == 1) out[8] ^= 1;
                if (fault == 2) out = reply(3004, bytes(lf_default_test_card()));
                if (fault == 3) out = reply(cmd, bytes(lf_default_test_card()), 0x68);
                if (fault == 4) out.push_back(0);
                if (fault == 5) out = reply(cmd, {}, 0x41);
                if (fault == 6) port.stopped = true;
                if (fault == 7) ++port.gen;
            }
            return OK;
        };
        LFSnapshot snapshot;
        const auto status = lf_scan(port, LFFormat::EM410x, snapshot);
        assert(status == (fault == 5 ? NO_CARD : fault == 6 ? CANCELLED : fault == 7 ? STALE_GENERATION : IO_ERROR));
        assert(!snapshot.present);
    }
    FakePort empty; empty.caps.clear(); LFSnapshot snapshot;
    assert(lf_scan(empty, LFFormat::EM410x, snapshot) == UNSUPPORTED && empty.calls == 0);
    empty.caps = {1001, 3002};
    assert(lf_scan(empty, LFFormat::HIDProx, snapshot, 31) == INVALID_ARGUMENT && empty.calls == 0);
}
static LFSnapshot target() {
    LFSnapshot snap;
    snap.generation = 7; snap.present = true; snap.card = lf_default_test_card();
    return snap;
}
static void writer_exchange(FakePort& port, const LFCard& desired, bool absent = false) {
    port.exchange = [&, absent](unsigned cmd, const Bytes& in, Bytes& out) {
        if (cmd == 1001) { out = reply(cmd, {}, 0x68); return OK; }
        if (cmd == 3000) {
            out = absent && !port.writes ? reply(cmd, {}, 0x41) :
                reply(cmd, bytes(port.writes ? desired : lf_default_test_card()));
            return OK;
        }
        assert(cmd == 3016 && in.size() == 21);
        assert(in[9] <= 6 && in[9] != 7);
        for (unsigned i = 14; i < 20; ++i) assert(!in[i]);
        if (!in[9]) assert(!(in[13] & 0x10));
        out = reply(cmd);
        return OK;
    };
}
static void writes() {
    LFCard desired = parse(LFFormat::EM410x, "aabbccddee");
    for (const auto& d : lf_formats()) {
        if (!d.writable) continue;
        LFCard wanted;
        wanted.format = d.format; wanted.size = d.id_length;
        wanted.data.fill(d.requires_ascii ? 'B' : 0xab);
        LFEncoding encoded;
        assert(lf_encode_t5577(wanted, encoded) == OK);
        for (unsigned mismatch = 0; mismatch < 2; ++mismatch) {
            FakePort port;
            port.exchange = [&](unsigned cmd, const Bytes& in, Bytes& out) {
                if (cmd == 1001) { out = reply(cmd, {}, 0x68); return OK; }
                if (cmd == 3016) {
                    assert(port.writes <= encoded.count);
                    const auto& block = encoded.blocks[port.writes - 1];
                    assert(in[9] == block.block && in[9] <= 6);
                    uint32_t word = 0;
                    for (unsigned i = 10; i < 14; ++i) word = (word << 8) | in[i];
                    assert(word == block.word);
                    assert((port.writes == encoded.count) == (block.block == 0));
                    for (unsigned i = 14; i < 20; ++i) assert(!in[i]);
                    out = reply(cmd); return OK;
                }
                if (!port.writes) {
                    assert(cmd == 3000);
                    out = reply(cmd, bytes(lf_default_test_card()));
                } else {
                    assert(cmd == d.scan_command);
                    LFCard actual = wanted;
                    if (mismatch) actual.data[d.format == LFFormat::IoProx ? 11 : wanted.size - 1] ^= 1;
                    Bytes data = bytes(actual);
                    if (d.format == LFFormat::Electra) data.insert(data.begin(), {0,104});
                    if (d.format == LFFormat::IoProx) {
                        assert(in.size() == 11 && in[9] == 0);
                        data[15] ^= 0xff;
                    }
                    out = reply(cmd, data);
                }
                return OK;
            };
            auto result = lf_write_t5577(port, target(), wanted);
            assert(result.status == (mismatch ? WRITE_UNVERIFIED : OK));
            assert(result.blocks_written == encoded.count && port.writes == encoded.count);
        }
    }
    {
        FakePort port; writer_exchange(port, desired);
        auto result = lf_write_t5577(port, target(), desired);
        assert(result.status == OK && result.blocks_written == 3 && result.mutation_started);
        assert(result.actual.matches(desired) && port.waits == 5 && port.calls == 6);
    }
    {
        FakePort port; writer_exchange(port, desired, true);
        auto snap = target(); snap.present = false; snap.card.clear();
        assert(lf_write_t5577(port, snap, desired).status == OK);
    }
    for (unsigned fault_at = 1; fault_at <= 6; ++fault_at) {
        for (unsigned fault = 0; fault < 5; ++fault) {
            FakePort port; writer_exchange(port, desired);
            const auto normal = port.exchange;
            port.exchange = [&](unsigned cmd, const Bytes& in, Bytes& out) {
                auto status = normal(cmd, in, out);
                if (port.calls == fault_at) {
                    if (fault == 0) return TIMEOUT;
                    if (fault == 1) port.stopped = true;
                    if (fault == 2) ++port.gen;
                    if (fault == 3) out.back() ^= 1;
                    if (fault == 4) out = reply(cmd, {0xab}, cmd == 1001 ? 0x68 : 0x40);
                }
                return status;
            };
            auto result = lf_write_t5577(port, target(), desired);
            assert(result.status != OK);
            assert((result.status == WRITE_UNVERIFIED) == (fault_at >= 3));
            assert(port.calls == fault_at);
        }
    }
    for (unsigned fault = 0; fault < 10; ++fault) {
        FakePort port; writer_exchange(port, desired); auto snap = target();
        if (fault == 0) port.stopped = true;
        if (fault == 1) ++port.gen;
        if (fault == 2) snap.card.data[0] ^= 1;
        if (fault == 3) port.caps.erase(3016);
        if (fault == 4) port.caps.erase(3000);
        if (fault == 5) snap.generation = 0;
        if (fault == 6) snap.card.size = 0;
        if (fault == 7) snap.hint = 31;
        if (fault == 8) snap.present = false;
        if (fault == 9) snap.requested_format = static_cast<LFFormat>(255);
        auto result = lf_write_t5577(port, snap, desired);
        assert(result.status != OK && !result.mutation_started && port.writes == 0);
    }
    for (unsigned done = 0; done <= 4; ++done) {
        FakePort port; writer_exchange(port, desired);
        port.progressed = [&](unsigned n, unsigned total) { if (total == 4 && n == done) port.stopped = true; };
        auto result = lf_write_t5577(port, target(), desired);
        assert(result.status == (done == 0 ? CANCELLED : WRITE_UNVERIFIED));
        assert(port.writes == std::min(done, 3u));
    }
    {
        FakePort port; writer_exchange(port, desired);
        port.waited = [&] { port.stopped = true; };
        assert(lf_write_t5577(port, target(), desired).status == WRITE_UNVERIFIED);
        assert(port.waits == 1 && port.calls == 5);
    }
    {
        FakePort port; writer_exchange(port, desired);
        const auto normal = port.exchange;
        port.exchange = [&](unsigned cmd, const Bytes& in, Bytes& out) {
            auto status = normal(cmd, in, out);
            if (cmd == 3000 && port.writes) out = reply(cmd, bytes(lf_default_test_card()));
            return status;
        };
        assert(lf_write_t5577(port, target(), desired).status == WRITE_UNVERIFIED);
    }
}
int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "encode") {
        LFCard card;
        const auto format = static_cast<LFFormat>(std::stoi(argv[2]));
        if (lf_parse_id(format, argv[3], card) != OK) return 1;
        LFEncoding out;
        if (lf_encode_t5577(card, out) != OK) return 2;
        for (unsigned i = 0; i < out.count; ++i)
            std::cout << unsigned(out.blocks[i].block) << ':' << out.blocks[i].word << '\n';
        return 0;
    }
    decoders(); scans(); writes();
    std::cout << "LF portable decoder, scan, capability, guard and interruption tests passed\n";
}
