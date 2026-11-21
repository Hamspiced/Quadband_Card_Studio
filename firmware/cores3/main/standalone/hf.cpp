#include "hf.hpp"
#include <memory>

namespace qb::local {
namespace {
unsigned sector(unsigned b) { return b < 128 ? b/4 : 32+(b-128)/16; }
unsigned first(unsigned s) { return s < 32 ? s*4 : 128+(s-32)*16; }
HfResult result(Status status, const char* detail) {
    HfResult r; r.status = status; r.detail = detail; return r;
}
Status alive(Port& p, uint32_t generation) {
    if (p.cancelled()) return CANCELLED;
    return generation && p.generation(1) == generation ? OK : STALE_GENERATION;
}
Status identity(Port& p, const HfSnapshot& s) {
    auto status = alive(p,s.generation); if (status != OK) return status;
    HfIdentity card; status = p.identify(card);
    if (status != OK) return status;
    status = alive(p,s.generation);
    return status != OK ? status : card == s.card ? OK : STALE_DATA;
}
bool auth_valid(const NtagAuth& auth) { return auth.mode <= 2; }
Status read_unit(Port& p, const HfSnapshot& s, unsigned unit, std::array<uint8_t,16>& out) {
    auto status = alive(p,s.generation); if (status != OK) return status;
    out.fill(0);
    if (s.card.classic()) {
        unsigned sec = sector(unit);
        if (!s.key_known[sec]) return AUTH_FAILED;
        status = p.read_classic(s.card,unit,s.keys[sec],out);
    } else {
        Bytes data;
        status = p.read_pages(s.card,unit,1,s.auth,data);
        if (status == OK) {
            if (data.size() != 4) return IO_ERROR;
            std::copy(data.begin(),data.end(),out.begin());
        }
    }
    return status == OK ? alive(p,s.generation) : status;
}
Status access_code(const uint8_t* t, unsigned index, unsigned& code) {
    uint8_t c1 = t[7] >> 4, c2 = t[8] & 15, c3 = t[8] >> 4;
    if (((t[6]&15)^c1) != 15 || ((t[6]>>4)^c2) != 15 || ((t[7]&15)^c3) != 15) return PERMISSION_DENIED;
    code = ((c1 >> index & 1)<<2) | ((c2 >> index & 1)<<1) | (c3 >> index & 1); return OK;
}
bool readable(const HfSnapshot& s, unsigned b) {
    if (b >= s.card.units() || !s.known[b] || !s.known[trailer(b)] || !s.key_known[sector(b)]) return false;
    unsigned code = 0, tc = 0; const auto* t = s.data[trailer(b)].data();
    if (access_code(t,access_group(b),code) != OK || access_code(t,3,tc) != OK) return false;
    auto key = s.keys[sector(b)].type;
    if (key != 0x60 && key != 0x61) return false;
    if (key == 0x61 && tc <= 2) return false;
    return code == 0 || code == 1 || code == 2 || code == 4 || code == 6 || ((code == 3 || code == 5) && key == 0x61);
}
Status preflight(Port& p, const HfSnapshot& expected) {
    auto status = identity(p,expected); if (status != OK) return status;
    for (unsigned b = 0; b < expected.card.units(); ++b) {
        if (!expected.known[b]) continue;
        std::array<uint8_t,16> value{};
        status = read_unit(p,expected,b,value);
        if (status != OK) return status;
        if (value != expected.data[b]) return STALE_DATA;
        p.progress(b+1,expected.card.units(),"Checking target");
    }
    return identity(p,expected);
}
Status update(Port& p, HfSnapshot& current, unsigned b, const std::array<uint8_t,16>& desired, HfResult& r) {
    auto status = identity(p,current); if (status != OK) return status;
    std::array<uint8_t,16> previous{};
    status = read_unit(p,current,b,previous); if (status != OK) return status;
    if (previous != current.data[b]) return STALE_DATA;
    if (previous != desired) {
        status = alive(p,current.generation); if (status != OK) return status;
        r.attempted = true;
        if (current.card.classic()) {
            std::array<uint8_t,16> verified{};
            status = p.write_classic(current.card,b,current.keys[sector(b)],previous,desired,verified);
            if (status == OK && verified != desired) status = WRITE_UNVERIFIED;
        } else {
            std::array<uint8_t,4> old{}, value{}, verified{};
            std::copy_n(previous.begin(),4,old.begin()); std::copy_n(desired.begin(),4,value.begin());
            status = p.write_page(current.card,b,current.auth,old,value,verified);
            if (status == OK && verified != value) status = WRITE_UNVERIFIED;
        }
        if (status != OK) return status;
    }
    status = read_unit(p,current,b,previous); if (status != OK) return status;
    if (previous != desired) return WRITE_UNVERIFIED;
    status = identity(p,current); if (status != OK) return status;
    current.data[b] = desired; return OK;
}
HfResult failed(HfResult r, Status s, const char* text) {
    r.status = r.attempted ? WRITE_UNVERIFIED : s; r.verified = false; r.detail = text; return r;
}
uint8_t mad_crc(const Bytes& data) {
    uint8_t c = 0xc7;
    for (size_t i = 1; i < data.size(); ++i) {
        c ^= data[i];
        for (unsigned b = 0; b < 8; ++b) c = uint8_t((c<<1) ^ (c&128 ? 0x1d : 0));
    }
    return c;
}
bool control(const HfSnapshot& s, const HfNdefArea& a, uint8_t tag, const Bytes& value) {
    if (!s.card.ntag() || value.size() != 3) return false;
    unsigned page_size = 1u << (value[2]&15), offset = value[0]&15;
    if (offset >= page_size) return false;
    unsigned address = (value[0]>>4)*page_size+offset, size = value[1] ? value[1] : 256;
    if (tag == 1) return address == (s.card.units()-5)*4 &&
        size == (s.card.family == HfFamily::Ntag213 ? 12u : s.card.family == HfFamily::Ntag215 ? 8u : 14u) &&
        (1u << (value[2]>>4)) == (s.card.family == HfFamily::Ntag213 ? 8u : 64u);
    return !(value[2]>>4) && address >= 16+a.memory.size() && address+size <= s.card.units()*4;
}
}
void HfSnapshot::clear() {
    secure_zero(data.data(),sizeof(data)); secure_zero(card.uid.data(),card.uid.size()); card = {};
    known.fill(false); errors.fill(OK); key_known.fill(false); generation = 0;
    for (auto& k : keys) { secure_zero(k.bytes.data(),k.bytes.size()); k.type = 0x60; }
    secure_zero(auth.password.data(),auth.password.size()); secure_zero(auth.pack.data(),auth.pack.size()); auth.mode = 0;
}
void HfProfile::clear() {
    secure_zero(data.data(),sizeof(data)); included.fill(false); family = HfFamily::Unknown; unit_size = unit_count = 0;
}
bool hf_data_unit(const HfIdentity& card, unsigned unit) {
    return card.classic() ? classic_data(card.units(),unit) : card.ntag() && unit >= 4 && unit < card.units()-5;
}
bool hf_profile_unit(const HfIdentity& card, unsigned unit) {
    if (!hf_data_unit(card,unit)) return false;
    return !card.classic() || (unit != 1 && unit != 2 && (card.family != HfFamily::Classic4K || unit < 64 || unit > 66));
}
bool hf_writable(const HfSnapshot& s, unsigned b) {
    if (!hf_data_unit(s.card,b) || !s.known[b]) return false;
    if (s.card.classic())
        return s.known[trailer(b)] && s.key_known[sector(b)] &&
            classic_write_allowed(b,s.keys[sector(b)].type,s.data[trailer(b)].data());
    unsigned d = s.card.units()-5;
    for (unsigned unit : {2u,3u,d,d+1,d+2}) if (!s.known[unit]) return false;
    std::array<uint8_t,16> initial{}; std::array<uint8_t,8> config{};
    std::copy_n(s.data[2].begin(),4,initial.begin()+8); std::copy_n(s.data[3].begin(),4,initial.begin()+12);
    std::copy_n(s.data[d+1].begin(),4,config.begin()); std::copy_n(s.data[d+2].begin(),4,config.begin()+4);
    return ntag_write_allowed(s.card.units(),b,initial.data(),s.data[d].data(),config.data(),s.auth.mode != 0);
}
HfResult hf_read(Port& p, const HfReadOptions& options, HfSnapshot& out) {
    out.clear();
    if ((options.key.type != 0x60 && options.key.type != 0x61) || !auth_valid(options.auth))
        return result(INVALID_ARGUMENT,"Invalid authentication options.");
    out.generation = p.generation(1);
    auto status = alive(p,out.generation); if (status != OK) return result(status,"Reader unavailable or cancelled.");
    status = p.identify(out.card); if (status != OK) return result(status,"Card identification failed.");
    if (!out.card.units() || !out.card.uid_size || out.card.uid_size > out.card.uid.size()) return result(UNSUPPORTED,"Unsupported HF identity.");
    out.auth = options.auth;
    HfResult r; r.total = out.card.ntag() ? out.card.units()-2 : out.card.units();
    if (out.card.classic()) {
        for (unsigned sec = 0; sec < (out.card.units() == 64 ? 16u : 40u); ++sec) {
            auto begin = first(sec), end = trailer(begin);
            std::vector<ClassicKey> candidates{options.key};
            if (options.standard_keys) {
                ClassicKey standard, factory;
                standard.bytes = sec == 0 || sec == 16 ? std::array<uint8_t,6>{{0xa0,0xa1,0xa2,0xa3,0xa4,0xa5}} :
                    std::array<uint8_t,6>{{0xd3,0xf7,0xd3,0xf7,0xd3,0xf7}};
                candidates = {standard,options.key,factory};
            }
            Status error = AUTH_FAILED;
            for (const auto& key : candidates) {
                status = identity(p,out); if (status != OK) return failed(r,status,"Read interrupted.");
                status = p.read_classic(out.card,begin,key,out.data[begin]);
                if (status == OK) { out.keys[sec] = key; out.key_known[sec] = true; out.known[begin] = true; ++r.completed; break; }
                error = status;
                if (status != AUTH_FAILED) break;
                status = alive(p,out.generation); if (status != OK) return failed(r,status,"Read interrupted.");
                status = p.reset_hf(); if (status != OK) return failed(r,status,"Authentication reset failed.");
            }
            for (unsigned b = begin; b <= end; ++b) {
                if (b != begin && out.key_known[sec]) {
                    status = read_unit(p,out,b,out.data[b]); out.known[b] = status == OK; out.errors[b] = status;
                    if (status == OK) ++r.completed;
                    if (status == CANCELLED || status == STALE_GENERATION) return failed(r,status,"Read interrupted.");
                } else if (!out.known[b]) out.errors[b] = error;
                p.progress(b+1,r.total,"Reading Classic memory");
            }
        }
    } else {
        for (unsigned b = 0; b < r.total;) {
            status = identity(p,out); if (status != OK) return failed(r,status,"Read interrupted.");
            unsigned count = std::min(16u,r.total-b);
            Bytes bytes;
            status = p.read_pages(out.card,b,count,out.auth,bytes);
            if (status == OK && bytes.size() != count*4) status = IO_ERROR;
            if (status == OK) status = identity(p,out);
            for (unsigned i = 0; i < count; ++i) {
                out.errors[b+i] = status; out.known[b+i] = status == OK;
                if (status == OK) { std::copy_n(bytes.begin()+i*4,4,out.data[b+i].begin()); ++r.completed; }
            }
            if (status == CANCELLED || status == STALE_GENERATION || status == STALE_DATA || status == NO_CARD)
                return failed(r,status,"Read interrupted.");
            b += count; p.progress(b,r.total,"Reading NTAG pages");
        }
    }
    status = identity(p,out); if (status != OK) return failed(r,status,"Card changed during read.");
    r.verified = r.completed == r.total; r.status = r.verified ? OK : IO_ERROR;
    r.detail = r.verified ? "Memory read complete." : "Partial read; inspect unit errors."; return r;
}
HfResult hf_edit(Port& p, const HfSnapshot& expected, unsigned b, const Bytes& desired) {
    if (!hf_writable(expected,b)) return result(PERMISSION_DENIED,"Protected unit or incomplete permissions.");
    if (desired.size() != expected.card.unit_size()) return result(INVALID_ARGUMENT,"Wrong unit size.");
    HfResult r; r.total = 1;
    auto status = preflight(p,expected); if (status != OK) return failed(r,status,"Target changed before write.");
    auto current = std::make_unique<HfSnapshot>(expected);
    std::array<uint8_t,16> value{}; std::copy(desired.begin(),desired.end(),value.begin());
    status = update(p,*current,b,value,r); if (status != OK) return failed(r,status,"Write not verified; read target again.");
    r.completed = 1; r.verified = true; r.detail = "Unit verified."; return r;
}
HfResult hf_capture(const HfSnapshot& s, HfProfile& out) {
    out.clear();
    if (!s.card.units()) return result(UNSUPPORTED,"Unsupported source family.");
    HfResult r;
    for (unsigned b = 0; b < s.card.units(); ++b) if (hf_profile_unit(s.card,b)) {
        ++r.total;
        if (!s.known[b]) return result(IO_ERROR,"Source data is incomplete.");
    }
    out.family = s.card.family; out.unit_size = s.card.unit_size(); out.unit_count = s.card.units();
    for (unsigned b = 0; b < s.card.units(); ++b) if (hf_profile_unit(s.card,b)) {
        out.data[b] = s.data[b]; out.included[b] = true; ++r.completed;
    }
    r.verified = true; r.detail = "Data-only RAM source captured."; return r;
}
HfResult hf_copy(Port& p, const HfSnapshot& expected, const HfProfile& profile) {
    if (profile.family != expected.card.family || profile.unit_count != expected.card.units() || profile.unit_size != expected.card.unit_size())
        return result(INVALID_ARGUMENT,"Source and target geometry mismatch.");
    HfResult r;
    for (unsigned b = 0; b < 256; ++b) {
        bool eligible = hf_profile_unit(expected.card,b);
        if (profile.included[b] != eligible) return result(INVALID_ARGUMENT,"Source contains missing data or protected metadata.");
        if (!eligible) continue;
        ++r.total;
        if (!expected.known[b] || (profile.data[b] != expected.data[b] && !hf_writable(expected,b)))
            return result(PERMISSION_DENIED,"Preflight: target data unavailable or protected.");
        if (expected.card.ntag() && std::any_of(profile.data[b].begin()+4,profile.data[b].end(),[](uint8_t v){return v != 0;}))
            return result(INVALID_ARGUMENT,"Malformed source page.");
    }
    auto status = preflight(p,expected); if (status != OK) return failed(r,status,"Target changed before copy.");
    auto current = std::make_unique<HfSnapshot>(expected);
    for (unsigned b = 0; b < profile.unit_count; ++b) if (profile.included[b]) {
        status = update(p,*current,b,profile.data[b],r); if (status != OK) return failed(r,status,"Copy incomplete; read target again.");
        ++r.completed; p.progress(r.completed,r.total,"Copying data");
    }
    status = preflight(p,*current); if (status != OK) return failed(r,status,"Final copy readback failed.");
    r.verified = true; r.detail = "All copied data verified."; return r;
}
HfResult hf_ndef_area(const HfSnapshot& s, HfNdefArea& a) {
    a = {}; a.writable = true;
    auto reject = [&](const char* detail) { a = {}; return result(INVALID_ARGUMENT,detail); };
    if (s.card.classic()) {
        for (unsigned b : {1u,2u,3u}) if (!s.known[b]) return reject("Incomplete MAD directory.");
        Bytes mad(s.data[1].begin(),s.data[1].end()); mad.insert(mad.end(),s.data[2].begin(),s.data[2].end());
        uint8_t gpb = s.data[3][9];
        if ((gpb != 0xc1 && gpb != 0xc2) || mad_crc(mad) != mad[0] || !readable(s,1) || !readable(s,2))
            return reject("Invalid MAD CRC, C1/C2 or permissions; no formatting offered.");
        std::vector<unsigned> sectors;
        for (unsigned sec = 1; sec < 16; ++sec) if (mad[sec*2] == 3 && mad[sec*2+1] == 0xe1) sectors.push_back(sec);
        if (gpb == 0xc2) {
            if (s.card.family != HfFamily::Classic4K) return reject("MAD2 requires Classic4K.");
            Bytes mad2;
            for (unsigned b : {64u,65u,66u}) {
                if (!readable(s,b)) return reject("MAD2 unavailable or unreadable.");
                mad2.insert(mad2.end(),s.data[b].begin(),s.data[b].end());
            }
            if (!s.known[67] || s.data[67][9] != 0 || mad_crc(mad2) != mad2[0]) return reject("Invalid MAD2 CRC or 00 GPB.");
            for (unsigned sec = 17; sec < 40; ++sec) if (mad2[2+(sec-17)*2] == 3 && mad2[3+(sec-17)*2] == 0xe1) sectors.push_back(sec);
        }
        for (auto sec : sectors) for (unsigned b = first(sec); b < trailer(first(sec)); ++b) {
            if (!readable(s,b)) return reject("NDEF data unavailable or unreadable.");
            a.units.push_back(b);
            a.writable = a.writable && hf_writable(s,b) && (s.data[trailer(b)][9]&3) == 0;
        }
        if (a.units.empty()) return reject("MAD contains no NDEF sectors.");
    } else if (s.card.ntag()) {
        const auto& cc = s.data[3];
        if (!s.known[3] || cc[0] != 0xe1 || cc[1] != 0x10 || !cc[2] || cc[3]>>4 ||
            unsigned(cc[2])*8 > (s.card.units()-9)*4) return reject("Unsupported NTAG capability container.");
        for (unsigned b = 4; b < 4+unsigned(cc[2])*2; ++b) {
            if (!s.known[b]) return reject("Incomplete NTAG NDEF memory.");
            a.units.push_back(b); a.writable = a.writable && hf_writable(s,b);
        }
    } else return reject("Unsupported card family.");
    for (auto b : a.units) a.memory.insert(a.memory.end(),s.data[b].begin(),s.data[b].begin()+s.card.unit_size());
    if (a.memory.size() > ndef_limit) return reject("NDEF area exceeds local limit.");
    bool all_null = s.card.ntag();
    if (all_null) for (unsigned b = 4; b < s.card.units()-5; ++b) {
        if (!s.known[b] || std::any_of(s.data[b].begin(),s.data[b].begin()+4,[](uint8_t v){return v != 0;})) all_null = false;
    }
    if (all_null) { a.length_position = 1; a.capacity = unsigned(a.memory.size())-(a.memory.size()>256 ? 4 : 2); return result(OK,"Known all-NULL NTAG; first NDEF allowed."); }
    size_t cursor = 0, start = 0, payload = 0, end = 0, last = 0; bool found = false;
    while (cursor < a.memory.size()) {
        size_t at = cursor; uint8_t tag = a.memory[cursor++];
        if (!tag) continue;
        if (tag == 0xfe) break;
        if (cursor == a.memory.size()) return reject("Truncated NFC TLV.");
        unsigned length = a.memory[cursor++];
        if (length == 255) {
            if (a.memory.size()-cursor < 2) return reject("Truncated NFC TLV length.");
            length = unsigned(a.memory[cursor])<<8 | a.memory[cursor+1]; cursor += 2;
        }
        if (length > a.memory.size()-cursor) return reject("TLV exceeds memory.");
        if (tag == 1 || tag == 2) a.writable = a.writable && control(s,a,tag,Bytes(a.memory.begin()+cursor,a.memory.begin()+cursor+length));
        if (tag == 3 && !found) { found = true; start = at; payload = cursor; end = cursor+length; }
        cursor += length; last = cursor;
    }
    if (!found) return reject("No existing NDEF TLV; Classic formatting is forbidden.");
    a.prefix.assign(a.memory.begin(),a.memory.begin()+start);
    a.suffix.assign(a.memory.begin()+end,a.memory.begin()+std::max(end,last));
    std::string error;
    auto status = ndef_decode(Bytes(a.memory.begin()+payload,a.memory.begin()+end),a.records,error);
    if (status != OK) return reject("Malformed NDEF message.");
    a.length_position = unsigned(start+1);
    size_t overhead = a.prefix.size()+a.suffix.size()+(a.memory.size()>256 ? 4 : 2);
    a.capacity = unsigned(a.memory.size() > overhead ? a.memory.size()-overhead : 0);
    return result(OK,a.writable ? "NDEF area available." : "NDEF readable; layout or permissions prevent writes.");
}
HfResult hf_write_ndef(Port& p, const HfSnapshot& expected, const std::vector<NdefRecord>& records) {
    HfNdefArea area; auto r = hf_ndef_area(expected,area);
    if (r.status != OK) return r;
    if (!area.writable) return result(PERMISSION_DENIED,"NDEF area is protected or reserved layout unsupported.");
    Bytes message; std::string error;
    auto status = ndef_encode(records,message,error); if (status != OK) return result(status,"Invalid or oversized NDEF records.");
    Bytes final = area.prefix; final.push_back(3);
    if (message.size() < 255) final.push_back(uint8_t(message.size()));
    else final.insert(final.end(),{255,uint8_t(message.size()>>8),uint8_t(message.size())});
    final.insert(final.end(),message.begin(),message.end()); final.insert(final.end(),area.suffix.begin(),area.suffix.end());
    if (final.size() > area.memory.size()) return result(INVALID_ARGUMENT,"Records exceed target capacity.");
    if (final.size() < area.memory.size()) final.push_back(0xfe);
    final.resize(area.memory.size(),0);
    status = preflight(p,expected); if (status != OK) return failed(r,status,"Target changed before NDEF write.");
    if (area.records == records) { r.verified = true; r.detail = "Frozen NDEF already correct; no write."; return r; }
    auto current = std::make_unique<HfSnapshot>(expected); unsigned size = expected.card.unit_size();
    unsigned commit = area.length_position/size, offset = area.length_position%size;
    if (commit >= area.units.size()) return result(INVALID_ARGUMENT,"Invalid NDEF length position.");
    r.total = unsigned(area.units.size()); r.completed = 0;
    auto initial = current->data[area.units[commit]]; initial[offset] = 0;
    status = update(p,*current,area.units[commit],initial,r);
    if (status != OK) return failed(r,status,"NDEF staging failed; read target again.");
    Bytes staging = final; staging[area.length_position] = 0;
    for (size_t i = 0; i < area.units.size(); ++i) {
        std::array<uint8_t,16> value{}; std::copy_n(staging.begin()+i*size,size,value.begin());
        status = update(p,*current,area.units[i],value,r);
        if (status != OK) return failed(r,status,"NDEF incomplete; target may be empty.");
        ++r.completed; p.progress(r.completed,r.total,"Writing staged NDEF");
    }
    std::array<uint8_t,16> value{}; std::copy_n(final.begin()+commit*size,size,value.begin());
    status = update(p,*current,area.units[commit],value,r);
    if (status != OK) return failed(r,status,"NDEF length commit unverified.");
    status = preflight(p,*current); if (status != OK) return failed(r,status,"Final NDEF readback failed.");
    HfNdefArea verified; auto parsed = hf_ndef_area(*current,verified);
    if (parsed.status != OK || verified.records != records) return failed(r,WRITE_UNVERIFIED,"NDEF records did not verify.");
    r.verified = true; r.status = OK; r.detail = "NDEF committed length-last and verified."; return r;
}
}
