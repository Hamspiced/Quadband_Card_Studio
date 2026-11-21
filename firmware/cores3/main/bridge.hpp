#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace qb {
class Bytes : public std::vector<uint8_t> {
    void wipe() noexcept { volatile uint8_t* p = data(); for (size_t i = 0; i < size(); ++i) p[i] = 0; }
public:
    using std::vector<uint8_t>::vector;
    using std::vector<uint8_t>::operator=;
    ~Bytes() { wipe(); }
    void clear() noexcept { wipe(); std::vector<uint8_t>::clear(); }
};
enum Status : uint16_t {
    OK, BAD_FRAME, BAD_VERSION, NOT_AUTHORIZED, BAD_SESSION, UNAVAILABLE,
    STALE_GENERATION, BUSY, TIMEOUT, CANCELLED, NO_CARD, AUTH_FAILED,
    UNSUPPORTED, INVALID_ARGUMENT, IO_ERROR, WRITE_UNVERIFIED,
    PERMISSION_DENIED, STALE_DATA
};
constexpr size_t max_payload = 2048, max_frame = 2078, max_buffer = 8312;
uint16_t le16(const uint8_t*);
uint32_t le32(const uint8_t*);
uint32_t crc32(const uint8_t*, size_t);
struct Frame {
    uint8_t kind = 0;
    uint64_t session = 0;
    uint32_t id = 0;
    uint8_t channel = 0, op = 0;
    Status status = OK;
    uint32_t generation = 0;
    Bytes payload;
    Bytes encode() const;
};
class Decoder {
    Bytes buffer;
    bool lost = false;
public:
    bool feed(const uint8_t*, size_t, const std::function<void(const Frame&)>&);
    void reset();
};
struct Slot {
    uint32_t generation = 1;
    bool present = false;
    std::string kind = "unknown";
    std::vector<std::string> capabilities;
};
struct Session {
    uint64_t nonce = 0;
    uint32_t last = 0, pending = 0;
    bool cancelled = false;
    uint64_t epoch = 0;
    void clear();
    Status admit(const Frame&, bool authorized, const std::array<Slot, 3>&);
};
unsigned trailer(unsigned block);
unsigned access_group(unsigned block);
bool classic_data(unsigned blocks, unsigned block);
bool classic_write_allowed(unsigned block, uint8_t key_type, const uint8_t trailer_data[16]);
struct NtagLayout { unsigned pages, last_user, dynamic; };
NtagLayout ntag(unsigned pages);
bool ntag_write_allowed(unsigned pages, unsigned page, const uint8_t initial[16],
                        const uint8_t dynamic[4], const uint8_t config[8], bool authenticated);
Status validate_uhf(const Bytes&);
Status validate_hf(const Frame&);
Status take_uhf_frame(Bytes& buffer, Bytes& frame);
unsigned classic_geometry(uint8_t sak, const uint8_t atqa[2], unsigned uid_size);
Status validate_chameleon(const Bytes&);
uint32_t usb_timeout_ms(unsigned command);
bool exact_uhf_selection(const Bytes&);
class UhfGuard {
    Bytes selection;
    bool verified = false, mode = false;
public:
    void clear();
    void observe(const Bytes& request, const Bytes& response);
    bool writable() const { return verified && mode && exact_uhf_selection(selection); }
    const Bytes& identity() const { return selection; }
};
}
