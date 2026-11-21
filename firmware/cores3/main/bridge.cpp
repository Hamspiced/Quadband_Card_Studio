#include "bridge.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace qb {
uint16_t le16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
uint32_t le32(const uint8_t* p) { return le16(p) | uint32_t(le16(p + 2)) << 16; }
static void put(Bytes& b, uint64_t n, unsigned count) {
    while (count--) { b.push_back(uint8_t(n)); n >>= 8; }
}
uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
Bytes Frame::encode() const {
    if (payload.size() > max_payload) return {};
    Bytes b{0x51, 0x42, 1, kind};
    put(b, session, 8); put(b, id, 4);
    b.push_back(channel); b.push_back(op);
    put(b, status, 2); put(b, generation, 4); put(b, payload.size(), 2);
    b.insert(b.end(), payload.begin(), payload.end());
    put(b, crc32(b.data(), b.size()), 4);
    return b;
}
void Decoder::reset() { std::fill(buffer.begin(), buffer.end(), 0); buffer.clear(); lost = false; }
bool Decoder::feed(const uint8_t* p, size_t n, const std::function<void(const Frame&)>& consume) {
    if (lost || n > max_buffer - buffer.size()) { lost = true; return false; }
    buffer.insert(buffer.end(), p, p + n);
    while (buffer.size() >= 26) {
        auto* h = buffer.data();
        size_t len = le16(h + 24), total = len + 30;
        if (h[0] != 0x51 || h[1] != 0x42 || h[2] != 1 || h[3] > 2 || len > max_payload) {
            lost = true; return false;
        }
        if (buffer.size() < total) break;
        if (crc32(h, total - 4) != le32(h + total - 4)) { lost = true; return false; }
        Frame f;
        f.kind = h[3]; f.session = le32(h + 4) | uint64_t(le32(h + 8)) << 32;
        f.id = le32(h + 12); f.channel = h[16]; f.op = h[17];
        f.status = Status(le16(h + 18)); f.generation = le32(h + 20);
        f.payload.assign(h + 26, h + 26 + len);
        consume(f);
        std::fill(buffer.begin(), buffer.begin() + total, 0);
        buffer.erase(buffer.begin(), buffer.begin() + total);
    }
    return true;
}
void Session::clear() { nonce = 0; last = pending = 0; cancelled = true; ++epoch; }
Status Session::admit(const Frame& f, bool authorized, const std::array<Slot, 3>& slots) {
    if (!authorized) return NOT_AUTHORIZED;
    if (f.kind || f.status || !f.id || f.channel > 3) return INVALID_ARGUMENT;
    if (f.channel == 0 && f.op == 1) {
        if (pending) return BUSY;
        if (f.session || f.generation || f.payload.size() != 8) return INVALID_ARGUMENT;
        last = f.id;
        return OK;
    }
    if (!nonce || nonce != f.session || f.id <= last || last == UINT32_MAX) return BAD_SESSION;
    last = f.id;
    if (!f.channel) {
        if (f.generation) return INVALID_ARGUMENT;
        return OK;
    }
    const auto& s = slots[f.channel - 1];
    if (!f.generation || f.generation != s.generation) return STALE_GENERATION;
    if (!s.present) return UNAVAILABLE;
    if (pending) return BUSY;
    pending = f.id; cancelled = false;
    return OK;
}
unsigned trailer(unsigned b) { return b < 128 ? b / 4 * 4 + 3 : 128 + (b - 128) / 16 * 16 + 15; }
unsigned access_group(unsigned b) { return b < 128 ? b % 4 : (b % 16 == 15 ? 3 : b % 16 / 5); }
bool classic_data(unsigned blocks, unsigned b) {
    return (blocks == 64 || blocks == 256) && b && b < blocks && trailer(b) != b;
}
bool classic_write_allowed(unsigned b, uint8_t key, const uint8_t t[16]) {
    if (key != 0x60 && key != 0x61) return false;
    const uint8_t c1 = t[7] >> 4, c2 = t[8] & 15, c3 = t[8] >> 4;
    if (((t[6] & 15) ^ c1) != 15 || ((t[6] >> 4) ^ c2) != 15 || ((t[7] & 15) ^ c3) != 15) return false;
    unsigned g = access_group(b);
    unsigned code = ((c1 >> g & 1) << 2) | ((c2 >> g & 1) << 1) | (c3 >> g & 1);
    unsigned tc = ((c1 >> 3 & 1) << 2) | ((c2 >> 3 & 1) << 1) | (c3 >> 3 & 1);
    if (key == 0x61 && (tc == 0 || tc == 1 || tc == 2)) return false;
    return code == 0 || (key == 0x61 && (code == 3 || code == 4 || code == 6));
}
NtagLayout ntag(unsigned pages) {
    switch (pages) {
    case 45: return {45, 39, 40};
    case 135: return {135, 129, 130};
    case 231: return {231, 225, 226};
    default: return {};
    }
}
bool ntag_write_allowed(unsigned pages, unsigned p, const uint8_t initial[16],
                        const uint8_t dyn[4], const uint8_t cfg[8], bool auth) {
    auto l = ntag(pages);
    if (!l.pages || p < 4 || p > l.last_user) return false;
    const auto* cc = initial + 12;
    if (cc[0] != 0xe1 || cc[1] != 0x10 || cc[3] != 0 || !cc[2] ||
        unsigned(cc[2]) * 8 > (l.last_user - 3) * 4) return false;
    if (p <= 15 && (initial[10 + p / 8] & (1u << (p % 8)))) return false;
    if (p >= 16) {
        unsigned group = (p - 16) / (pages == 45 ? 2 : 16);
        if (group >= (pages == 45 ? 12u : pages == 135 ? 8u : 14u) ||
            (dyn[group / 8] & (1u << (group % 8)))) return false;
    }
    if (p >= cfg[3] && !auth) return false;
    return true;
}
Status validate_hf(const Frame& f) {
    const auto& p = f.payload;
    if (f.op == 16 || f.op == 21) return p.empty() ? OK : INVALID_ARGUMENT;
    if (f.op == 22 || f.op == 23) {
        if (p.empty() || (p[0] != 4 && p[0] != 7 && p[0] != 10)) return INVALID_ARGUMENT;
        const size_t offset = size_t(p[0])+1;
        if (p.size() <= offset || p[offset] > 1) return INVALID_ARGUMENT;
        const size_t snapshot = p.size()-offset-1;
        return f.op == 22 ? (snapshot == 1024 ? OK : INVALID_ARGUMENT) :
            (p[0] == 7 && (snapshot == 172 || snapshot == 532 || snapshot == 916) ? OK : INVALID_ARGUMENT);
    }
    if (f.op < 17 || f.op > 20) return UNSUPPORTED;
    if (p.empty() || (p[0] != 4 && p[0] != 7 && p[0] != 10)) return INVALID_ARGUMENT;
    size_t offset = 1 + p[0] + 2;
    if (f.op == 17 || f.op == 18) {
        if (p.size() != offset + (f.op == 17 ? 7 : 39)) return INVALID_ARGUMENT;
        return p[offset] == 0x60 || p[offset] == 0x61 ? OK : INVALID_ARGUMENT;
    }
    if (f.op == 19) {
        if (p.size() <= offset || !p[offset] || p[offset] > 16) return INVALID_ARGUMENT;
        ++offset;
    }
    if (p.size() <= offset || p[offset] > 2) return INVALID_ARGUMENT;
    size_t credential = p[offset] == 0 ? 0 : p[offset] == 1 ? 4 : 6;
    return p.size() == offset + 1 + credential + (f.op == 20 ? 8 : 0) ? OK : INVALID_ARGUMENT;
}
static uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
Status validate_uhf(const Bytes& b) {
    if (b.size() < 7 || b[0] != 0xbb || b[1] != 0 || b.back() != 0x7e ||
        b.size() != size_t(be16(b.data() + 3)) + 7) return INVALID_ARGUMENT;
    uint8_t sum = 0; for (size_t i = 1; i < b.size() - 2; ++i) sum += b[i];
    if (sum != b[b.size() - 2]) return INVALID_ARGUMENT;
    const auto* p = b.data() + 5; size_t n = b.size() - 7;
    switch (b[2]) {
    case 0x03: return n == 1 && p[0] <= 2 ? OK : INVALID_ARGUMENT;
    case 0x08: case 0x0b: case 0x22: case 0x28: case 0xb7:
        return n == 0 ? OK : INVALID_ARGUMENT;
    case 0x12: return n == 1 && p[0] <= 2 ? OK : INVALID_ARGUMENT;
    case 0x0c: return n >= 7 && p[0] <= 7 && p[5] <= 240 && p[6] <= 1 &&
        n == size_t(7 + (p[5] + 7) / 8) ? OK : INVALID_ARGUMENT;
    case 0xb6: return n == 2 && be16(p) >= 1800 && be16(p) <= 2600 ? OK : PERMISSION_DENIED;
    case 0x39: case 0x49: {
        if (n < 9) return INVALID_ARGUMENT;
        unsigned bank = p[4], addr = be16(p + 5), count = be16(p + 7);
        if (!count || count > (b[2] == 0x39 ? 64u : 32u) || addr + count > 65536 || bank > 3) return INVALID_ARGUMENT;
        if (b[2] == 0x39) return n == 9 ? OK : INVALID_ARGUMENT;
        if ((bank != 1 && bank != 3) || (bank == 1 && addr < 1)) return PERMISSION_DENIED;
        return n == 9 + count * 2 ? OK : INVALID_ARGUMENT;
    }
    default: return PERMISSION_DENIED;
    }
}
uint32_t usb_timeout_ms(unsigned command) {
    return command >= 3000 && command <= 3021 ? 4200 : 2500;
}
Status validate_chameleon(const Bytes& b) {
    if (b.size() < 10 || b[0] != 0x11 || uint8_t(b[0] + b[1]) != 0 ||
        be16(b.data() + 4) != 0 || b.size() != size_t(be16(b.data() + 6)) + 10) return INVALID_ARGUMENT;
    uint8_t h = 0, all = 0;
    for (size_t i = 0; i < 9; ++i) h += b[i];
    for (size_t i = 9; i < b.size(); ++i) all += b[i];
    if (h || all) return INVALID_ARGUMENT;
    auto cmd = be16(b.data() + 2); auto n = b.size() - 10; const auto* p = b.data() + 9;
    switch (cmd) {
    case 1000: case 1002: case 1017: case 1033: case 1035:
    case 2000: case 3000: case 3004: case 3014: case 3019: case 3021:
        return n == 0 ? OK : INVALID_ARGUMENT;
    case 3002: return n == 1 && (p[0] <= 30 || p[0] == 42) ? OK : INVALID_ARGUMENT;
    case 3010: return n == 1 && p[0] == 0 ? OK : INVALID_ARGUMENT;
    case 1001: return n == 1 && p[0] <= 1 ? OK : INVALID_ARGUMENT;
    case 2008: return n == 8 && (p[0] == 0x60 || p[0] == 0x61) ? OK : INVALID_ARGUMENT;
    case 2009: return n == 24 && (p[0] == 0x60 || p[0] == 0x61) &&
        classic_data(256, p[1]) ? OK : PERMISSION_DENIED;
    case 3016:
        if (n != 11 || p[0] > 6) return PERMISSION_DENIED;
        for (size_t i = 5; i < 11; ++i) if (p[i]) return PERMISSION_DENIED;
        if (p[0] == 0 && (p[4] & 0x10)) return PERMISSION_DENIED;
        return OK;
    default: return PERMISSION_DENIED;
    }
}
Status take_uhf_frame(Bytes& buffer, Bytes& frame) {
    if (buffer.size() > max_buffer) return IO_ERROR;
    if (buffer.empty()) return BUSY;
    if (buffer[0] != 0xbb) return IO_ERROR;
    if (buffer.size() < 5) return BUSY;
    size_t total = be16(buffer.data() + 3) + 7;
    if ((buffer[1] != 1 && buffer[1] != 2) || total > max_payload) return IO_ERROR;
    if (buffer.size() < total) return BUSY;
    uint8_t sum = 0;
    for (size_t i = 1; i < total - 2; ++i) sum += buffer[i];
    if (sum != buffer[total - 2] || buffer[total - 1] != 0x7e) return IO_ERROR;
    frame.assign(buffer.begin(), buffer.begin() + total);
    buffer.erase(buffer.begin(), buffer.begin() + total); return OK;
}
unsigned classic_geometry(uint8_t sak, const uint8_t atqa[2], unsigned uid_size) {
    if (uid_size != 4 && uid_size != 7 && uid_size != 10) return 0;
    unsigned base = atqa[0] & 0x3f;
    if (atqa[1] != 0 || (atqa[0] & 0xc0) != (uid_size == 4 ? 0 : uid_size == 7 ? 0x40 : 0x80)) return 0;
    return sak == 8 && base == 4 ? 64 : sak == 0x18 && base == 2 ? 256 : 0;
}
bool exact_uhf_selection(const Bytes& p) {
    if (p.size() < 11 || p.size() > 37 || p[0] != 1 || p[1] || p[2] || p[3] || p[4] != 16 ||
        p[6] || p[5] != (p.size() - 7) * 8 || (p.size() - 9) % 2) return false;
    return unsigned(p[7] >> 3) * 2 == p.size() - 9;
}
void UhfGuard::clear() { std::fill(selection.begin(), selection.end(), 0); selection.clear(); verified = mode = false; }
void UhfGuard::observe(const Bytes& request, const Bytes& response) {
    if (request.size() < 7 || response.size() < 7 || response[2] == 0xff) { clear(); return; }
    const unsigned cmd = request[2];
    Bytes p(request.begin() + 5, request.end() - 2), r(response.begin() + 5, response.end() - 2);
    if (cmd == 0x0c) {
        clear();
        if (r == Bytes{0} && exact_uhf_selection(p)) selection = p;
    } else if (cmd == 0x0b) verified = !selection.empty() && r == selection;
    else if (cmd == 0x12) mode = p == Bytes{0} && r == Bytes{0};
    else if (cmd == 0x49) clear();
}
}
