#include "lf.hpp"

namespace qb::local {
namespace {
constexpr std::array<LFDescriptor, 8> formats{{
    {LFFormat::EM410x, "EM410x", 3000, 5, false, true},
    {LFFormat::Electra, "EM410x Electra", 3000, 13, false, true},
    {LFFormat::HIDProx, "HID Prox", 3002, 13, false, false},
    {LFFormat::Viking, "Viking", 3004, 4, false, true},
    {LFFormat::IoProx, "ioProx", 3010, 16, false, true},
    {LFFormat::PACStanley, "PAC / Stanley", 3014, 8, true, true},
    {LFFormat::Jablotron, "Jablotron", 3019, 5, false, true},
    {LFFormat::Pyramid, "Pyramid", 3021, 16, false, false}
}};
bool hid_hint(uint8_t value) { return value <= 30 || value == 42; }
Status live(const Port& port, uint32_t generation) {
    if (port.cancelled()) return CANCELLED;
    return generation && port.generation(3) == generation ? OK : STALE_GENERATION;
}
uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
Bytes frame(uint16_t command, const Bytes& payload) {
    Bytes out{0x11, 0xef, uint8_t(command >> 8), uint8_t(command), 0, 0, 0, uint8_t(payload.size()), 0};
    uint8_t sum = 0;
    for (auto b : out) sum += b;
    out[8] = uint8_t(0 - sum);
    out.insert(out.end(), payload.begin(), payload.end());
    sum = 0;
    for (auto b : out) sum += b;
    out.push_back(uint8_t(0 - sum));
    return out;
}
Status command(Port& port, uint32_t generation, uint16_t cmd, const Bytes& payload,
               uint16_t expected_status, Bytes& data) {
    data.clear();
    auto status = live(port, generation);
    if (status != OK) return status;
    if (!port.supports(3, cmd)) return UNSUPPORTED;
    Bytes request = frame(cmd, payload), response;
    status = validate_chameleon(request);
    if (status != OK) return status;
    status = port.transact(3, request, response);
    if (status != OK) return status;
    status = live(port, generation);
    if (status != OK) return status;
    if (response.size() < 10 || response.size() > 26 ||
        response[0] != 0x11 || response[1] != 0xef ||
        be16(response.data() + 2) != cmd ||
        size_t(be16(response.data() + 6)) + 10 != response.size()) return IO_ERROR;
    uint8_t header_sum = 0, sum = 0;
    for (size_t i = 0; i < response.size(); ++i) {
        sum += response[i];
        if (i < 9) header_sum += response[i];
    }
    if (header_sum || sum) return IO_ERROR;
    auto reply_status = be16(response.data() + 4);
    if (reply_status == 1 || reply_status == 0x41) return NO_CARD;
    if (reply_status != expected_status) return IO_ERROR;
    data.assign(response.begin() + 9, response.end() - 1);
    return OK;
}
Status reader_mode(Port& port, uint32_t generation) {
    Bytes data;
    auto status = command(port, generation, 1001, {1}, 0x68, data);
    return status == OK && !data.empty() ? IO_ERROR : status;
}
Status scan_current(Port& port, uint32_t generation, LFFormat format, uint8_t hint, LFCard& out) {
    out.clear();
    const auto* d = lf_descriptor(format);
    if (!d || (format == LFFormat::HIDProx ? !hid_hint(hint) : hint != 0)) return INVALID_ARGUMENT;
    Bytes payload, data;
    if (format == LFFormat::HIDProx || format == LFFormat::IoProx) payload.push_back(hint);
    auto status = command(port, generation, d->scan_command, payload, 0x40, data);
    return status == OK ? lf_decode_response(format, data, out) : status;
}
unsigned parity(unsigned value) {
    unsigned p = 0;
    while (value) { p ^= value & 1; value >>= 1; }
    return p;
}
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}
const std::array<LFDescriptor, 8>& lf_formats() { return formats; }
const LFDescriptor* lf_descriptor(LFFormat format) {
    for (const auto& d : formats) if (d.format == format) return &d;
    return nullptr;
}
bool lf_scannable(const Port& port, LFFormat format) {
    const auto* d = lf_descriptor(format);
    return d && port.supports(3, 1001) && port.supports(3, d->scan_command);
}
bool lf_writable(const Port& port, LFFormat format) {
    const auto* d = lf_descriptor(format);
    return d && d->writable && lf_scannable(port, format) && port.supports(3, 3016);
}
void LFCard::clear() {
    secure_zero(data.data(), data.size());
    size = 0;
    format = LFFormat::EM410x;
}
bool LFCard::valid() const {
    const auto* d = lf_descriptor(format);
    if (!d || size != d->id_length) return false;
    if (format == LFFormat::HIDProx && (!data[0] || !hid_hint(data[0]))) return false;
    if (d->requires_ascii) {
        for (unsigned i = 0; i < size; ++i) if (data[i] < 32 || data[i] > 126) return false;
    }
    return true;
}
bool LFCard::matches(const LFCard& other) const {
    if (!valid() || !other.valid() || format != other.format) return false;
    return std::equal(data.begin(), data.begin() + (format == LFFormat::IoProx ? 12 : size), other.data.begin());
}
Status lf_parse_id(LFFormat format, const std::string& text, LFCard& out) {
    out.clear();
    const auto* d = lf_descriptor(format);
    if (!d) return INVALID_ARGUMENT;
    LFCard parsed;
    parsed.format = format;
    parsed.size = d->id_length;
    if (d->requires_ascii) {
        if (text.size() != parsed.size) return INVALID_ARGUMENT;
        std::copy(text.begin(), text.end(), parsed.data.begin());
    } else {
        if (text.size() != size_t(parsed.size) * 2) return INVALID_ARGUMENT;
        for (unsigned i = 0; i < parsed.size; ++i) {
            int hi = hex_digit(text[i * 2]), lo = hex_digit(text[i * 2 + 1]);
            if (hi < 0 || lo < 0) return INVALID_ARGUMENT;
            parsed.data[i] = uint8_t((hi << 4) | lo);
        }
    }
    if (!parsed.valid()) return INVALID_ARGUMENT;
    out = parsed;
    return OK;
}
LFCard lf_default_test_card() {
    LFCard card;
    card.size = 5;
    for (unsigned i = 0; i < 5; ++i) card.data[i] = uint8_t(i + 1);
    return card;
}
Status lf_decode_response(LFFormat requested, const Bytes& data, LFCard& out) {
    out.clear();
    LFCard card;
    card.format = requested;
    size_t offset = 0, length = data.size();
    if (requested == LFFormat::EM410x || requested == LFFormat::Electra) {
        card.format = LFFormat::EM410x;
        if (length == 5) {}
        else if (length == 7 && be16(data.data()) >= 100 && be16(data.data()) <= 103) {
            offset = 2; length = 5;
        } else if (length == 15 && be16(data.data()) == 104) {
            card.format = LFFormat::Electra; offset = 2; length = 13;
        } else return IO_ERROR;
    } else if (requested == LFFormat::HIDProx && length == 16) {
        length = 13;
    }
    if (length > card.data.size()) return IO_ERROR;
    card.size = uint8_t(length);
    std::copy_n(data.begin() + offset, length, card.data.begin());
    if (!card.valid()) return IO_ERROR;
    out = card;
    return OK;
}
void LFSnapshot::clear() {
    card.clear();
    requested_format = LFFormat::EM410x;
    hint = 0; generation = 0; present = false;
}
Status lf_scan(Port& port, LFFormat format, LFSnapshot& out, uint8_t hint) {
    out.clear();
    if (!lf_descriptor(format) || (format == LFFormat::HIDProx ? !hid_hint(hint) : hint != 0))
        return INVALID_ARGUMENT;
    if (!lf_scannable(port, format)) return UNSUPPORTED;
    const auto generation = port.generation(3);
    auto status = reader_mode(port, generation);
    if (status != OK) return status;
    port.progress(0, 1, "Scanning LF identity");
    LFCard card;
    status = scan_current(port, generation, format, hint, card);
    if (status != OK && status != NO_CARD) return status;
    out.requested_format = format; out.hint = hint; out.generation = generation;
    out.present = status == OK;
    if (out.present) out.card = card;
    port.progress(1, 1, out.present ? "LF identity captured" : "No LF identity detected");
    const auto after = live(port, generation);
    if (after != OK) { out.clear(); return after; }
    return status;
}
Status lf_encode_t5577(const LFCard& card, LFEncoding& out) {
    out.clear();
    if (!card.valid()) return INVALID_ARGUMENT;
    const auto* d = lf_descriptor(card.format);
    if (!d->writable) return UNSUPPORTED;
    std::array<uint8_t, 24> raw{};
    struct Wipe { std::array<uint8_t, 24>& bytes; ~Wipe() { secure_zero(bytes.data(), bytes.size()); } } wipe{raw};
    unsigned length = 0;
    uint32_t config = 0;
    if (card.format == LFFormat::EM410x || card.format == LFFormat::Electra) {
        uint64_t bits = 0x1ff;
        unsigned columns = 0;
        for (unsigned i = 0; i < 5; ++i) {
            for (unsigned shift : {4u, 0u}) {
                unsigned n = (card.data[i] >> shift) & 15;
                bits = (bits << 5) | (n << 1) | parity(n);
                columns ^= n;
            }
        }
        bits = (bits << 5) | (columns << 1);
        for (unsigned i = 0; i < 8; ++i) raw[i] = uint8_t(bits >> ((7 - i) * 8));
        length = card.format == LFFormat::Electra ? 16 : 8;
        if (length == 16) std::copy_n(card.data.begin() + 5, 8, raw.begin() + 8);
        config = 0x00148000 | ((length / 4) << 5);
    } else if (card.format == LFFormat::Viking) {
        length = 8; raw[0] = 0xf2; uint8_t checksum = 0x5a;
        for (unsigned i = 0; i < 4; ++i) { raw[i + 3] = card.data[i]; checksum ^= card.data[i]; }
        raw[7] = checksum; config = 0x00088040;
    } else if (card.format == LFFormat::IoProx) {
        std::copy_n(card.data.begin() + 4, 8, raw.begin());
        length = 8; config = 0x00147040;
    } else if (card.format == LFFormat::Jablotron) {
        length = 8; raw[0] = raw[1] = 0xff;
        unsigned sum = 0;
        for (unsigned i = 0; i < 5; ++i) { raw[i + 2] = card.data[i]; sum += card.data[i]; }
        raw[7] = uint8_t(sum ^ 0x3a); config = 0x00158040;
    } else {
        unsigned bit = 0;
        auto put = [&](unsigned value) {
            if (value) raw[bit / 8] |= uint8_t(1u << (7 - bit % 8));
            ++bit;
        };
        for (unsigned i = 0; i < 8; ++i) put(1);
        uint8_t checksum = 0;
        for (unsigned i = 0; i < 8; ++i) checksum ^= card.data[i];
        for (unsigned i = 0; i < 12; ++i) {
            uint8_t byte = i == 0 ? 2 : i == 1 ? '2' : i == 2 ? '0' : i == 11 ? checksum : card.data[i - 3];
            put(0);
            for (unsigned b = 0; b < 7; ++b) put((byte >> b) & 1);
            put(1 - parity(byte & 0x7f)); put(1);
        }
        length = 16; config = 0x00080080;
    }
    if ((config & 0x10) || !length || length > 24 || length % 4) return INVALID_ARGUMENT;
    for (unsigned i = 0; i < length; i += 4) {
        uint32_t word = 0;
        for (unsigned j = 0; j < 4; ++j) word = (word << 8) | raw[i + j];
        out.blocks[out.count++] = {uint8_t(i / 4 + 1), word};
    }
    out.blocks[out.count++] = {0, config};
    return OK;
}
LFResult lf_write_t5577(Port& port, const LFSnapshot& expected, const LFCard& desired) {
    LFResult result;
    LFEncoding encoding;
    result.status = lf_encode_t5577(desired, encoding);
    if (result.status != OK) return result;
    if (!lf_writable(port, desired.format) || !lf_scannable(port, expected.requested_format)) {
        result.status = UNSUPPORTED; result.detail = "Matching scan and password-free writer required"; return result;
    }
    if ((expected.present && (!expected.card.valid() ||
         lf_descriptor(expected.card.format)->scan_command != lf_descriptor(expected.requested_format)->scan_command)) ||
        !expected.generation ||
        (expected.requested_format == LFFormat::HIDProx ? !hid_hint(expected.hint) : expected.hint != 0)) {
        result.status = INVALID_ARGUMENT; return result;
    }
    result.status = live(port, expected.generation);
    if (result.status != OK) { result.detail = "LF session no longer current"; return result; }
    result.status = reader_mode(port, expected.generation);
    if (result.status != OK) { result.detail = "Reader mode unavailable"; return result; }
    LFCard current;
    result.status = scan_current(port, expected.generation, expected.requested_format, expected.hint, current);
    const bool matches = expected.present ? result.status == OK && expected.card.matches(current) : result.status == NO_CARD;
    if (!matches) {
        if (result.status == OK || result.status == NO_CARD) result.status = STALE_DATA;
        result.detail = "Target observation changed; review again"; return result;
    }
    for (unsigned i = 0; i < encoding.count; ++i) {
        port.progress(i, encoding.count + 1, "Writing password-free T5577 blocks");
        result.status = live(port, expected.generation);
        if (result.status != OK) break;
        const auto& block = encoding.blocks[i];
        Bytes payload{block.block, uint8_t(block.word >> 24), uint8_t(block.word >> 16),
                      uint8_t(block.word >> 8), uint8_t(block.word), 0, 0, 0, 0, 0, 0}, reply;
        result.mutation_started = true;
        result.status = command(port, expected.generation, 3016, payload, 0x40, reply);
        if (result.status != OK) break;
        if (!reply.empty()) { result.status = IO_ERROR; break; }
        ++result.blocks_written;
    }
    if (result.status == OK) {
        port.progress(encoding.count, encoding.count + 1, "Verifying full LF identity");
        for (unsigned i = 0; i < 5 && result.status == OK; ++i) {
            result.status = live(port, expected.generation);
            if (result.status == OK) port.wait_ms(50);
        }
        if (result.status == OK) {
            const uint8_t hint = desired.format == LFFormat::HIDProx ? desired.data[0] : 0;
            result.status = scan_current(port, expected.generation, desired.format, hint, result.actual);
        }
        if (result.status == OK && !desired.matches(result.actual)) result.status = STALE_DATA;
    }
    if (result.status == OK) {
        port.progress(encoding.count + 1, encoding.count + 1, "T5577 identity verified");
        result.status = live(port, expected.generation);
    }
    if (result.status != OK && result.mutation_started) {
        result.status = WRITE_UNVERIFIED;
        result.detail = "Write not verified; target may be partly changed. Scan before retrying";
    } else if (result.status == OK) {
        result.detail = "T5577 identity verified; no password written. Protection state is unknown";
    } else result.detail = "Write stopped before mutation";
    return result;
}
}
