#pragma once
#include "ndef.hpp"

namespace qb::local {
struct HfResult {
    Status status = OK;
    unsigned completed = 0, total = 0;
    bool attempted = false, verified = false;
    std::string detail;
};
struct HfReadOptions {
    ClassicKey key;
    bool standard_keys = false;
    NtagAuth auth;
};
struct HfSnapshot {
    HfIdentity card;
    uint32_t generation = 0;
    std::array<std::array<uint8_t, 16>, 256> data{};
    std::array<bool, 256> known{};
    std::array<Status, 256> errors{};
    std::array<ClassicKey, 40> keys{};
    std::array<bool, 40> key_known{};
    NtagAuth auth;
    void clear();
    ~HfSnapshot() { clear(); }
};
struct HfProfile {
    HfFamily family = HfFamily::Unknown;
    unsigned unit_size = 0, unit_count = 0;
    std::array<std::array<uint8_t, 16>, 256> data{};
    std::array<bool, 256> included{};
    void clear();
    ~HfProfile() { clear(); }
};
struct HfNdefArea {
    std::vector<unsigned> units;
    Bytes memory, prefix, suffix;
    std::vector<NdefRecord> records;
    unsigned length_position = 0, capacity = 0;
    bool writable = false;
};
bool hf_data_unit(const HfIdentity&, unsigned unit);
bool hf_profile_unit(const HfIdentity&, unsigned unit);
bool hf_writable(const HfSnapshot&, unsigned unit);
HfResult hf_read(Port&, const HfReadOptions&, HfSnapshot&);
HfResult hf_edit(Port&, const HfSnapshot& expected, unsigned unit, const Bytes& desired);
HfResult hf_capture(const HfSnapshot&, HfProfile&);
HfResult hf_copy(Port&, const HfSnapshot& expected, const HfProfile&);
HfResult hf_ndef_area(const HfSnapshot&, HfNdefArea&);
HfResult hf_write_ndef(Port&, const HfSnapshot& expected, const std::vector<NdefRecord>&);
}
