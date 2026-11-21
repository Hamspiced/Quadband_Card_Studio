#pragma once
#include "hf.hpp"
#include "format.hpp"
#include <memory>

namespace qb::local {
class BulkNdef {
public:
    enum class Phase { Idle, Removal, Target, Review, Unverified };
    Phase phase = Phase::Idle;
    HfFamily family = HfFamily::Classic1K;
    bool overwrite = false, formatting = false, needs_format = false;
    unsigned written = 0, identical = 0, rejected = 0, failed = 0;
    Bytes encoded;
private:
    uint32_t generation_ = 0;
    std::vector<NdefRecord> records_;
    HfSnapshot target_;
    Bytes snapshot_;
    static bool empty_area(const HfNdefArea& area) {
        if (!area.records.empty()) return false;
        auto cursor = area.memory.begin()+area.prefix.size();
        const auto end = area.memory.end();
        if (std::all_of(cursor,end,[](uint8_t b){return b == 0;})) return true;
        if (end-cursor < 2 || cursor[0] != 3 || cursor[1] != 0) return false;
        cursor += 2;
        if (cursor != end && *cursor == 0xfe) ++cursor;
        return std::all_of(cursor,end,[](uint8_t b){return b == 0;});
    }
    HfResult reject(Status s, const char* detail) {
        ++rejected; phase = Phase::Removal;
        HfResult r; r.status = s; r.detail = detail; return r;
    }
public:
    bool active() const { return phase != Phase::Idle; }
    void latch_unverified() { phase = Phase::Unverified; ++failed; target_.clear(); snapshot_.clear(); }
    void stop() { if (phase != Phase::Unverified) phase = Phase::Idle; target_.clear(); snapshot_.clear(); }
    void clear() { phase = Phase::Idle; records_.clear(); encoded.clear(); target_.clear(); snapshot_.clear(); }
    Status arm(Port& p, HfFamily selected, const std::vector<NdefRecord>& records, bool erase, bool format) {
        if (active() || records.empty() || records.size() > 3 ||
            (selected != HfFamily::Classic1K && selected != HfFamily::Ntag213 &&
             selected != HfFamily::Ntag215 && selected != HfFamily::Ntag216)) return INVALID_ARGUMENT;
        std::string error;
        auto s = ndef_encode(records,encoded,error); if (s != OK) return s;
        generation_ = p.generation(1); if (!generation_) return UNAVAILABLE;
        if (format && !p.supports(1,selected == HfFamily::Classic1K ? 22 : 23)) return UNSUPPORTED;
        family = selected; records_ = records; overwrite = erase; formatting = format;
        written = identical = rejected = failed = 0; needs_format = false; phase = Phase::Removal;
        return OK;
    }
    void observe(Status status, uint32_t generation) {
        if (!active() || phase == Phase::Unverified) return;
        if (generation != generation_ || !generation) { stop(); return; }
        if (status != OK && status != NO_CARD) {
            phase = Phase::Removal; target_.clear(); snapshot_.clear(); return;
        }
        if (status == NO_CARD && phase == Phase::Removal) phase = Phase::Target;
    }
    void cancel_review() {
        if (phase == Phase::Review) { phase = Phase::Removal; target_.clear(); snapshot_.clear(); }
    }
    HfResult review(Port& p, const HfSnapshot& actual) {
        HfResult r;
        if (phase != Phase::Target) { r.status = PERMISSION_DENIED; r.detail = "Read must observe No card before each target."; return r; }
        if (actual.generation != generation_ || p.generation(1) != generation_)
            return reject(STALE_GENERATION,"Reader changed; stop and re-arm.");
        if (actual.card.family != family) return reject(INVALID_ARGUMENT,"Target differs from the exact selected family.");
        const unsigned count = actual.card.classic() ? actual.card.units() : actual.card.units()-2;
        snapshot_.clear();
        for (unsigned b = 0; b < count; ++b) {
            if (!actual.known[b]) return reject(IO_ERROR,"Complete readable target snapshot required.");
            snapshot_.insert(snapshot_.end(),actual.data[b].begin(),actual.data[b].begin()+actual.card.unit_size());
        }
        target_ = actual;
        HfNdefArea area;
        auto parsed = hf_ndef_area(actual,area);
        needs_format = parsed.status != OK;
        if (needs_format) {
            if (!formatting || !p.supports(1,actual.card.classic() ? 22 : 23))
                return reject(PERMISSION_DENIED,"Canonical formatting requires explicit enabled option and reader support.");
            if (actual.auth.mode) return reject(PERMISSION_DENIED,"Initialization does not accept password authentication.");
            HfSnapshot preview = actual;
            if (actual.card.classic()) for (unsigned sec = 0; sec < 16; ++sec) {
                ClassicKey factory;
                if (!actual.key_known[sec] || actual.keys[sec].type != 0x60 || actual.keys[sec].bytes != factory.bytes)
                    return reject(PERMISSION_DENIED,"All sectors require proven factory key A.");
            }
            FormatIO io;
            io.guard = [] { return OK; };
            io.read = [&](unsigned b, const ClassicKey&, Bytes& out) {
                out.assign(preview.data[b].begin(),preview.data[b].begin()+actual.card.unit_size()); return OK;
            };
            io.write = [&](unsigned b, const ClassicKey&, const Bytes& data) {
                std::copy(data.begin(),data.end(),preview.data[b].begin()); return OK;
            };
            auto s = canonical_format(actual.card,overwrite ? 1 : 0,snapshot_,io);
            if (s != OK) return reject(s,"Target is not an eligible factory/blank-CC card.");
            if (actual.card.classic()) for (unsigned sec = 0; sec < 16; ++sec)
                preview.keys[sec].bytes = sec == 0 ? std::array<uint8_t,6>{{0xa0,0xa1,0xa2,0xa3,0xa4,0xa5}} :
                    std::array<uint8_t,6>{{0xd3,0xf7,0xd3,0xf7,0xd3,0xf7}};
            parsed = hf_ndef_area(preview,area);
        }
        if (parsed.status != OK || !area.writable) return reject(PERMISSION_DENIED,"Target layout or permissions prevent NDEF writes.");
        if (area.prefix.size()+area.suffix.size()+encoded.size()+(encoded.size()<255 ? 2 : 4) > area.memory.size())
            return reject(INVALID_ARGUMENT,"Frozen records exceed actual target capacity.");
        if (area.records != records_ && !empty_area(area) && !overwrite)
            return reject(PERMISSION_DENIED,"Existing different records or data require explicit overwrite permission.");
        phase = Phase::Review;
        r.detail = needs_format ? "Review canonical formatting then frozen NDEF write." : "Review frozen NDEF target.";
        return r;
    }
    HfResult confirm(Port& p) {
        HfResult r;
        if (phase != Phase::Review) { r.status = PERMISSION_DENIED; return r; }
        phase = Phase::Removal;
        if (p.generation(1) != generation_ || p.cancelled()) {
            r.status = p.cancelled() ? CANCELLED : STALE_GENERATION; ++rejected; return r;
        }
        bool formatted = false;
        if (needs_format) {
            auto s = p.format_ndef(target_.card,overwrite ? 1 : 0,snapshot_);
            if (s != OK) {
                r.status = s; r.attempted = s == WRITE_UNVERIFIED;
                if (r.attempted) { phase = Phase::Unverified; ++failed; } else ++rejected;
                return r;
            }
            formatted = true;
            HfReadOptions options; options.standard_keys = true;
            auto fresh = std::make_unique<HfSnapshot>();
            r = hf_read(p,options,*fresh);
            if (!r.verified || fresh->card != target_.card || fresh->generation != generation_) {
                r.status = WRITE_UNVERIFIED; r.verified = false; r.attempted = true;
                phase = Phase::Unverified; ++failed; return r;
            }
            target_ = *fresh;
        }
        HfNdefArea area; hf_ndef_area(target_,area);
        bool same = area.records == records_;
        r = hf_write_ndef(p,target_,records_);
        if (formatted && r.status != OK) { r.status = WRITE_UNVERIFIED; r.attempted = true; }
        if (r.status == WRITE_UNVERIFIED) { phase = Phase::Unverified; ++failed; }
        else if (r.status != OK) ++rejected;
        else if (same) ++identical;
        else ++written;
        target_.clear(); snapshot_.clear(); return r;
    }
};
}
