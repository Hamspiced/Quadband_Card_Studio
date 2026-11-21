#pragma once
#include "standalone/port.hpp"

namespace qb::local {
enum class Tool : uint8_t { None, Hf, Uhf, Lf };
enum class Command : uint8_t {
    None, OpenHf, OpenUhf, OpenLf, Exit, SetField, ClearSession, Stop,
    HfRead, HfSelectUnit, HfCapture, HfPrepareUnit, HfPrepareCopy,
    HfAddRecord, HfClearRecords, HfPrepareNdef,
    UhfStart, UhfSelectTag, UhfRead, UhfPrepareUser, UhfPrepareEpc,
    UhfReadPower, UhfPreparePower, LfScan, LfCapture, LfTestId, LfPrepareWrite,
    Confirm, CancelReview, HfArmBulk, HfPrepareBulk, HfRemoveRecord, HfMoveRecordUp
};
enum class Field : uint8_t {
    HfUnit, HfValue, HfKey, HfKeyType, HfStandardKeys, NtagMode, NtagPassword, NtagPack,
    UhfBank, UhfWord, UhfCount, UhfValue, UhfPassword, UhfEpc, UhfPower,
    LfFormat, LfHint, LfId, NdefKind, NdefField0, NdefField1, NdefField2,
    NdefField3, NdefField4, NdefField5, NdefField6, NdefField7,
    BulkFamily, BulkOverwrite, BulkFormatting
};
struct Intent {
    Command command = Command::None;
    Field field = Field::HfUnit;
    unsigned index = 0;
    uint64_t review_token = 0;
    std::array<char, 257> text{};
    ~Intent() { secure_zero(text.data(), text.size()); }
};
struct TagView {
    std::array<uint8_t, 62> epc{};
    uint8_t size = 0;
    int8_t rssi = 0;
    uint32_t reads = 0;
};
struct NdefFieldView {
    std::array<char, 32> label{};
    std::array<char, 257> value{};
    std::array<std::array<char, 24>, 4> choices{};
    uint8_t choice_count = 0;
    bool secret = false;
};
struct View {
    uint64_t revision = 1, review_token = 0;
    Tool tool = Tool::None;
    bool busy = false, inventory = false, reader_present = false, review = false;
    Status status = OK;
    unsigned done = 0, total = 0;
    std::array<char, 105> message{};
    std::array<char, 96> identity{};
    std::array<char, 48> review_title{};
    std::array<std::array<char, 64>, 4> review_lines{};
    int battery = -1;
    bool light_theme = false;

    bool hf_ready = false, hf_source = false, hf_writable = false, ndef_available = false;
    bool hf_copy_armed = false;
    HfFamily hf_family = HfFamily::Unknown;
    unsigned hf_unit = 0, hf_units = 0, hf_unit_size = 16, hf_read_count = 0;
    bool hf_unit_known = false, hf_standard_keys = false;
    uint8_t hf_key_type = 0x60, ntag_mode = 0;
    std::array<char, 33> hf_data{}, hf_value{};
    unsigned ndef_count = 0, ndef_size = 0, ndef_capacity = 0, ndef_queue = 0, ndef_kind = 0;
    std::array<std::array<char, 24>, 19> ndef_kinds{};
    std::array<NdefFieldView, 8> ndef_fields{};
    uint8_t ndef_field_count = 0;
    std::array<std::array<char, 96>, 8> ndef_records{};
    bool bulk_active = false, bulk_overwrite = false, bulk_formatting = false;
    unsigned bulk_phase = 0, bulk_written = 0, bulk_identical = 0, bulk_rejected = 0, bulk_failed = 0, bulk_bytes = 0;
    HfFamily bulk_family = HfFamily::Classic1K;

    std::array<TagView, 64> tags{};
    unsigned tag_count = 0, selected_tag = 64;
    unsigned uhf_bank = 3, uhf_word = 0, uhf_count = 1, uhf_power = 18;
    bool uhf_memory = false, uhf_power_known = false;
    std::array<char, 257> uhf_data{};
    std::array<char, 129> uhf_value{};
    std::array<char, 57> uhf_epc{};

    unsigned lf_format = 0, lf_hint = 0;
    uint16_t lf_scan_mask = 0, lf_write_mask = 0;
    bool lf_present = false, lf_write_armed = false, lf_card_valid = false;
    std::array<char, 48> lf_value{};
    std::array<char, 48> lf_actual{};
};
}
