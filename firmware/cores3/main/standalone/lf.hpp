#pragma once
#include "port.hpp"

namespace qb::local {
enum class LFFormat : uint8_t {
    EM410x, Electra, HIDProx, Viking, IoProx, PACStanley, Jablotron, Pyramid
};
struct LFDescriptor {
    LFFormat format;
    const char* name;
    uint16_t scan_command;
    uint8_t id_length;
    bool requires_ascii;
    bool writable;
};
const std::array<LFDescriptor, 8>& lf_formats();
const LFDescriptor* lf_descriptor(LFFormat);
bool lf_scannable(const Port&, LFFormat);
bool lf_writable(const Port&, LFFormat);

struct LFCard {
    LFFormat format = LFFormat::EM410x;
    std::array<uint8_t, 16> data{};
    uint8_t size = 0;
    void clear();
    bool valid() const;
    bool matches(const LFCard&) const;
    ~LFCard() { secure_zero(data.data(), data.size()); }
};
Status lf_parse_id(LFFormat, const std::string&, LFCard&);
LFCard lf_default_test_card();
Status lf_decode_response(LFFormat, const Bytes&, LFCard&);

struct LFSnapshot {
    LFFormat requested_format = LFFormat::EM410x;
    uint8_t hint = 0;
    uint32_t generation = 0;
    bool present = false;
    LFCard card;
    void clear();
};
struct LFBlock { uint8_t block = 0; uint32_t word = 0; };
struct LFEncoding {
    std::array<LFBlock, 7> blocks{};
    uint8_t count = 0;
    void clear() { secure_zero(blocks.data(), sizeof(blocks)); count = 0; }
    ~LFEncoding() { clear(); }
};
Status lf_encode_t5577(const LFCard&, LFEncoding&);
struct LFResult {
    Status status = INVALID_ARGUMENT;
    unsigned blocks_written = 0;
    bool mutation_started = false;
    LFCard actual;
    const char* detail = "Invalid LF write request";
};
Status lf_scan(Port&, LFFormat, LFSnapshot&, uint8_t hint = 0);
// The caller owns review/removal gates, single unprotected target confirmation,
// and restoration of the original reader mode. An absent snapshot does not
// identify a blank tag or establish its protection state.
LFResult lf_write_t5577(Port&, const LFSnapshot& expected_target, const LFCard& desired);
}
