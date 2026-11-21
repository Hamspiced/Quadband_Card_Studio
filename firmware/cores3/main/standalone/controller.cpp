#include "controller.hpp"
#include "hf.hpp"
#include "bulk.hpp"
#include "uhf.hpp"
#include "lf.hpp"
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>

namespace qb::local {
namespace {
template<size_t N> void set(std::array<char,N>& out, const std::string& value) {
    secure_zero(out.data(),out.size());
    std::copy_n(value.begin(),std::min(value.size(),N-1),out.begin());
}
std::string number(unsigned value) { return std::to_string(value); }
std::string hex(const uint8_t* data, size_t size) {
    const char* digits = "0123456789ABCDEF"; std::string out; out.reserve(size*2);
    for (size_t i = 0; i < size; ++i) { out += digits[data[i]>>4]; out += digits[data[i]&15]; }
    return out;
}
bool integer(const std::string& text, unsigned low, unsigned high, unsigned& out) {
    if (text.empty() || text.size() > 10) return false;
    uint64_t value = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9') return false;
        value = value*10+c-'0'; if (value > high) return false;
    }
    if (value < low) return false;
    out = unsigned(value); return true;
}
int nibble(char c) {
    return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
}
bool parse_hex(const std::string& text, Bytes& out, size_t maximum, size_t exact = 0) {
    out.clear(); int hi = -1;
    for (char c : text) {
        if (c == ' ' || c == ':' || c == '-' || c == '\t' || c == '\r' || c == '\n') continue;
        int v = nibble(c); if (v < 0) { out.clear(); return false; }
        if (hi < 0) hi = v;
        else {
            if (out.size() == maximum) { out.clear(); return false; }
            out.push_back(uint8_t(hi*16+v)); hi = -1;
        }
    }
    if (hi >= 0 || (exact && out.size() != exact)) { out.clear(); return false; }
    return true;
}
void wipe(NdefValues& values) {
    for (auto& f : values) secure_zero(f.second.data(),f.second.size());
    values.clear();
}
std::string lf_value(const LFCard& card) {
    if (!card.valid()) return "";
    auto descriptor = lf_descriptor(card.format);
    return descriptor->requires_ascii ? std::string(card.data.begin(),card.data.begin()+card.size) : hex(card.data.data(),card.size);
}
std::string record_summary(const NdefRecord& record) {
    if (record.tnf == 1 && record.type == Bytes{'T'} && !record.payload.empty() && !(record.payload[0]&0xc0)) {
        size_t offset = 1+(record.payload[0]&63);
        if (offset <= record.payload.size()) return "Text: "+std::string(record.payload.begin()+offset,record.payload.end());
    }
    if (record.tnf == 1 && record.type == Bytes{'U'} && !record.payload.empty()) {
        static const char* prefixes[] = {"","http://www.","https://www.","http://","https://","tel:","mailto:",
            "ftp://anonymous:anonymous@","ftp://ftp.","ftps://","sftp://","smb://","nfs://","ftp://","dav://",
            "news:","telnet://","imap:","rtsp://","urn:","pop:","sip:","sips:","tftp:","btspp://","btl2cap://",
            "btgoep://","tcpobex://","irdaobex://","file://","urn:epc:id:","urn:epc:tag:","urn:epc:pat:",
            "urn:epc:raw:","urn:epc:","urn:nfc:"};
        if (record.payload[0] < sizeof(prefixes)/sizeof(*prefixes))
            return std::string(prefixes[record.payload[0]])+std::string(record.payload.begin()+1,record.payload.end());
    }
    return std::string(record.type.begin(),record.type.end())+" ("+number(unsigned(record.payload.size()))+" bytes)";
}
const char* status_text(Status status) {
    switch (status) {
    case OK: return "Ready.";
    case NO_CARD: return "No card detected. Place the next target.";
    case CANCELLED: return "Stopped. Review again before writing.";
    case STALE_GENERATION: return "Reader changed. Read the target again.";
    case STALE_DATA: return "Target changed. Read and review again.";
    case AUTH_FAILED: return "Authentication failed. Check supplied credentials.";
    case PERMISSION_DENIED: return "Protected or read-only operation.";
    case UNSUPPORTED: return "Reader or format does not support this operation.";
    case BUSY: return "Stop inventory before selecting or editing.";
    case WRITE_UNVERIFIED: return "Write unverified; target may be partly changed. Read again.";
    case INVALID_ARGUMENT: return "Invalid field or incomplete target/source.";
    default: return "Operation failed. Check reader and read again.";
    }
}
}
struct Workbench::State {
    enum class Review { None, HfUnit, HfCopy, HfNdef, HfBulk, UhfUser, UhfEpc, UhfPower, LfWrite };
    Port& port;
    View v;
    Uhf uhf;
    HfReadOptions options;
    HfSnapshot hf, review_hf;
    HfProfile profile, review_profile;
    HfNdefArea area;
    bool hf_ready = false, source = false, copy_armed = false, copy_fresh = false;
    uint32_t source_generation = 0;
    unsigned unit = 0;
    Bytes hf_desired;
    NdefValues fields;
    unsigned kind = 0;
    std::vector<NdefRecord> queue, review_records;
    BulkNdef bulk;
    UhfPassword password, review_password;
    UhfMemory memory, review_memory;
    UhfTag selected, review_tag;
    UhfPower review_power;
    unsigned selected_index = 64, bank = 3, word = 0, count = 1, power = 18;
    Bytes uhf_desired, desired_epc, review_bytes;
    bool inventory = false;
    LFCard desired_lf, review_lf;
    LFSnapshot actual_lf, review_target;
    unsigned lf_format = 0, hint = 0;
    bool lf_armed = false;
    uint32_t lf_source_generation = 0;
    Review review = Review::None;
    unsigned review_unit = 0, review_dbm = 0;
    uint64_t sequence = 0, consumed = 0;
    std::array<uint32_t,3> generations{};
    bool in_write = false;

    explicit State(Port& p) : port(p), uhf(p) {
        for (unsigned i = 0; i < 3; ++i) generations[i] = p.generation(uint8_t(i+1));
        reset_fields(); project();
    }
    ~State() { wipe(fields); secure_zero(&v,sizeof(v)); }
    void bump() { if (v.revision != std::numeric_limits<uint64_t>::max()) ++v.revision; }
    void invalidate() {
        review = Review::None; v.review = false; v.review_token = 0;
        review_hf.clear(); review_profile.clear(); review_memory.clear(); review_target.clear(); review_lf.clear();
        review_bytes.clear(); review_records.clear(); secure_zero(review_password.bytes.data(),review_password.bytes.size());
        secure_zero(&review_tag,sizeof(review_tag)); review_power = {};
        v.review_title.fill(0); for (auto& line : v.review_lines) line.fill(0);
    }
    bool changed() {
        bool any = false;
        for (unsigned i = 0; i < 3; ++i) {
            auto generation = port.generation(uint8_t(i+1));
            if (generation == generations[i]) continue;
            any = true; generations[i] = generation;
            if (i == 0) {
                bulk.stop();
                hf.clear(); area = {}; hf_ready = false; copy_armed = copy_fresh = false; hf_desired.clear();
                source_generation = generation;
            }
            if (i == 1) { uhf.clear(); inventory = false; memory.clear(); selected_index = 64; secure_zero(&selected,sizeof(selected)); }
            if (i == 2) { actual_lf.clear(); lf_armed = false; lf_source_generation = generation; }
        }
        if (any) { invalidate(); bump(); }
        return any;
    }
    void reset_fields() {
        wipe(fields);
        for (const auto& f : ndef_catalog()[kind].fields) fields[f.id] = f.initial;
    }
    void project() {
        unsigned channel = v.tool == Tool::Hf ? 1 : v.tool == Tool::Uhf ? 2 : v.tool == Tool::Lf ? 3 : 0;
        v.reader_present = channel && port.generation(uint8_t(channel)) &&
            (channel == 1 ? port.supports(1,16) : channel == 2 ? port.supports(2,0x22) : port.supports(3,1001));
        v.inventory = inventory; v.hf_ready = hf_ready; v.hf_source = source; v.hf_copy_armed = copy_armed;
        v.hf_family = hf.card.family; v.hf_units = hf.card.units(); v.hf_unit_size = hf.card.unit_size(); v.hf_unit = unit;
        v.hf_unit_known = hf_ready && unit < hf.card.units() && hf.known[unit];
        v.hf_writable = hf_ready && hf_writable(hf,unit);
        v.hf_standard_keys = options.standard_keys; v.hf_key_type = options.key.type; v.ntag_mode = options.auth.mode;
        v.hf_read_count = 0; for (unsigned b = 0; b < hf.card.units(); ++b) if (hf.known[b]) ++v.hf_read_count;
        if (v.hf_unit_known && hf.card.classic() && trailer(unit) == unit)
            set(v.hf_data,"************"+hex(hf.data[unit].data()+6,4)+"************");
        else set(v.hf_data,v.hf_unit_known ? hex(hf.data[unit].data(),hf.card.unit_size()) : "");
        set(v.hf_value,hex(hf_desired.data(),hf_desired.size()));
        v.ndef_available = hf_ready && !area.units.empty(); v.ndef_capacity = area.capacity;
        v.ndef_count = unsigned(area.records.size()); v.ndef_queue = unsigned(queue.size()); v.ndef_kind = kind;
        v.bulk_active = bulk.active(); v.bulk_phase = unsigned(bulk.phase);
        v.bulk_written = bulk.written; v.bulk_identical = bulk.identical;
        v.bulk_rejected = bulk.rejected; v.bulk_failed = bulk.failed;
        Bytes queued; std::string encoding_error;
        v.bulk_bytes = ndef_encode(queue,queued,encoding_error) == OK ? unsigned(queued.size()) : 0;
        v.ndef_size = 0;
        if (v.ndef_available && area.length_position < area.memory.size()) {
            size_t p = area.length_position;
            v.ndef_size = area.memory[p] == 255 && p+2 < area.memory.size() ?
                unsigned(area.memory[p+1])*256+area.memory[p+2] : area.memory[p];
        }
        const auto& catalog = ndef_catalog();
        for (size_t i = 0; i < catalog.size(); ++i) set(v.ndef_kinds[i],catalog[i].label);
        v.ndef_field_count = uint8_t(catalog[kind].fields.size());
        for (auto& f : v.ndef_fields) f = {};
        for (size_t i = 0; i < catalog[kind].fields.size(); ++i) {
            const auto& f = catalog[kind].fields[i]; auto& out = v.ndef_fields[i];
            set(out.label,f.label); out.secret = std::string(f.id) == "password";
            set(out.value,out.secret ? (fields[f.id].empty() ? "" : "********") : fields[f.id]);
            out.choice_count = uint8_t(std::min(size_t(4),f.choices.size()));
            for (unsigned j = 0; j < out.choice_count; ++j) set(out.choices[j],f.choices[j]);
        }
        for (auto& record : v.ndef_records) record.fill(0);
        size_t at = 0;
        for (const auto& record : area.records) { if (at == 8) break; set(v.ndef_records[at++],record_summary(record)); }
        for (const auto& record : queue) { if (at == 8) break; set(v.ndef_records[at++],"+ "+record_summary(record)); }
        v.tag_count = unsigned(uhf.tag_count()); v.selected_tag = selected_index;
        for (auto& tag : v.tags) tag = {};
        for (unsigned i = 0; i < v.tag_count; ++i) {
            const auto& tag = uhf.tags()[i]; v.tags[i].epc = tag.epc; v.tags[i].size = tag.epc_size;
            v.tags[i].rssi = int8_t(std::max(-128,std::min(127,int(tag.rssi)))); v.tags[i].reads = tag.reads;
        }
        v.uhf_bank = bank; v.uhf_word = word; v.uhf_count = count; v.uhf_power = power;
        v.uhf_memory = memory.valid; v.uhf_power_known = uhf.power().valid;
        set(v.uhf_data,memory.valid ? hex(memory.data.data(),memory.words*2) : "");
        set(v.uhf_value,hex(uhf_desired.data(),uhf_desired.size())); set(v.uhf_epc,hex(desired_epc.data(),desired_epc.size()));
        v.lf_format = lf_format; v.lf_hint = hint; v.lf_scan_mask = v.lf_write_mask = 0;
        for (unsigned i = 0; i < 8; ++i) {
            if (lf_scannable(port,LFFormat(i))) v.lf_scan_mask |= uint16_t(1u<<i);
            if (lf_writable(port,LFFormat(i))) v.lf_write_mask |= uint16_t(1u<<i);
        }
        v.lf_present = actual_lf.present; v.lf_card_valid = desired_lf.valid(); v.lf_write_armed = lf_armed;
        set(v.lf_value,lf_value(desired_lf)); set(v.lf_actual,lf_value(actual_lf.card));
        if (v.tool == Tool::Hf) set(v.identity,hf_ready ? hex(hf.card.uid.data(),hf.card.uid_size) : "");
        else if (v.tool == Tool::Uhf) set(v.identity,selected_index < 64 ? hex(selected.epc.data(),selected.epc_size) : "");
        else if (v.tool == Tool::Lf) set(v.identity,actual_lf.present ? lf_value(actual_lf.card) : "");
        else v.identity.fill(0);
    }
    Status finish(Status status, const std::string& message = "") {
        if (bulk.phase == BulkNdef::Phase::Unverified) {
            status = WRITE_UNVERIFIED;
            set(v.message,"Bulk write unverified. No retry; read target and Clear session to acknowledge.");
            v.status = status; v.busy = false; project(); bump(); return status;
        }
        v.status = status; v.busy = inventory; set(v.message,message.empty() ? status_text(status) : message); project(); bump(); return status;
    }
    Status begin_review(Review action, const char* title, const std::array<std::string,4>& lines) {
        if (sequence == std::numeric_limits<uint64_t>::max()) return finish(PERMISSION_DENIED,"Review token exhausted; restart required.");
        review = action; v.review = true; v.review_token = ++sequence; set(v.review_title,title);
        for (unsigned i = 0; i < 4; ++i) set(v.review_lines[i],lines[i]);
        return finish(OK,"Review target and changes. Confirm once to write.");
    }
    void select_unit(unsigned b) {
        unit = b; hf_desired.clear();
        if (hf_ready && b < hf.card.units() && hf.known[b] && hf_data_unit(hf.card,b))
            hf_desired.assign(hf.data[b].begin(),hf.data[b].begin()+hf.card.unit_size());
    }
    Status field(const Intent& intent) {
        size_t length = 0; while (length < intent.text.size() && intent.text[length]) ++length;
        if (length == intent.text.size()) return finish(INVALID_ARGUMENT,"Field exceeds 256 bytes.");
        std::string text(intent.text.data(),length);
        struct Wipe { std::string& s; ~Wipe() { secure_zero(s.data(),s.size()); } } cleanup{text};
        unsigned value = 0; Bytes bytes;
        auto numeric = [&](unsigned low,unsigned high) { return integer(text,low,high,value); };
        auto hexbytes = [&](size_t maximum,size_t exact = 0) { return parse_hex(text,bytes,maximum,exact); };
        unsigned f = unsigned(intent.field);
        if (f <= unsigned(Field::NtagPack) && v.tool != Tool::Hf) return finish(INVALID_ARGUMENT);
        if (f >= unsigned(Field::UhfBank) && f <= unsigned(Field::UhfPower) && v.tool != Tool::Uhf) return finish(INVALID_ARGUMENT);
        if (f >= unsigned(Field::LfFormat) && f <= unsigned(Field::LfId) && v.tool != Tool::Lf) return finish(INVALID_ARGUMENT);
        if (f >= unsigned(Field::NdefKind) && v.tool != Tool::Hf) return finish(INVALID_ARGUMENT);
        switch (intent.field) {
        case Field::BulkFamily:
            if (!numeric(1,5) || value == unsigned(HfFamily::Classic4K)) return finish(INVALID_ARGUMENT);
            v.bulk_family = HfFamily(value); break;
        case Field::BulkOverwrite:
            if (!numeric(0,1)) return finish(INVALID_ARGUMENT);
            v.bulk_overwrite = value != 0; break;
        case Field::BulkFormatting:
            if (!numeric(0,1)) return finish(INVALID_ARGUMENT);
            v.bulk_formatting = value != 0; break;
        case Field::HfUnit:
            if (!numeric(0,hf.card.units() ? hf.card.units()-1 : 255)) return finish(INVALID_ARGUMENT);
            select_unit(value); break;
        case Field::HfValue:
            if (!hexbytes(hf.card.unit_size(),hf.card.unit_size())) return finish(INVALID_ARGUMENT,"Enter exactly one memory unit.");
            hf_desired = bytes; break;
        case Field::HfKey:
            if (!hexbytes(6,6)) return finish(INVALID_ARGUMENT,"Enter exactly six key bytes.");
            std::copy(bytes.begin(),bytes.end(),options.key.bytes.begin()); hf_ready = false; area = {}; break;
        case Field::HfKeyType:
            if (!numeric(96,97)) return finish(INVALID_ARGUMENT);
            options.key.type = uint8_t(value); hf_ready = false; area = {}; break;
        case Field::HfStandardKeys:
            if (!numeric(0,1)) return finish(INVALID_ARGUMENT);
            options.standard_keys = value != 0; hf_ready = false; area = {}; break;
        case Field::NtagMode:
            if (!numeric(0,2)) return finish(INVALID_ARGUMENT);
            options.auth.mode = uint8_t(value); hf_ready = false; area = {}; break;
        case Field::NtagPassword:
            if (!hexbytes(4,4)) return finish(INVALID_ARGUMENT,"Enter exactly four password bytes.");
            std::copy(bytes.begin(),bytes.end(),options.auth.password.begin()); hf_ready = false; area = {}; break;
        case Field::NtagPack:
            if (!hexbytes(2,2)) return finish(INVALID_ARGUMENT,"Enter exactly two PACK bytes.");
            std::copy(bytes.begin(),bytes.end(),options.auth.pack.begin()); hf_ready = false; area = {}; break;
        case Field::UhfBank:
            if (!numeric(0,3)) return finish(INVALID_ARGUMENT);
            bank = value; memory.clear(); break;
        case Field::UhfWord:
            if (!numeric(0,65535)) return finish(INVALID_ARGUMENT);
            word = value; memory.clear(); break;
        case Field::UhfCount:
            if (!numeric(1,64)) return finish(INVALID_ARGUMENT);
            count = value; memory.clear(); break;
        case Field::UhfValue:
            if (!hexbytes(64) || bytes.empty() || bytes.size()%2) return finish(INVALID_ARGUMENT,"Enter 1–32 whole words.");
            uhf_desired = bytes; break;
        case Field::UhfPassword:
            if (!hexbytes(4,4)) return finish(INVALID_ARGUMENT,"Enter exactly four access-password bytes.");
            std::copy(bytes.begin(),bytes.end(),password.bytes.begin()); memory.clear(); break;
        case Field::UhfEpc:
            if (!hexbytes(28) || bytes.size() < 2 || bytes.size()%2) return finish(INVALID_ARGUMENT,"EPC needs 1–14 whole words.");
            desired_epc = bytes; break;
        case Field::UhfPower:
            if (!numeric(18,26)) return finish(INVALID_ARGUMENT,"Power must be 18–26 dBm.");
            power = value; break;
        case Field::LfFormat:
            if (!numeric(0,8)) return finish(INVALID_ARGUMENT);
            lf_format = value; desired_lf.clear(); actual_lf.clear(); lf_armed = false; hint = 0; break;
        case Field::LfHint:
            if (!numeric(0,42) || (value > 30 && value != 42)) return finish(INVALID_ARGUMENT);
            hint = value; actual_lf.clear(); lf_armed = false; break;
        case Field::LfId:
            if (lf_format >= 8 || lf_parse_id(LFFormat(lf_format),text,desired_lf) != OK) {
                desired_lf.clear(); lf_armed = false; return finish(INVALID_ARGUMENT,"ID length/format does not match the selected LF format.");
            }
            lf_armed = false; lf_source_generation = port.generation(3); break;
        case Field::NdefKind:
            if (!numeric(0,18)) return finish(INVALID_ARGUMENT);
            kind = value; reset_fields(); break;
        default:
            if (f < unsigned(Field::NdefField0) || f > unsigned(Field::NdefField7)) return finish(INVALID_ARGUMENT);
            value = f-unsigned(Field::NdefField0);
            if (value >= ndef_catalog()[kind].fields.size()) return finish(INVALID_ARGUMENT);
            {
                const auto& descriptor = ndef_catalog()[kind].fields[value];
                if (!descriptor.choices.empty() && std::none_of(descriptor.choices.begin(),descriptor.choices.end(),
                    [&](const char* choice){return text == choice;})) return finish(INVALID_ARGUMENT,"Choose a listed value.");
                auto& old = fields[descriptor.id]; secure_zero(old.data(),old.size()); old = text;
            }
            break;
        }
        return finish(OK,"Field updated; previous review invalidated.");
    }
    Status read_hf() {
        hf_ready = false; area = {};
        auto r = hf_read(port,options,hf);
        bulk.observe(r.status,port.generation(1));
        hf_ready = (r.status == OK || r.status == IO_ERROR) && hf.card.units() && r.completed;
        if (r.status == NO_CARD && source && source_generation == port.generation(1)) {
            copy_armed = true; copy_fresh = false;
        } else if (hf_ready) {
            copy_fresh = copy_armed;
            hf_ndef_area(hf,area); select_unit(std::min(unit,hf.card.units()-1));
        } else if (r.status != NO_CARD) copy_armed = copy_fresh = false;
        v.done = r.completed; v.total = r.total;
        return finish(r.status,r.detail);
    }
    bool selected_valid() const {
        return !inventory && selected_index < uhf.tag_count() && selected.same_identity(uhf.tags()[selected_index]) &&
            selected.generation == port.generation(2);
    }
    Status scan_lf() {
        Status status = UNSUPPORTED; actual_lf.clear();
        if (lf_format < 8) status = lf_scan(port,LFFormat(lf_format),actual_lf,lf_format == unsigned(LFFormat::HIDProx) ? uint8_t(hint) : 0);
        else {
            bool attempted = false; status = NO_CARD;
            for (unsigned i = 0; i < 8; ++i) {
                if (!lf_scannable(port,LFFormat(i))) continue;
                attempted = true;
                status = lf_scan(port,LFFormat(i),actual_lf,i == unsigned(LFFormat::HIDProx) ? uint8_t(hint) : 0);
                if (status != NO_CARD) break;
            }
            if (!attempted) status = UNSUPPORTED;
        }
        if (status == NO_CARD && desired_lf.valid() && actual_lf.generation == port.generation(3) &&
            lf_source_generation == actual_lf.generation) lf_armed = true;
        else if (status != OK) lf_armed = false;
        return finish(status);
    }
    Status confirm(const Intent& intent) {
        if (!v.review || review == Review::None || !intent.review_token || intent.review_token != v.review_token || intent.review_token <= consumed) {
            invalidate(); return finish(PERMISSION_DENIED,"Stale or already consumed confirmation. Prepare a new review.");
        }
        consumed = intent.review_token; Review action = review;
        v.review = false; v.review_token = 0; review = Review::None; in_write = true;
        Status status = INVALID_ARGUMENT; std::string detail;
        if (action == Review::HfUnit) {
            auto r = hf_edit(port,review_hf,review_unit,review_bytes); status = r.status; detail = r.detail; v.done = r.completed; v.total = r.total;
            hf_ready = false; area = {};
        } else if (action == Review::HfCopy) {
            copy_armed = copy_fresh = false;
            auto r = hf_copy(port,review_hf,review_profile); status = r.status; detail = r.detail; v.done = r.completed; v.total = r.total;
            hf_ready = false; area = {};
        } else if (action == Review::HfBulk) {
            auto r = bulk.confirm(port); status = r.status; detail = r.detail; v.done = r.completed; v.total = r.total;
            hf_ready = false; area = {};
        } else if (action == Review::HfNdef) {
            auto r = hf_write_ndef(port,review_hf,review_records); status = r.status; detail = r.detail; v.done = r.completed; v.total = r.total;
            hf_ready = false; area = {}; if (status == OK) queue.clear();
        } else if (action == Review::UhfUser) {
            status = uhf.write_user(review_tag,review_memory,review_bytes.data(),review_bytes.size(),review_password,memory).status;
        } else if (action == Review::UhfEpc) {
            UhfTag verified;
            status = uhf.rename_epc(review_tag,review_memory,review_bytes.data(),review_bytes.size(),review_password,verified).status;
            memory.clear();
            if (status == OK) { selected = verified; desired_epc.assign(verified.epc.begin(),verified.epc.begin()+verified.epc_size); }
        } else if (action == Review::UhfPower) status = uhf.set_power(review_power,review_dbm).status;
        else if (action == Review::LfWrite) {
            lf_armed = false;
            auto r = lf_write_t5577(port,review_target,review_lf); status = r.status; detail = r.detail;
            v.done = r.blocks_written; v.total = 0; actual_lf.clear();
            if (status == OK) {
                actual_lf.requested_format = review_lf.format; actual_lf.generation = port.generation(3);
                actual_lf.present = true; actual_lf.card = r.actual;
            }
        }
        in_write = false; invalidate(); return finish(status,detail);
    }
    Status process(const Intent& intent) {
        bool generation_changed = changed();
        if (intent.command == Command::Confirm && generation_changed) return finish(STALE_GENERATION);
        if (intent.command == Command::Confirm) return confirm(intent);
        bulk.cancel_review();
        invalidate();
        if (bulk.active() && intent.command != Command::HfRead && intent.command != Command::HfPrepareBulk &&
            intent.command != Command::Stop && intent.command != Command::CancelReview)
            return finish(BUSY,"Bulk records/options frozen. Stop the job before editing or changing tools.");
        if (intent.command == Command::SetField) return field(intent);
        if (intent.command == Command::CancelReview) return finish(OK,"Review cancelled; nothing written.");
        if (intent.command == Command::OpenHf || intent.command == Command::OpenUhf || intent.command == Command::OpenLf) {
            inventory = false; uhf.clear(); selected_index = 64; memory.clear();
            v.tool = intent.command == Command::OpenHf ? Tool::Hf : intent.command == Command::OpenUhf ? Tool::Uhf : Tool::Lf;
            return finish(OK);
        }
        if (intent.command == Command::Stop) {
            bulk.stop();
            inventory = false;
            auto status = uhf.scanning() ? uhf.inventory_stop().status : OK;
            return finish(status,"Stopped. No pending review remains.");
        }
        if (port.cancelled()) return finish(CANCELLED);
        v.busy = true;
        switch (intent.command) {
        case Command::HfRemoveRecord:
        case Command::HfMoveRecordUp:
            if (v.tool != Tool::Hf || intent.index >= queue.size()) return finish(INVALID_ARGUMENT);
            if (intent.command == Command::HfRemoveRecord) queue.erase(queue.begin()+intent.index);
            else if (intent.index) std::swap(queue[intent.index],queue[intent.index-1]);
            return finish(OK,"Record queue updated.");
        case Command::HfArmBulk: {
            if (v.tool != Tool::Hf) return finish(INVALID_ARGUMENT);
            auto status = bulk.arm(port,v.bulk_family,queue,v.bulk_overwrite,v.bulk_formatting);
            hf_ready = false; area = {};
            return finish(status,status == OK ? "Job frozen. Remove all cards; Read must observe No card." : "Bulk needs 1-3 records, exact family and supported options.");
        }
        case Command::HfPrepareBulk: {
            if (v.tool != Tool::Hf || !hf_ready) return finish(PERMISSION_DENIED,"Read the target after observing No card.");
            auto r = bulk.review(port,hf); if (r.status != OK) return finish(r.status,r.detail);
            return begin_review(Review::HfBulk,"Bulk NFC target",{{"UID "+hex(hf.card.uid.data(),hf.card.uid_size),
                number(unsigned(bulk.encoded.size()))+" frozen message bytes; exact family",
                bulk.needs_format ? "FORMAT/INITIALIZE canonical layout" : "Preserve CC/MAD/control metadata",
                bulk.overwrite ? "Overwrite explicitly enabled; confirm once" : "Overwrite OFF; confirm once"}});
        }
        case Command::HfRead: return v.tool == Tool::Hf ? read_hf() : finish(INVALID_ARGUMENT);
        case Command::HfSelectUnit:
            if (v.tool != Tool::Hf || !hf_ready || intent.index >= hf.card.units()) return finish(INVALID_ARGUMENT);
            select_unit(intent.index); return finish(OK);
        case Command::HfCapture: {
            if (v.tool != Tool::Hf || !hf_ready) return finish(INVALID_ARGUMENT,"Read the source first.");
            auto r = hf_capture(hf,profile); source = r.status == OK; copy_armed = copy_fresh = false; source_generation = port.generation(1);
            return finish(r.status,source ? "RAM source captured. Remove it; Read must observe no card." : r.detail);
        }
        case Command::HfPrepareUnit:
            if (v.tool != Tool::Hf || !hf_ready || !hf_writable(hf,unit) || hf_desired.size() != hf.card.unit_size()) return finish(PERMISSION_DENIED);
            review_hf = hf; review_unit = unit; review_bytes = hf_desired;
            return begin_review(Review::HfUnit,"Write HF unit",{{"UID "+hex(hf.card.uid.data(),hf.card.uid_size),
                "Unit "+number(unit)+"; "+number(hf.card.unit_size())+" data bytes",
                hex(hf.data[unit].data(),hf.card.unit_size())+" ->",hex(hf_desired.data(),hf_desired.size())}});
        case Command::HfPrepareCopy:
            if (v.tool != Tool::Hf || !source || !copy_armed || !copy_fresh || !hf_ready) return finish(PERMISSION_DENIED,"Remove source, observe No card, then read the target.");
            if (profile.family != hf.card.family) return finish(INVALID_ARGUMENT,"Source and target families differ.");
            for (unsigned b = 0; b < hf.card.units(); ++b) if (profile.included[b] && (!hf.known[b] ||
                (profile.data[b] != hf.data[b] && !hf_writable(hf,b)))) return finish(PERMISSION_DENIED,"Target data incomplete or protected.");
            review_hf = hf; review_profile = profile;
            return begin_review(Review::HfCopy,"Copy HF RAM data",{{"UID "+hex(hf.card.uid.data(),hf.card.uid_size),
                "Copy "+number(unsigned(std::count(profile.included.begin(),profile.included.end(),true)))+" same-family data units",
                "Excludes UID/keys/trailers/MAD/config","Remove target after every outcome"}});
        case Command::HfAddRecord: {
            if (v.tool != Tool::Hf || queue.size() >= 8) return finish(INVALID_ARGUMENT,"RAM record queue holds at most eight records.");
            NdefRecord record; std::string error;
            auto status = ndef_build(kind,fields,record,error); if (status != OK) return finish(status,error);
            auto candidate = queue; candidate.push_back(record); Bytes encoded;
            status = ndef_encode(candidate,encoded,error); if (status != OK) return finish(status,error);
            queue.push_back(record); return finish(OK,"Record added to RAM queue.");
        }
        case Command::HfClearRecords:
            if (v.tool != Tool::Hf) return finish(INVALID_ARGUMENT);
            queue.clear(); return finish(OK,"RAM queue cleared; target records unchanged.");
        case Command::HfPrepareNdef: {
            if (v.tool != Tool::Hf || !hf_ready || area.units.empty() || !area.writable || queue.empty()) return finish(PERMISSION_DENIED,"Read writable NDEF memory and add a record.");
            review_records = area.records; review_records.insert(review_records.end(),queue.begin(),queue.end());
            Bytes encoded; std::string error; auto status = ndef_encode(review_records,encoded,error);
            if (status != OK || encoded.size() > area.capacity) { review_records.clear(); return finish(INVALID_ARGUMENT,"Record queue exceeds target message capacity."); }
            review_hf = hf;
            return begin_review(Review::HfNdef,"Write NFC records",{{"UID "+hex(hf.card.uid.data(),hf.card.uid_size),
                "Append "+number(unsigned(queue.size()))+" records; existing preserved",
                number(unsigned(encoded.size()))+" / "+number(area.capacity)+" message bytes","Length-last commit + full readback"}});
        }
        case Command::UhfStart:
            if (v.tool != Tool::Uhf) return finish(INVALID_ARGUMENT);
            if (uhf.scanning()) return finish(BUSY);
            {
                auto r = uhf.inventory_start(); inventory = r.status == OK; selected_index = 64; memory.clear();
                return finish(r.status);
            }
        case Command::UhfSelectTag:
            if (v.tool != Tool::Uhf || inventory || uhf.scanning()) return finish(BUSY);
            if (intent.index >= uhf.tag_count()) return finish(INVALID_ARGUMENT);
            selected_index = intent.index; selected = uhf.tags()[intent.index]; memory.clear();
            desired_epc.assign(selected.epc.begin(),selected.epc.begin()+selected.epc_size);
            return finish(OK,"Tag selected by exact PC + EPC.");
        case Command::UhfRead:
            if (v.tool != Tool::Uhf || !selected_valid() || word+count > 65536) return finish(INVALID_ARGUMENT);
            {
                auto status = uhf.read(selected,UhfBank(bank),uint16_t(word),uint16_t(count),password,memory).status;
                if (status == OK && count <= 32) uhf_desired.assign(memory.data.begin(),memory.data.begin()+count*2);
                return finish(status);
            }
        case Command::UhfPrepareUser:
            if (v.tool != Tool::Uhf || !selected_valid() || !memory.valid || memory.bank != UhfBank::User ||
                bank != 3 || memory.word != word || memory.words != count || count > 32 ||
                uhf_desired.size() != count*2 || !memory.tag.same_identity(selected)) return finish(PERMISSION_DENIED,"Read the exact User range first (1–32 words).");
            review_memory = memory; review_tag = selected; review_bytes = uhf_desired; review_password = password;
            return begin_review(Review::UhfUser,"Write UHF User memory",{{"EPC "+hex(selected.epc.data(),selected.epc_size),
                "User word "+number(word)+"; "+number(count)+" words",
                "Old "+hex(memory.data.data(),std::min(size_t(16),size_t(count)*2))+(count > 8 ? "..." : ""),
                "New "+hex(uhf_desired.data(),std::min(size_t(16),uhf_desired.size()))+(count > 8 ? "..." : "")}});
        case Command::UhfPrepareEpc: {
            if (v.tool != Tool::Uhf || !selected_valid() || desired_epc.size() < 2 || desired_epc.size() > 28 || desired_epc.size()%2) return finish(INVALID_ARGUMENT);
            unsigned words = unsigned((std::max(size_t(selected.epc_size),desired_epc.size())+2)/2);
            auto status = uhf.read(selected,UhfBank::Epc,1,uint16_t(words),password,review_memory).status;
            if (status != OK) return finish(status);
            review_tag = selected; review_bytes = desired_epc; review_password = password;
            return begin_review(Review::UhfEpc,"Rename UHF EPC",{{"Old "+hex(selected.epc.data(),selected.epc_size),
                "New "+hex(desired_epc.data(),desired_epc.size()),
                "PC+EPC word1; "+number(words)+" previous words","Exact selection; no lock/kill"}});
        }
        case Command::UhfReadPower:
            if (v.tool != Tool::Uhf) return finish(INVALID_ARGUMENT);
            {
                auto status = uhf.read_power().status;
                if (status == OK) power = uhf.power().centidbm/100;
                return finish(status);
            }
        case Command::UhfPreparePower: {
            if (v.tool != Tool::Uhf || inventory || power < 18 || power > 26) return finish(INVALID_ARGUMENT);
            auto status = uhf.read_power().status; if (status != OK) return finish(status);
            review_power = uhf.power(); review_dbm = power;
            return begin_review(Review::UhfPower,"Set UHF RF power",{{"Current "+number(review_power.centidbm)+" centi-dBm",
                "New "+number(power)+" dBm","Permitted range 18–26 dBm","Setting read back after change"}});
        }
        case Command::LfScan: return v.tool == Tool::Lf ? scan_lf() : finish(INVALID_ARGUMENT);
        case Command::LfCapture:
            if (v.tool != Tool::Lf || !actual_lf.present || !actual_lf.card.valid() || actual_lf.generation != port.generation(3)) return finish(INVALID_ARGUMENT,"Scan a source before capture.");
            desired_lf = actual_lf.card; lf_format = unsigned(desired_lf.format); lf_source_generation = port.generation(3); lf_armed = false;
            return finish(OK,"Source captured in RAM. Remove it; Scan must observe No card.");
        case Command::LfTestId:
            if (v.tool != Tool::Lf) return finish(INVALID_ARGUMENT);
            desired_lf = lf_default_test_card(); lf_format = unsigned(desired_lf.format); hint = 0;
            lf_source_generation = port.generation(3); lf_armed = false;
            return finish(OK,"Test ID 0102030405 selected. Remove all cards; scan No card.");
        case Command::LfPrepareWrite: {
            if (v.tool != Tool::Lf || !desired_lf.valid() || !lf_armed || lf_source_generation != port.generation(3)) return finish(PERMISSION_DENIED,"Capture/set ID, remove cards, and Scan No card first.");
            if (!lf_writable(port,desired_lf.format)) return finish(UNSUPPORTED,"Selected LF format is read-only.");
            auto status = lf_scan(port,desired_lf.format,review_target,desired_lf.format == LFFormat::HIDProx ? uint8_t(hint) : 0);
            if (status != OK && status != NO_CARD) { lf_armed = false; return finish(status); }
            if (review_target.generation != lf_source_generation) { lf_armed = false; return finish(STALE_GENERATION); }
            review_lf = desired_lf;
            return begin_review(Review::LfWrite,"Write password-free T5577",{{
                std::string(lf_descriptor(desired_lf.format)->name)+" ID "+lf_value(desired_lf),
                review_target.present ? "Current "+lf_value(review_target.card) : "No reply does NOT prove blank",
                "ONE unprotected T5577 only","No block7/PWD; protection unknown"}});
        }
        case Command::None: return finish(OK);
        default: return finish(INVALID_ARGUMENT);
        }
    }
};
Workbench::Workbench(Port& port) : state_(std::make_unique<State>(port)) {}
Workbench::~Workbench() = default;
const View& Workbench::view() const { return state_->v; }
void Workbench::refresh() { state_->changed(); state_->project(); state_->bump(); }
void Workbench::progress(unsigned done, unsigned total, const char* detail) {
    state_->v.busy = true; state_->v.done = done; state_->v.total = total;
    set(state_->v.message,detail ? detail : "Working"); state_->bump();
}
void Workbench::cancel() {
    const bool unverified = state_->v.status == WRITE_UNVERIFIED;
    state_->inventory = false; state_->uhf.cancel_inventory(); state_->invalidate(); state_->v.busy = false;
    state_->copy_armed = state_->copy_fresh = state_->lf_armed = false;
    state_->bulk.stop();
    state_->finish(unverified ? WRITE_UNVERIFIED : CANCELLED);
}
void Workbench::clear() {
    auto& s = *state_; s.invalidate(); s.uhf.clear(); s.hf.clear(); s.profile.clear(); s.area = {};
    s.bulk.clear();
    s.source = s.hf_ready = s.copy_armed = s.copy_fresh = s.inventory = s.lf_armed = false;
    s.source_generation = s.lf_source_generation = 0; s.selected_index = 64; s.memory.clear(); s.actual_lf.clear(); s.desired_lf.clear();
    s.hf_desired.clear(); s.uhf_desired.clear(); s.desired_epc.clear(); s.queue.clear();
    secure_zero(s.password.bytes.data(),s.password.bytes.size()); secure_zero(&s.selected,sizeof(s.selected));
    secure_zero(s.options.key.bytes.data(),s.options.key.bytes.size());
    secure_zero(s.options.auth.password.data(),s.options.auth.password.size()); secure_zero(s.options.auth.pack.data(),s.options.auth.pack.size());
    s.options = HfReadOptions{}; s.kind = s.unit = s.word = s.lf_format = s.hint = 0; s.bank = 3; s.count = 1; s.power = 18;
    s.reset_fields();
    uint64_t revision = s.v.revision; int battery = s.v.battery; bool light = s.v.light_theme;
    secure_zero(&s.v,sizeof(s.v)); s.v.~View(); ::new (static_cast<void*>(&s.v)) View();
    s.v.revision = revision; s.v.battery = battery; s.v.light_theme = light;
    s.finish(OK,"Session cleared. RAM results and credentials removed.");
}
Status Workbench::process(const Intent& intent) {
    if (intent.command == Command::ClearSession || intent.command == Command::Exit) {
        const auto tool = state_->v.tool;
        clear();
        if (intent.command == Command::ClearSession) {
            state_->v.tool = tool; state_->project(); state_->bump();
        }
        return OK;
    }
    try { return state_->process(intent); }
    catch (const std::exception&) {
        bool write = state_->in_write; state_->in_write = false; state_->invalidate();
        state_->copy_armed = state_->copy_fresh = state_->lf_armed = false;
        if (write) {
            if (state_->bulk.active()) state_->bulk.latch_unverified();
            state_->hf_ready = false; state_->area = {}; state_->memory.clear(); state_->actual_lf.clear();
        }
        return state_->finish(write ? WRITE_UNVERIFIED : IO_ERROR,"Operation stopped; read again before reviewing.");
    }
}
Status Workbench::tick() {
    auto& s = *state_;
    if (s.changed()) return s.finish(STALE_GENERATION);
    if (!s.inventory) return OK;
    if (s.port.cancelled()) { cancel(); return CANCELLED; }
    auto status = s.uhf.inventory_poll().status;
    if (status != OK) s.inventory = false;
    return s.finish(status);
}
void Workbench::feed_uhf(const Bytes& bytes) {
    auto& s = *state_;
    if (s.changed()) { s.finish(STALE_GENERATION); return; }
    if (!s.inventory) return;
    auto status = s.uhf.feed(bytes.data(),bytes.size()).status;
    if (status != OK) s.inventory = false;
    s.finish(status);
}
}
