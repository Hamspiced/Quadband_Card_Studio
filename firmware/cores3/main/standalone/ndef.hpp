#pragma once
#include "port.hpp"
#include <map>

namespace qb::local {
constexpr size_t ndef_limit = 4096, ndef_field_limit = 256;
struct NdefRecord {
    uint8_t tnf = 0;
    Bytes type, payload, identifier;
    bool operator==(const NdefRecord& r) const {
        return tnf == r.tnf && type == r.type && payload == r.payload && identifier == r.identifier;
    }
};
struct NdefField {
    const char* id;
    const char* label;
    const char* initial;
    std::vector<const char*> choices;
};
struct NdefKind {
    const char* label;
    std::vector<NdefField> fields;
};
const std::vector<NdefKind>& ndef_catalog();
using NdefValues = std::map<std::string, std::string>;
Status ndef_build(size_t kind, const NdefValues&, NdefRecord&, std::string& error);
Status ndef_encode(const std::vector<NdefRecord>&, Bytes&, std::string& error);
Status ndef_decode(const Bytes&, std::vector<NdefRecord>&, std::string& error);
}
