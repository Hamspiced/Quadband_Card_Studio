#include "uhf.hpp"
#include <limits>

namespace qb::local {
namespace {
UhfResult result(Status s) { return {s, s, 0, false, false}; }
uint16_t be16(const uint8_t* p) { return uint16_t((unsigned(p[0]) << 8) | p[1]); }
uint16_t tag_crc(const uint8_t* p, size_t n) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(p[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = uint16_t((unsigned(crc) << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc ^ 0xffff;
}
Bytes frame(uint8_t command, const Bytes& data) {
    Bytes out{0xbb, 0, command, uint8_t(data.size() >> 8), uint8_t(data.size())};
    out.insert(out.end(), data.begin(), data.end());
    unsigned sum = 0;
    for (size_t i = 1; i < out.size(); ++i) sum += out[i];
    out.push_back(uint8_t(sum)); out.push_back(0x7e);
    return out;
}
bool valid_frame(const uint8_t* p, size_t size) {
    if (size < 7 || p[0] != 0xbb || (p[1] != 1 && p[1] != 2) ||
        be16(p + 3) > 1024 || size != size_t(be16(p + 3)) + 7 || p[size - 1] != 0x7e)
        return false;
    unsigned sum = 0;
    for (size_t i = 1; i < size - 2; ++i) sum += p[i];
    return uint8_t(sum) == p[size - 2];
}
UhfResult module_error(const uint8_t* p, size_t size) {
    if (size != 1) return result(BAD_FRAME);
    Status status = IO_ERROR;
    switch (*p) {
    case 0x09: case 0x15: status = NO_CARD; break;
    case 0x16: status = AUTH_FAILED; break;
    case 0xa3: case 0xb3: case 0x17: status = INVALID_ARGUMENT; break;
    case 0xa4: case 0xb4: status = PERMISSION_DENIED; break;
    default: break;
    }
    auto r = result(status); r.module_error = *p; return r;
}
bool selectable(const UhfTag& tag) {
    return tag.epc_size >= 2 && tag.epc_size <= 28 && !(tag.epc_size % 2) &&
        (tag.pc[0] >> 3) * 2 == tag.epc_size;
}
Bytes selection(const UhfTag& tag) {
    Bytes p{1, 0, 0, 0, 0x10, uint8_t((tag.epc_size + 2) * 8), 0};
    p.insert(p.end(), tag.pc.begin(), tag.pc.end());
    p.insert(p.end(), tag.epc.begin(), tag.epc.begin() + tag.epc_size);
    return p;
}
Bytes memory_payload(UhfBank bank, uint16_t word, uint16_t words, const UhfPassword& password) {
    Bytes p(password.bytes.begin(), password.bytes.end());
    p.insert(p.end(), {uint8_t(bank), uint8_t(word >> 8), uint8_t(word),
                      uint8_t(words >> 8), uint8_t(words)});
    return p;
}
UhfResult memory_data(const Bytes& p, const UhfTag& tag, size_t size, const uint8_t*& data) {
    const size_t id = size_t(tag.epc_size) + 2;
    if (p.size() != 1 + id + size || p[0] != id) return result(BAD_FRAME);
    if (p[1] != tag.pc[0] || p[2] != tag.pc[1] ||
        !std::equal(tag.epc.begin(), tag.epc.begin() + tag.epc_size, p.begin() + 3))
        return result(STALE_DATA);
    data = p.data() + 1 + id;
    return result(OK);
}
UhfResult ack(const Bytes& p) { return result(p.size() == 1 && p[0] == 0 ? OK : BAD_FRAME); }
UhfResult unverified(UhfResult r) {
    r.cause = r.status; r.status = WRITE_UNVERIFIED;
    r.mutation_attempted = true; r.verified = false; return r;
}
}

bool UhfTag::same_identity(const UhfTag& other) const {
    return epc_size >= 2 && epc_size <= epc.size() && epc_size == other.epc_size &&
        generation == other.generation && pc == other.pc &&
        std::equal(epc.begin(), epc.begin() + epc_size, other.epc.begin());
}
void UhfMemory::clear() {
    secure_zero(data.data(), data.size()); secure_zero(&tag, sizeof(tag));
    bank = UhfBank::User; word = words = 0; valid = false;
}
void Uhf::clear() {
    secure_zero(tags_.data(), sizeof(tags_)); count_ = 0; dropped_ = 0;
    generation_ = 0; scanning_ = false; memory_.clear(); power_ = {};
    secure_zero(stream_.data(), stream_.size()); stream_size_ = 0;
}
UhfResult Uhf::check(uint32_t generation) const {
    if (port_.cancelled()) return result(CANCELLED);
    if (port_.generation(2) != generation) return result(STALE_GENERATION);
    return result(OK);
}
bool Uhf::known(const UhfTag& tag) const {
    for (size_t i = 0; i < count_; ++i)
        if (tags_[i].same_identity(tag)) return true;
    return false;
}
UhfResult Uhf::report(const uint8_t* p, size_t size) {
    if (size < 7) return result(BAD_FRAME);
    const unsigned n = (p[1] >> 3) * 2;
    if (n < 2 || n > 62 || size != n + 5 || tag_crc(p + 1, n + 2) != be16(p + size - 2))
        return result(BAD_FRAME);
    UhfTag tag;
    tag.epc_size = uint8_t(n); tag.pc = {p[1], p[2]};
    std::copy_n(p + 3, n, tag.epc.begin());
    tag.rssi = p[0] >= 128 ? int(p[0]) - 256 : p[0];
    tag.generation = generation_; tag.reads = 1;
    for (size_t i = 0; i < count_; ++i) {
        // A changed PC for the same EPC invalidates any old reviewed snapshot.
        if (tags_[i].epc_size == n &&
            std::equal(tag.epc.begin(), tag.epc.begin() + n, tags_[i].epc.begin())) {
            tag.reads = tags_[i].reads == std::numeric_limits<uint32_t>::max()
                ? tags_[i].reads : tags_[i].reads + 1;
            tags_[i] = tag; return result(OK);
        }
    }
    if (count_ < tags_.size()) tags_[count_++] = tag;
    else if (dropped_ != std::numeric_limits<uint32_t>::max()) ++dropped_;
    return result(OK);
}
UhfResult Uhf::command(uint8_t command_id, const Bytes& payload, Bytes& response, uint32_t generation) {
    response.clear();
    auto r = check(generation); if (!r) return r;
    if (!port_.supports(2, command_id)) return result(UNSUPPORTED);
    Bytes raw;
    Status status = port_.transact(2, frame(command_id, payload), raw);
    r = check(generation); if (!r) return r;
    if (status != OK) return result(status);
    if (raw.size() > max_buffer) return result(BAD_FRAME);
    bool found = false, notification = false;
    for (size_t at = 0; at < raw.size();) {
        if (raw.size() - at < 7) return result(BAD_FRAME);
        const size_t n = size_t(be16(raw.data() + at + 3)) + 7;
        if (n > raw.size() - at || !valid_frame(raw.data() + at, n)) return result(BAD_FRAME);
        const uint8_t* p = raw.data() + at;
        if (p[1] == 2) {
            if (!scanning_ || p[2] != 0x22) return result(BAD_FRAME);
            r = report(p + 5, n - 7); if (!r) return r;
            notification = true;
        } else if (p[2] == 0xff) {
            return module_error(p + 5, n - 7);
        } else {
            if (p[2] != command_id || found) return result(BAD_FRAME);
            response.assign(p + 5, p + n - 2); found = true;
        }
        at += n;
    }
    return result(found || (command_id == 0x22 && notification) ? OK : BAD_FRAME);
}
UhfResult Uhf::feed(const uint8_t* bytes, size_t size) {
    auto r = check(generation_); if (!r) { stream_size_ = 0; return r; }
    if (!scanning_) return result(BUSY);
    if ((!bytes && size) || size > max_buffer) return result(INVALID_ARGUMENT);
    for (size_t i = 0; i < size; ++i) {
        r = check(generation_); if (!r) { stream_size_ = 0; return r; }
        if (stream_size_ == stream_.size()) { stream_size_ = 0; return result(BAD_FRAME); }
        stream_[stream_size_++] = bytes[i];
        if (stream_[0] != 0xbb) { stream_size_ = 0; return result(BAD_FRAME); }
        if (stream_size_ < 5) continue;
        if ((stream_[1] != 2 || stream_[2] != 0x22) &&
            (stream_[1] != 1 || stream_[2] != 0xff)) {
            stream_size_ = 0; return result(BAD_FRAME);
        }
        const size_t total = size_t(be16(stream_.data() + 3)) + 7;
        if (total > stream_.size()) { stream_size_ = 0; return result(BAD_FRAME); }
        if (stream_size_ < total) continue;
        if (!valid_frame(stream_.data(), total)) { stream_size_ = 0; return result(BAD_FRAME); }
        if (stream_[1] == 2 && stream_[2] == 0x22) r = report(stream_.data() + 5, total - 7);
        else if (stream_[1] == 1 && stream_[2] == 0xff) r = module_error(stream_.data() + 5, total - 7);
        else r = result(BAD_FRAME);
        secure_zero(stream_.data(), stream_size_); stream_size_ = 0;
        if (!r && r.status != NO_CARD) return r;
    }
    return result(OK);
}
UhfResult Uhf::inventory_start() {
    if (scanning_) return result(BUSY);
    if (generation_ != port_.generation(2)) clear();
    generation_ = port_.generation(2);
    Bytes p; auto r = command(0x12, {1}, p, generation_);
    if (!r) return r;
    r = ack(p); if (r) scanning_ = true; return r;
}
UhfResult Uhf::inventory_poll() {
    if (!scanning_) return result(BUSY);
    Bytes p; auto r = command(0x22, {}, p, generation_);
    if (r.status == NO_CARD) return result(OK);
    if (r && !p.empty()) return result(BAD_FRAME);
    return r;
}
UhfResult Uhf::inventory_stop() {
    scanning_ = false; stream_size_ = 0;
    Bytes p; auto r = command(0x28, {}, p, generation_);
    return r ? ack(p) : r;
}
UhfResult Uhf::select(const UhfTag& tag) {
    auto r = check(tag.generation); if (!r) return r;
    if (scanning_) return result(BUSY);
    if (!selectable(tag)) return result(INVALID_ARGUMENT);
    if (!known(tag)) return result(STALE_DATA);
    const Bytes expected = selection(tag); Bytes p;
    r = command(0x0c, expected, p, tag.generation); if (!r) return r;
    r = ack(p); if (!r) return r;
    r = command(0x0b, {}, p, tag.generation); if (!r) return r;
    if (p != expected) return result(STALE_DATA);
    r = command(0x12, {0}, p, tag.generation); return r ? ack(p) : r;
}
UhfResult Uhf::read_selected(const UhfTag& tag, UhfBank bank, uint16_t word, uint16_t words,
                             const UhfPassword& password, UhfMemory& output) {
    output.clear();
    if (uint8_t(bank) > 3 || words < 1 || words > 64 || unsigned(word) + words > 0x10000)
        return result(INVALID_ARGUMENT);
    Bytes p; auto r = command(0x39, memory_payload(bank, word, words, password), p, tag.generation);
    if (!r) return r;
    const uint8_t* data = nullptr;
    r = memory_data(p, tag, words * 2, data); if (!r) return r;
    output.tag = tag; output.bank = bank; output.word = word; output.words = words;
    std::copy_n(data, words * 2, output.data.begin()); output.valid = true;
    return result(OK);
}
UhfResult Uhf::read(const UhfTag& tag, UhfBank bank, uint16_t word, uint16_t words,
                    const UhfPassword& password, UhfMemory& output) {
    // Copy first: callers may pass the service's own memory model as the expected target.
    const UhfTag selected = tag;
    output.clear(); memory_.clear();
    if (uint8_t(bank) > 3 || words < 1 || words > 64 || unsigned(word) + words > 0x10000)
        return result(INVALID_ARGUMENT);
    auto r = select(selected); if (!r) return r;
    r = read_selected(selected, bank, word, words, password, output);
    if (r) memory_ = output;
    return r;
}
UhfResult Uhf::mutate(const UhfTag& tag, const UhfMemory& expected, const Bytes& desired,
                      const UhfPassword& password, const UhfTag& after, UhfMemory& output) {
    const UhfMemory prior = expected;
    const UhfTag before = tag;
    const UhfTag target = after;
    output.clear(); memory_.clear();
    auto failed = [&](UhfResult failure) {
        output.clear(); clear(); return unverified(failure);
    };
    if (!prior.valid || !prior.tag.same_identity(before) || prior.words < 1 || prior.words > 64 ||
        desired.size() < 2 || desired.size() > 64 || desired.size() % 2 ||
        unsigned(prior.word) + desired.size() / 2 > 0x10000)
        return result(INVALID_ARGUMENT);
    auto r = select(before); if (!r) return r;
    UhfMemory actual;
    r = read_selected(before, prior.bank, prior.word, prior.words, password, actual); if (!r) return r;
    if (!std::equal(prior.data.begin(), prior.data.begin() + prior.words * 2, actual.data.begin()))
        return result(STALE_DATA);
    UhfMemory pc;
    r = read_selected(before, UhfBank::Epc, 1, 1, password, pc); if (!r) return r;
    if (pc.data[0] != before.pc[0] || pc.data[1] != before.pc[1]) return result(STALE_DATA);
    Bytes p;
    r = command(0x0b, {}, p, before.generation); if (!r) return r;
    if (p != selection(before)) return result(STALE_DATA);
    r = check(before.generation); if (!r) return r;
    port_.progress(0, 1, "Writing UHF; do not remove tag");
    Bytes payload = memory_payload(prior.bank, prior.word, uint16_t(desired.size() / 2), password);
    payload.insert(payload.end(), desired.begin(), desired.end());
    r = command(0x49, payload, p, before.generation);
    if (!r) return failed(r);
    const uint8_t* status = nullptr;
    r = memory_data(p, before, 1, status);
    if (!r) r = memory_data(p, target, 1, status);
    if (!r || !status || *status != 0) return failed(r ? result(IO_ERROR) : r);
    // Only the acknowledged candidate identity is made available for exact re-selection.
    for (size_t i = 0; i < count_; ++i) if (tags_[i].same_identity(before)) { tags_[i] = target; break; }
    r = select(target);
    if (r) r = read_selected(target, prior.bank, prior.word, uint16_t(desired.size() / 2), password, output);
    if (!r) return failed(r);
    if (!std::equal(desired.begin(), desired.end(), output.data.begin())) {
        return failed(result(STALE_DATA));
    }
    memory_ = output; port_.progress(1, 1, "UHF write verified");
    r = result(OK); r.mutation_attempted = true; r.verified = true; return r;
}
UhfResult Uhf::write_user(const UhfTag& tag, const UhfMemory& expected,
                          const uint8_t* desired, size_t size, const UhfPassword& password,
                          UhfMemory& output) {
    if (!desired || expected.bank != UhfBank::User || expected.words < 1 ||
        expected.words > 32 || size != size_t(expected.words) * 2) return result(INVALID_ARGUMENT);
    return mutate(tag, expected, Bytes(desired, desired + size), password, tag, output);
}
UhfResult Uhf::rename_epc(const UhfTag& tag, const UhfMemory& expected,
                          const uint8_t* epc, size_t size, const UhfPassword& password, UhfTag& output) {
    if (!epc || size < 2 || size > 28 || size % 2 || !selectable(tag) ||
        !expected.valid || expected.bank != UhfBank::Epc || expected.word != 1 ||
        expected.words < (std::max(size_t(tag.epc_size), size) + 2) / 2 ||
        expected.words > 64 || expected.data[0] != tag.pc[0] ||
        expected.data[1] != tag.pc[1] ||
        !std::equal(tag.epc.begin(), tag.epc.begin() + tag.epc_size, expected.data.begin() + 2))
        return result(INVALID_ARGUMENT);
    UhfTag after = tag;
    after.epc.fill(0); after.epc_size = uint8_t(size);
    std::copy_n(epc, size, after.epc.begin());
    after.pc[0] = uint8_t((tag.pc[0] & 7) | ((size / 2) << 3));
    Bytes desired(after.pc.begin(), after.pc.end());
    desired.insert(desired.end(), epc, epc + size);
    UhfMemory verified;
    auto r = mutate(tag, expected, desired, password, after, verified);
    output = r ? after : UhfTag{};
    return r;
}
UhfResult Uhf::read_power() {
    const uint32_t gen = port_.generation(2);
    power_ = {}; Bytes p;
    auto r = command(0xb7, {}, p, gen); if (!r) return r;
    if (p.size() != 2 || be16(p.data()) < 1800 || be16(p.data()) > 2600) return result(BAD_FRAME);
    power_ = {be16(p.data()), gen, true}; return result(OK);
}
UhfResult Uhf::set_power(const UhfPower& expected, unsigned dbm) {
    const UhfPower prior = expected;
    if (!prior.valid || dbm < 18 || dbm > 26) return result(INVALID_ARGUMENT);
    auto r = check(prior.generation); if (!r) return r;
    r = read_power(); if (!r) return r;
    if (power_.generation != prior.generation || power_.centidbm != prior.centidbm) return result(STALE_DATA);
    Bytes p; const unsigned value = dbm * 100;
    r = command(0xb6, {uint8_t(value >> 8), uint8_t(value)}, p, prior.generation);
    power_ = {};
    if (!r) return unverified(r);
    r = ack(p); if (!r) return unverified(r);
    r = read_power(); if (!r) return unverified(r);
    if (power_.generation != prior.generation || power_.centidbm != value) {
        power_ = {}; return unverified(result(STALE_DATA));
    }
    r = result(OK); r.mutation_attempted = true; r.verified = true; return r;
}
}
