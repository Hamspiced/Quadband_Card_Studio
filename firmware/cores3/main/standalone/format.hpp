#pragma once
#include "port.hpp"

namespace qb::local {
// Only the canonical formatter receives this physical boundary. Ordinary Port
// writes retain their manufacturer, trailer, CC and configuration exclusions.
struct FormatIO {
    std::function<Status()> guard;
    std::function<Status(unsigned, const ClassicKey&, Bytes&)> read;
    std::function<Status(unsigned, const ClassicKey&, const Bytes&)> write;
};
inline Status canonical_format_impl(const HfIdentity& card, uint8_t flags, const Bytes& expected, FormatIO& io, bool& attempted) {
    const bool classic = card.family == HfFamily::Classic1K;
    const unsigned size = classic ? 16 : 4, count = classic ? 64 : card.units()-2;
    if (flags > 1 || (!classic && !card.ntag()) || expected.size() != size_t(count)*size)
        return INVALID_ARGUMENT;
    if (classic) {
        if (classic_geometry(card.sak,card.atqa.data(),card.uid_size) != 64) return UNSUPPORTED;
    } else {
        const uint8_t storage = card.family == HfFamily::Ntag213 ? 15 : card.family == HfFamily::Ntag215 ? 17 : 19;
        if (card.uid_size != 7 || card.sak || card.atqa != std::array<uint8_t,2>{{0x44,0}} ||
            card.version != std::array<uint8_t,8>{{0,4,4,2,1,0,storage,3}}) return UNSUPPORTED;
    }
    std::vector<Bytes> current(count);
    ClassicKey factory;
    auto equal = [&](unsigned address, const Bytes& actual, const Bytes& wanted) {
        const unsigned skip = classic && address%4 == 3 ? 6 : 0;
        return actual.size() == size && wanted.size() == size &&
            std::equal(actual.begin()+skip,actual.end(),wanted.begin()+skip);
    };
    for (unsigned b = 0; b < count; ++b) {
        auto s = io.guard(); if (s != OK) return s;
        s = io.read(b,factory,current[b]); if (s != OK) return s;
        Bytes wanted(expected.begin()+b*size,expected.begin()+(b+1)*size);
        if (!equal(b,current[b],wanted)) return STALE_DATA;
        if (classic && b%4 == 3) {
            if (current[b][6] != 0xff || current[b][7] != 7 || current[b][8] != 0x80 ||
                !std::all_of(current[b].begin()+10,current[b].end(),[](uint8_t v){return v == 255;}))
                return PERMISSION_DENIED;
        } else if (classic && b && !flags &&
                   std::any_of(current[b].begin(),current[b].end(),[](uint8_t v){return v != 0;}))
            return PERMISSION_DENIED;
    }
    if (classic && (current[3][9] == 0xc1 || current[3][9] == 0xc2)) return PERMISSION_DENIED;
    unsigned user_end = 0;
    uint8_t cc_size = 0;
    if (!classic) {
        const unsigned dynamic = card.units()-5;
        cc_size = card.family == HfFamily::Ntag213 ? 18 : card.family == HfFamily::Ntag215 ? 62 : 109;
        user_end = 4+unsigned(cc_size)*2;
        if (current[2][2] || current[2][3] ||
            std::any_of(current[3].begin(),current[3].end(),[](uint8_t v){return v != 0;}) ||
            std::any_of(current[dynamic].begin(),current[dynamic].begin()+3,[](uint8_t v){return v != 0;}) ||
            current[dynamic+1][3] != 255 || (current[dynamic+1][0]&0xc0) || (current[dynamic+2][0]&0xc0))
            return PERMISSION_DENIED;
        for (unsigned b = 4; b < dynamic; ++b)
            if (!flags && std::any_of(current[b].begin(),current[b].end(),[](uint8_t v){return v != 0;}))
                return PERMISSION_DENIED;
    }
    auto change = [&](unsigned b, const ClassicKey& key, const Bytes& desired, const ClassicKey& verify_key) {
        auto s = io.guard(); if (s != OK) return attempted ? WRITE_UNVERIFIED : s;
        const auto metadata = classic ? std::vector<unsigned>{trailer(b)} :
            std::vector<unsigned>{2,3,card.units()-5,card.units()-4,card.units()-3};
        for (unsigned unit : metadata) {
            Bytes permissions;
            s = io.read(unit,key,permissions);
            if (s == OK && !equal(unit,permissions,current[unit])) s = STALE_DATA;
            if (s != OK) return attempted ? WRITE_UNVERIFIED : s;
        }
        Bytes old;
        s = io.read(b,key,old);
        if (s == OK && !equal(b,old,current[b])) s = STALE_DATA;
        if (s != OK) return attempted ? WRITE_UNVERIFIED : s;
        if (old == desired) return OK;
        s = io.guard(); if (s != OK) return attempted ? WRITE_UNVERIFIED : s;
        attempted = true;
        s = io.write(b,key,desired); if (s != OK) return WRITE_UNVERIFIED;
        s = io.guard(); if (s != OK) return WRITE_UNVERIFIED;
        Bytes actual;
        s = io.read(b,verify_key,actual);
        if (s != OK || actual.size() != size) return WRITE_UNVERIFIED;
        if (classic && b%4 == 3) {
            if (!std::equal(actual.begin()+6,actual.begin()+10,desired.begin()+6)) return WRITE_UNVERIFIED;
        } else if (actual != desired) return WRITE_UNVERIFIED;
        current[b] = actual; return OK;
    };
    Status s;
    if (classic) {
        ClassicKey ndef, mad;
        ndef.bytes = {{0xd3,0xf7,0xd3,0xf7,0xd3,0xf7}};
        mad.bytes = {{0xa0,0xa1,0xa2,0xa3,0xa4,0xa5}};
        for (unsigned sector = 1; sector < 16; ++sector) {
            for (unsigned b = sector*4; b < sector*4+3; ++b) {
                Bytes data(16,0); if (b == 4) { data[0] = 3; data[2] = 0xfe; }
                s = change(b,factory,data,factory); if (s != OK) return s;
            }
            Bytes t{0xd3,0xf7,0xd3,0xf7,0xd3,0xf7,0x7f,7,0x88,0x40,255,255,255,255,255,255};
            s = change(sector*4+3,factory,t,ndef); if (s != OK) return s;
            for (unsigned b = sector*4; b < sector*4+3; ++b) {
                s = io.guard(); if (s != OK) return WRITE_UNVERIFIED;
                Bytes actual;
                s = io.read(b,ndef,actual);
                if (s != OK || actual != current[b]) return WRITE_UNVERIFIED;
            }
        }
        Bytes directory(32,0); directory[1] = 1;
        for (unsigned sector = 1; sector < 16; ++sector) { directory[sector*2] = 3; directory[sector*2+1] = 0xe1; }
        uint8_t crc = 0xc7;
        for (unsigned i = 1; i < directory.size(); ++i) {
            crc ^= directory[i];
            for (unsigned bit = 0; bit < 8; ++bit) crc = uint8_t((crc<<1) ^ (crc&128 ? 0x1d : 0));
        }
        directory[0] = crc;
        for (unsigned b = 1; b <= 2; ++b) {
            s = change(b,factory,Bytes(directory.begin()+(b-1)*16,directory.begin()+b*16),factory);
            if (s != OK) return s;
        }
        s = change(3,factory,Bytes{0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0x78,0x77,0x88,0xc1,255,255,255,255,255,255},mad);
        if (s != OK) return s;
    } else {
        for (unsigned b = 4; b < user_end; ++b) {
            Bytes data(4,0); if (b == 4) { data[0] = 3; data[2] = 0xfe; }
            s = change(b,factory,data,factory); if (s != OK) return s;
        }
        s = change(3,factory,Bytes{0xe1,0x10,cc_size,0},factory); if (s != OK) return s;
    }
    for (unsigned b = 0; b < count; ++b) {
        s = io.guard(); if (s != OK) return attempted ? WRITE_UNVERIFIED : s;
        ClassicKey key;
        if (classic) key.bytes = b < 4 ? std::array<uint8_t,6>{{0xa0,0xa1,0xa2,0xa3,0xa4,0xa5}} :
            std::array<uint8_t,6>{{0xd3,0xf7,0xd3,0xf7,0xd3,0xf7}};
        Bytes actual;
        s = io.read(b,key,actual);
        if (s != OK || !equal(b,actual,current[b])) return attempted ? WRITE_UNVERIFIED : s == OK ? STALE_DATA : s;
    }
    s = io.guard();
    return s != OK && attempted ? WRITE_UNVERIFIED : s;
}
inline Status canonical_format(const HfIdentity& card, uint8_t flags, const Bytes& expected, FormatIO& io) {
    bool attempted = false;
    try { return canonical_format_impl(card,flags,expected,io,attempted); }
    catch (Status status) { return attempted ? WRITE_UNVERIFIED : status; }
    catch (...) { return attempted ? WRITE_UNVERIFIED : IO_ERROR; }
}
}
