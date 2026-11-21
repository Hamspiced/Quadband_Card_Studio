#include "../main/standalone/hf.hpp"
#include "../main/standalone/bulk.hpp"
#include <cassert>
#include <iostream>
using namespace qb;
using namespace qb::local;
#include "hf_fixtures_golden.hpp"

struct Mock : Port {
    HfIdentity card;
    std::array<std::array<uint8_t,16>,256> data{};
    uint32_t gen = 7;
    bool stop = false, cancel_after_write = false, change_after_write = false;
    bool enforce_key = false;
    ClassicKey required_key;
    unsigned writes = 0, fail_write = 0;
    unsigned page_reads = 0;
    int fail_read = -1;
    std::vector<unsigned> write_units;
    std::vector<std::array<uint8_t,16>> written;
    Mock(HfFamily family = HfFamily::Ntag213) {
        card.family = family; card.uid_size = 7; card.uid[0] = 4;
        if (card.ntag()) {
            card.atqa = {{0x44,0}};
            card.version = {{0,4,4,2,1,0,uint8_t(family == HfFamily::Ntag213 ? 15 : family == HfFamily::Ntag215 ? 17 : 19),3}};
            unsigned d = card.units()-5;
            data[3] = {0xe1,0x10,uint8_t(family == HfFamily::Ntag213 ? 18 : family == HfFamily::Ntag215 ? 63 : 111),0};
            data[d+1][3] = 255;
        } else for (unsigned b = 0; b < card.units(); ++b) if (trailer(b) == b) {
            data[b][6] = 0xff; data[b][7] = 0x07; data[b][8] = 0x80;
        }
    }
    bool cancelled() const override { return stop; }
    uint32_t generation(uint8_t) const override { return gen; }
    bool supports(uint8_t,uint16_t) const override { return true; }
    void progress(unsigned,unsigned,const char*) override {}
    void wait_ms(unsigned) override {}
    Status identify(HfIdentity& out) override { out = card; return OK; }
    Status read_classic(const HfIdentity&,unsigned b,const ClassicKey& key,std::array<uint8_t,16>& out) override {
        if (enforce_key && (key.type != required_key.type || key.bytes != required_key.bytes)) return AUTH_FAILED;
        if (int(b) == fail_read) return AUTH_FAILED;
        out = data[b]; return OK;
    }
    Status mutate(unsigned b,const std::array<uint8_t,16>& old,const std::array<uint8_t,16>& value) {
        assert(old == data[b]); ++writes; write_units.push_back(b); written.push_back(value);
        if (writes == fail_write) return IO_ERROR;
        data[b] = value;
        if (cancel_after_write) stop = true;
        if (change_after_write) ++gen;
        return OK;
    }
    Status write_classic(const HfIdentity&,unsigned b,const ClassicKey&,const std::array<uint8_t,16>& old,
                         const std::array<uint8_t,16>& value,std::array<uint8_t,16>& verified) override {
        assert(classic_data(card.units(),b)); auto s = mutate(b,old,value); verified = data[b]; return s;
    }
    Status read_pages(const HfIdentity&,unsigned b,unsigned count,const NtagAuth&,Bytes& out) override {
        ++page_reads; assert(count && count <= 16);
        if (fail_read >= int(b) && fail_read < int(b+count)) return AUTH_FAILED;
        assert(b+count <= card.units()-2); out.clear();
        for (unsigned i = b; i < b+count; ++i) out.insert(out.end(),data[i].begin(),data[i].begin()+4);
        return OK;
    }
    Status write_page(const HfIdentity&,unsigned b,const NtagAuth&,const std::array<uint8_t,4>& old,
                      const std::array<uint8_t,4>& value,std::array<uint8_t,4>& verified) override {
        assert(b >= 4 && b < card.units()-5); std::array<uint8_t,16> a{},v{};
        std::copy(old.begin(),old.end(),a.begin()); std::copy(value.begin(),value.end(),v.begin());
        auto s = mutate(b,a,v); std::copy_n(data[b].begin(),4,verified.begin()); return s;
    }
    Status reset_hf() override { return OK; }
    Status format_ndef(const HfIdentity& expected, uint8_t flags, const Bytes& snapshot) override {
        FormatIO io;
        io.guard = [&]() { return stop ? CANCELLED : card != expected ? STALE_DATA : OK; };
        io.read = [&](unsigned b, const ClassicKey&, Bytes& out) {
            if (int(b) == fail_read) return AUTH_FAILED;
            out.assign(data[b].begin(),data[b].begin()+card.unit_size()); return OK;
        };
        io.write = [&](unsigned b, const ClassicKey&, const Bytes& desired) {
            std::array<uint8_t,16> value{}; std::copy(desired.begin(),desired.end(),value.begin());
            return mutate(b,data[b],value);
        };
        return canonical_format(card,flags,snapshot,io);
    }
    Status transact(uint8_t,const Bytes&,Bytes&) override { assert(false); return UNSUPPORTED; }
};
static uint8_t crc(const Bytes& data) {
    uint8_t c = 0xc7;
    for (size_t i = 1; i < data.size(); ++i) {
        c ^= data[i]; for (unsigned b = 0; b < 8; ++b) c = uint8_t((c<<1) ^ (c&128 ? 0x1d : 0));
    }
    return c;
}
static void formatted(Mock& p, bool large = false) {
    Bytes mad(32,0); mad[2] = 3; mad[3] = 0xe1; mad[0] = crc(mad);
    std::copy_n(mad.begin(),16,p.data[1].begin()); std::copy_n(mad.begin()+16,16,p.data[2].begin());
    p.data[3][9] = large ? 0xc2 : 0xc1;
    assert(mad[0] == golden_mad1_crc);
    p.data[4][0] = 3; p.data[4][1] = 0; p.data[4][2] = 0xfe;
    if (large) {
        Bytes mad2(48,0); mad2[32] = 3; mad2[33] = 0xe1; mad2[0] = crc(mad2);
        assert(mad2[0] == golden_mad2_crc);
        for (unsigned i = 0; i < 3; ++i) std::copy_n(mad2.begin()+i*16,16,p.data[64+i].begin());
    }
}
#include "ndef_bulk_tests.hpp"
int main() {
    ndef_bulk_tests();
    std::string error; NdefRecord r; Bytes encoded; std::vector<NdefRecord> decoded;
    assert(ndef_catalog().size() == 19);
    for (const auto& g : goldens) {
        assert(ndef_build(g.kind,g.values,r,error) == OK);
        assert(ndef_encode({r},encoded,error) == OK && encoded == g.encoded);
        assert(ndef_decode(encoded,decoded,error) == OK && decoded == std::vector<NdefRecord>{r});
    }
    for (Bytes malformed : {Bytes{0xd1},Bytes{0xf1,0,0},Bytes{0x91,1,1,'T',0},Bytes{0xd6,0,0},Bytes{0xd0,1,0,'X'}})
        assert(ndef_decode(malformed,decoded,error) == INVALID_ARGUMENT);
    assert(ndef_build(0,{{"text",std::string(257,'x')}},r,error) == INVALID_ARGUMENT);
    assert(ndef_build(8,{{"query","x"},{"engine","invalid"}},r,error) == INVALID_ARGUMENT);
    assert(ndef_build(13,{{"latitude","nan"},{"longitude","1"}},r,error) == INVALID_ARGUMENT);
    assert(ndef_build(17,{{"ssid","x"},{"password","short"}},r,error) == INVALID_ARGUMENT);
    assert(ndef_build(0,{{"text",std::string("\xc0\xaf",2)}},r,error) == INVALID_ARGUMENT);
    assert(ndef_build(13,{{"latitude","0x10"},{"longitude","1"}},r,error) == INVALID_ARGUMENT);
    uint32_t random = 0x5152;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        Bytes malformed;
        for (unsigned n = trial%100; n; --n) {
            random = random*1664525u+1013904223u; malformed.push_back(uint8_t(random>>24));
        }
        auto status = ndef_decode(malformed,decoded,error);
        if (status == OK && !decoded.empty()) assert(ndef_encode(decoded,encoded,error) == OK);
    }
    assert(ndef_encode(std::vector<NdefRecord>(65,NdefRecord{}),encoded,error) == INVALID_ARGUMENT);
    NdefRecord unknown{4,{'x'}, {1,2,3}, {'i','d'}};
    assert(ndef_encode({unknown},encoded,error) == OK && ndef_decode(encoded,decoded,error) == OK && decoded[0] == unknown);
    HfReadOptions options; HfSnapshot s; HfNdefArea area; HfProfile profile;
    Mock p;
    p.card.family = HfFamily::Unknown;
    assert(hf_read(p,options,s).status == UNSUPPORTED && s.card.uid_size == 7);
    p = Mock(); p.stop = true;
    assert(hf_read(p,options,s).status == CANCELLED && !p.writes);
    p = Mock();
    assert(hf_read(p,options,s).verified);
    assert(!s.known[43] && !s.known[44]);
    assert(hf_ndef_area(s,area).status == OK && area.writable && area.records.empty());
    assert(hf_edit(p,s,2,{1,2,3,4}).status == PERMISSION_DENIED && !p.writes);
    assert(hf_edit(p,s,4,{1,2,3,4}).verified && p.writes == 1);
    assert(hf_edit(p,s,5,{1,2,3,4}).status == STALE_DATA && p.writes == 1);
    assert(hf_read(p,options,s).verified);
    assert(hf_capture(s,profile).verified && !profile.included[3] && !profile.included[40]);
    profile.data[5][0] = 55;
    assert(hf_copy(p,s,profile).verified);
    assert(hf_read(p,options,s).verified);
    p.data[2][2] = 1 << 5;
    assert(hf_edit(p,s,5,{0,0,0,0}).status == STALE_DATA);
    assert(hf_read(p,options,s).verified && !hf_writable(s,5));
    p.data[2][2] = 0;
    assert(hf_read(p,options,s).verified);
    p.fail_read = 10; assert(hf_read(p,options,s).status == IO_ERROR && !s.known[10] && s.errors[10] == AUTH_FAILED);
    p.fail_read = -1; p = Mock();
    assert(hf_read(p,options,s).verified);
    assert(ndef_build(0,{{"text","Hello"}},r,error) == OK);
    assert(hf_write_ndef(p,s,{r}).verified);
    assert(p.write_units.back() == 4 && p.written.back()[1] != 0);
    for (size_t i = 0; i+1 < p.written.size(); ++i) if (p.write_units[i] == 4) assert(p.written[i][1] == 0);
    assert(hf_read(p,options,s).verified && hf_ndef_area(s,area).status == OK && area.records[0] == r);
    p = Mock(); assert(hf_read(p,options,s).verified); p.fail_write = 1;
    assert(hf_write_ndef(p,s,{r}).status == WRITE_UNVERIFIED && p.writes == 1);
    p = Mock(); assert(hf_read(p,options,s).verified); p.cancel_after_write = true;
    assert(hf_edit(p,s,4,{1,2,3,4}).status == WRITE_UNVERIFIED && p.writes == 1);
    p = Mock(); assert(hf_read(p,options,s).verified); ++p.gen;
    assert(hf_edit(p,s,4,{1,2,3,4}).status == STALE_GENERATION && !p.writes);
    for (auto family : {HfFamily::Ntag215,HfFamily::Ntag216}) {
        p = Mock(family); assert(hf_read(p,options,s).verified);
        assert(hf_ndef_area(s,area).status == OK && area.writable);
        assert(hf_write_ndef(p,s,{r}).verified);
    }
    p = Mock(HfFamily::Ntag215); assert(hf_read(p,options,s).verified);
    NdefRecord long_record{2,{'x'},Bytes(300,'a'),{}};
    assert(hf_write_ndef(p,s,{long_record}).verified);
    assert(ndef_encode({long_record},encoded,error) == OK);
    assert(p.written.back()[1] == 255 && p.written.back()[2] == encoded.size()/256 && p.written.back()[3] == encoded.size()%256);
    assert(hf_read(p,options,s).verified && hf_ndef_area(s,area).status == OK && area.records[0] == long_record);
    for (const auto& g : type2_goldens) {
        p = Mock();
        for (size_t i = 0; i < g.memory.size(); ++i) p.data[4+i/4][i%4] = g.memory[i];
        assert(hf_read(p,options,s).verified && hf_ndef_area(s,area).status == OK);
        assert(area.writable == g.writable && area.prefix == g.prefix && area.suffix == g.suffix);
        if (g.writable) {
            assert(hf_write_ndef(p,s,{r,unknown}).verified);
            assert(hf_read(p,options,s).verified && hf_ndef_area(s,area).status == OK);
            assert(area.prefix == g.prefix && area.suffix == g.suffix && area.records[1] == unknown);
        } else assert(hf_write_ndef(p,s,{r}).status == PERMISSION_DENIED && !p.writes);
    }
    p = Mock(); assert(hf_read(p,options,s).verified); p.card.version[0] ^= 1;
    assert(hf_edit(p,s,4,{1,2,3,4}).status == STALE_DATA && !p.writes);
    p = Mock(); assert(hf_read(p,options,s).verified); p.change_after_write = true;
    assert(hf_edit(p,s,4,{1,2,3,4}).status == WRITE_UNVERIFIED && p.writes == 1);
    p = Mock(); p.data[40][0] = 1; assert(hf_read(p,options,s).verified);
    assert(!hf_writable(s,16) && !hf_writable(s,17) && hf_writable(s,18));
    assert(hf_capture(s,profile).verified); profile.data[4][0] = 10; profile.data[16][0] = 10;
    assert(hf_copy(p,s,profile).status == PERMISSION_DENIED && !p.writes);
    p = Mock(); p.data[41][3] = 4; assert(hf_read(p,options,s).verified);
    assert(!hf_writable(s,4)); options.auth.mode = 2; options.auth.password = {1,2,3,4}; options.auth.pack = {1,2};
    assert(hf_read(p,options,s).verified && hf_writable(s,4)); options.auth.mode = 0;
    p = Mock(HfFamily::Classic1K); formatted(p);
    assert(hf_read(p,options,s).verified && hf_ndef_area(s,area).status == OK && area.writable);
    assert(hf_write_ndef(p,s,{r}).verified);
    assert(hf_capture(s,profile).verified && !profile.included[0] && !profile.included[1] && !profile.included[2] && !profile.included[3]);
    assert(hf_edit(p,s,0,Bytes(16,0)).status == PERMISSION_DENIED);
    assert(hf_edit(p,s,3,Bytes(16,0)).status == PERMISSION_DENIED);
    p = Mock(HfFamily::Classic4K); formatted(p,true);
    assert(hf_read(p,options,s).verified && hf_ndef_area(s,area).status == OK && area.units.size() == 18);
    assert(area.units[3] == 128 && area.units.back() == 142);
    assert(hf_writable(s,142) && !hf_writable(s,143));
    assert(hf_write_ndef(p,s,{r}).verified);
    s.data[67][9] = 1; assert(hf_ndef_area(s,area).status != OK);
    assert(hf_read(p,options,s).verified); s.data[1][0] ^= 1; assert(hf_ndef_area(s,area).status != OK);
    p = Mock(HfFamily::Classic1K); assert(hf_read(p,options,s).verified && hf_ndef_area(s,area).status != OK);
    p.enforce_key = true; p.required_key.bytes = {1,2,3,4,5,6};
    assert(hf_read(p,options,s).status == IO_ERROR && !s.known[0]);
    options.key = p.required_key; assert(hf_read(p,options,s).verified);
    options.key = ClassicKey(); p.required_key.bytes = {0xd3,0xf7,0xd3,0xf7,0xd3,0xf7};
    options.standard_keys = true;
    assert(hf_read(p,options,s).status == IO_ERROR && !s.known[0] && s.known[4] && s.known[63]);
    options.standard_keys = false;
    p = Mock(); p.data[4] = {1,3,0xa0,12}; p.data[5] = {0x34,3,0,0xfe};
    assert(hf_read(p,options,s).verified); assert(hf_ndef_area(s,area).status == OK && area.writable);
    s.data[5][0] = 0x24; assert(hf_ndef_area(s,area).status == OK && !area.writable);
    s.clear(); profile.clear();
    assert(s.card.uid_size == 0 && !s.known[0] && !profile.included[4] && s.keys[0].bytes[0] == 0);
    std::cout << "HF local host tests passed; all 19 desktop golden records matched.\n";
}
