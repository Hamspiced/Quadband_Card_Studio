#include "../main/standalone/controller.hpp"
#include "../main/standalone/lf.hpp"
#include "../main/standalone/format.hpp"
#include <cassert>
#include <functional>
#include <iostream>

using namespace qb;
using namespace qb::local;
namespace {
Bytes m100(uint8_t command,const Bytes& data,uint8_t kind = 1) {
    Bytes out{0xbb,kind,command,uint8_t(data.size()>>8),uint8_t(data.size())};
    out.insert(out.end(),data.begin(),data.end()); uint8_t sum = 0;
    for (size_t i = 1; i < out.size(); ++i) sum += out[i];
    out.push_back(sum); out.push_back(0x7e); return out;
}
uint16_t crc(const Bytes& data) {
    uint16_t c = 0xffff;
    for (auto value : data) {
        c ^= uint16_t(value)<<8;
        for (unsigned i = 0; i < 8; ++i) c = uint16_t((unsigned(c)<<1) ^ (c&0x8000 ? 0x1021 : 0));
    }
    return c^0xffff;
}
Bytes ultra(uint16_t command,const Bytes& data,uint16_t status) {
    Bytes out{0x11,0xef,uint8_t(command>>8),uint8_t(command),uint8_t(status>>8),uint8_t(status),0,uint8_t(data.size()),0};
    uint8_t sum = 0; for (auto byte : out) sum += byte; out[8] = uint8_t(0-sum);
    out.insert(out.end(),data.begin(),data.end()); sum = 0; for (auto byte : out) sum += byte;
    out.push_back(uint8_t(0-sum)); return out;
}
struct Mock : Port {
    std::array<uint32_t,3> gens{{1,2,3}};
    bool stop = false, hf_present = true, lf_present = true, lf_writer = true;
    bool reader_mode_no_card = false, fail_write = false;
    unsigned calls = 0, writes = 0, power_writes = 0, lf_writes = 0, epc_read_words = 0;
    HfIdentity card;
    std::array<std::array<uint8_t,16>,256> pages{};
    Bytes epc{0x11,0x22,0x33,0x44}, filter;
    std::array<uint8_t,2> pc{{0x10,0}};
    std::array<uint8_t,512> user{};
    uint16_t rf = 2000;
    LFCard actual_lf, written_lf;
    std::function<void()> mutation_hook;
    Workbench* workbench = nullptr;
    Mock() {
        card.family = HfFamily::Ntag213; card.uid_size = 7; card.uid[0] = 4;
        card.atqa = {{0x44,0}}; card.version = {{0,4,4,2,1,0,15,3}};
        pages[3] = {0xe1,0x10,18,0}; pages[41][3] = 255;
        actual_lf = lf_default_test_card(); actual_lf.data[0] = 9; written_lf = lf_default_test_card();
    }
    bool cancelled() const override { return stop; }
    uint32_t generation(uint8_t channel) const override { return gens[channel-1]; }
    bool supports(uint8_t channel,uint16_t command) const override { return channel != 3 || command != 3016 || lf_writer; }
    void progress(unsigned done,unsigned total,const char* text) override { if (workbench) workbench->progress(done,total,text); }
    void wait_ms(unsigned) override {}
    void mutation() {
        if (workbench) assert(!workbench->view().review && workbench->view().review_token == 0);
        if (mutation_hook) mutation_hook();
    }
    Status identify(HfIdentity& out) override { ++calls; out = card; return hf_present ? OK : NO_CARD; }
    Status read_classic(const HfIdentity&,unsigned b,const ClassicKey&,std::array<uint8_t,16>& out) override {
        ++calls; out = pages[b]; return OK;
    }
    Status write_classic(const HfIdentity&,unsigned b,const ClassicKey&,const std::array<uint8_t,16>& previous,
                         const std::array<uint8_t,16>& desired,std::array<uint8_t,16>& verified) override {
        assert(classic_data(card.units(),b) && previous == pages[b]); ++calls; ++writes; mutation();
        if (fail_write) return IO_ERROR;
        pages[b] = verified = desired; return OK;
    }
    Status read_pages(const HfIdentity&,unsigned b,unsigned count,const NtagAuth&,Bytes& out) override {
        ++calls; out.clear();
        for (unsigned i = b; i < b+count; ++i) out.insert(out.end(),pages[i].begin(),pages[i].begin()+4);
        return OK;
    }
    Status write_page(const HfIdentity&,unsigned b,const NtagAuth&,const std::array<uint8_t,4>& previous,
                      const std::array<uint8_t,4>& desired,std::array<uint8_t,4>& verified) override {
        assert(b >= 4 && b <= 39 && std::equal(previous.begin(),previous.end(),pages[b].begin()));
        ++calls; ++writes; mutation(); if (fail_write) return IO_ERROR;
        std::copy(desired.begin(),desired.end(),pages[b].begin()); verified = desired; return OK;
    }
    Status reset_hf() override { return OK; }
    Status format_ndef(const HfIdentity& expected,uint8_t flags,const Bytes& snapshot) override {
        FormatIO io;
        io.guard = [&]() { return stop ? CANCELLED : card != expected ? STALE_DATA : OK; };
        io.read = [&](unsigned b,const ClassicKey&,Bytes& data) {
            data.assign(pages[b].begin(),pages[b].begin()+card.unit_size()); return OK;
        };
        io.write = [&](unsigned b,const ClassicKey&,const Bytes& data) {
            ++writes; mutation(); if (fail_write) return IO_ERROR;
            std::copy(data.begin(),data.end(),pages[b].begin()); return OK;
        };
        return canonical_format(card,flags,snapshot,io);
    }
    Bytes identity() {
        Bytes out{uint8_t(epc.size()+2),pc[0],pc[1]}; out.insert(out.end(),epc.begin(),epc.end()); return out;
    }
    Status transact(uint8_t channel,const Bytes& request,Bytes& response) override {
        ++calls;
        if (channel == 3) {
            assert(validate_chameleon(request) == OK);
            unsigned command = unsigned(request[2])*256+request[3]; Bytes data; uint16_t status = 0x40;
            if (command == 1001) status = reader_mode_no_card ? 1 : 0x68;
            else if (command == 3016) {
                ++lf_writes; mutation(); if (fail_write) return IO_ERROR;
                if (request[9] == 0) { actual_lf = written_lf; lf_present = true; }
            } else if (!lf_present) status = 1;
            else data.assign(actual_lf.data.begin(),actual_lf.data.begin()+actual_lf.size);
            response = ultra(uint16_t(command),data,status); return OK;
        }
        assert(channel == 2 && validate_uhf(request) == OK);
        unsigned command = request[2]; Bytes data{0}; Bytes payload(request.begin()+5,request.end()-2);
        switch (command) {
        case 0x12: case 0x28: break;
        case 0x22: {
            Bytes tag(pc.begin(),pc.end()); tag.insert(tag.end(),epc.begin(),epc.end()); auto c = crc(tag);
            tag.insert(tag.begin(),0xc9); tag.push_back(uint8_t(c>>8)); tag.push_back(uint8_t(c));
            response = m100(0x22,tag,2); return OK;
        }
        case 0x0c: filter = payload; break;
        case 0x0b: data = filter; break;
        case 0x39: {
            unsigned bank = payload[4], word = unsigned(payload[5])*256+payload[6], count = unsigned(payload[7])*256+payload[8];
            Bytes memory(user.begin(),user.end());
            if (bank == 1) {
                epc_read_words = count; memory = {0,0,pc[0],pc[1]}; memory.insert(memory.end(),epc.begin(),epc.end()); memory.resize(512,0xaa);
            }
            assert((word+count)*2 <= memory.size()); data = identity();
            data.insert(data.end(),memory.begin()+word*2,memory.begin()+(word+count)*2); break;
        }
        case 0x49: {
            ++writes; mutation(); if (fail_write) return IO_ERROR;
            data = identity(); data.push_back(0);
            if (payload[4] == 1) { pc = {payload[9],payload[10]}; epc.assign(payload.begin()+11,payload.end()); }
            else {
                unsigned word = unsigned(payload[5])*256+payload[6];
                std::copy(payload.begin()+9,payload.end(),user.begin()+word*2);
            }
            break;
        }
        case 0xb7: data = {uint8_t(rf>>8),uint8_t(rf)}; break;
        case 0xb6: ++power_writes; mutation(); if (fail_write) return IO_ERROR; rf = uint16_t(unsigned(payload[0])*256+payload[1]); break;
        default: assert(false);
        }
        response = m100(uint8_t(command),data); return OK;
    }
};
Status command(Workbench& w,Command command,unsigned index = 0,uint64_t token = 0) {
    Intent intent; intent.command = command; intent.index = index; intent.review_token = token; return w.process(intent);
}
Status field(Workbench& w,Field field,const std::string& value) {
    Intent intent; intent.command = Command::SetField; intent.field = field;
    assert(value.size() < intent.text.size()); std::copy(value.begin(),value.end(),intent.text.begin()); return w.process(intent);
}
uint64_t prepare_hf(Workbench& w) {
    assert(command(w,Command::HfRead) == OK); assert(field(w,Field::HfUnit,"4") == OK);
    assert(field(w,Field::HfValue,"01020304") == OK); assert(command(w,Command::HfPrepareUnit) == OK);
    assert(w.view().review && w.view().review_token); return w.view().review_token;
}
void hf_reviews() {
    Mock p; Workbench w(p); p.workbench = &w; assert(command(w,Command::OpenHf) == OK);
    auto token = prepare_hf(w); assert(!p.writes);
    assert(command(w,Command::Confirm,0,token+1) == PERMISSION_DENIED && !p.writes);
    auto newer = prepare_hf(w); assert(newer > token);
    assert(command(w,Command::Confirm,0,newer) == OK && p.writes == 1 && !w.view().review);
    assert(command(w,Command::Confirm,0,newer) == PERMISSION_DENIED && p.writes == 1);
    token = prepare_hf(w); assert(field(w,Field::HfValue,"05060708") == OK);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.writes == 1);
    token = prepare_hf(w); assert(command(w,Command::HfSelectUnit,5) == OK);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.writes == 1);
    token = prepare_hf(w); ++p.gens[0];
    assert(command(w,Command::Confirm,0,token) == STALE_GENERATION && p.writes == 1);
    token = prepare_hf(w); p.pages[5][0] = 10;
    assert(command(w,Command::Confirm,0,token) == STALE_DATA && p.writes == 1);
    token = prepare_hf(w); assert(field(w,Field::HfValue,"11121314") == OK); assert(command(w,Command::HfPrepareUnit) == OK);
    token = w.view().review_token; p.fail_write = true;
    assert(command(w,Command::Confirm,0,token) == WRITE_UNVERIFIED && p.writes == 2);
    p.fail_write = false;
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.writes == 2);
    token = prepare_hf(w); unsigned calls = p.calls; w.cancel();
    assert(p.calls == calls && !w.view().review);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.writes == 2);
    w.clear(); assert(w.view().tool == Tool::None && !w.view().hf_ready && !w.view().hf_source);
    assert(command(w,Command::OpenHf) == OK); assert(prepare_hf(w) > token);
    Intent oversized; oversized.command = Command::SetField; oversized.field = Field::HfKey; oversized.text.fill('A');
    assert(w.process(oversized) == INVALID_ARGUMENT && !w.view().review);
    assert(field(w,Field::HfKeyType,"95") == INVALID_ARGUMENT);
    assert(field(w,Field::HfKey,"AABBCCDDEEFF") == OK && !w.view().hf_ready);
    assert(field(w,Field::HfStandardKeys,"1") == OK && w.view().hf_standard_keys);
    assert(field(w,Field::NtagMode,"3") == INVALID_ARGUMENT);
    p.card.family = HfFamily::Classic1K;
    for (unsigned b = 3; b < 64; b += 4) {
        p.pages[b].fill(0xab); p.pages[b][6] = 0xff; p.pages[b][7] = 7; p.pages[b][8] = 0x80; p.pages[b][9] = 0;
    }
    assert(command(w,Command::HfRead) == OK && command(w,Command::HfSelectUnit,3) == OK);
    assert(std::string(w.view().hf_data.data()) == "************FF078000************" && !w.view().hf_value[0]);
}
void hf_copy_and_records() {
    Mock p; Workbench w(p); command(w,Command::OpenHf); command(w,Command::HfRead);
    assert(command(w,Command::HfCapture) == OK && !w.view().hf_copy_armed);
    assert(command(w,Command::HfPrepareCopy) == PERMISSION_DENIED);
    p.hf_present = false; assert(command(w,Command::HfRead) == NO_CARD && w.view().hf_copy_armed);
    assert(command(w,Command::HfPrepareCopy) == PERMISSION_DENIED);
    p.hf_present = true; p.card.uid[1] = 9; p.pages[4][0] = 3;
    assert(command(w,Command::HfRead) == OK && command(w,Command::HfPrepareCopy) == OK && !p.writes);
    auto token = w.view().review_token; assert(command(w,Command::Confirm,0,token) == OK && p.writes == 1);
    assert(!w.view().hf_copy_armed && command(w,Command::HfPrepareCopy) == PERMISSION_DENIED);
    assert(command(w,Command::HfRead) == OK);
    for (unsigned kind = 0; kind < 19; ++kind) {
        assert(field(w,Field::NdefKind,std::to_string(kind)) == OK);
        assert(w.view().ndef_field_count > 0 && w.view().ndef_field_count <= 8);
    }
    assert(field(w,Field::NdefKind,"17") == OK);
    assert(field(w,Field::NdefField0,"Test network") == OK && field(w,Field::NdefField2,"example123") == OK);
    assert(w.view().ndef_fields[2].secret && std::string(w.view().ndef_fields[2].value.data()) == "********");
    assert(field(w,Field::NdefKind,"0") == OK && field(w,Field::NdefField0,"Hello") == OK);
    assert(command(w,Command::HfAddRecord) == OK && w.view().ndef_queue == 1);
    assert(command(w,Command::HfPrepareNdef) == OK && p.writes == 1);
    token = w.view().review_token; assert(command(w,Command::Confirm,0,token) == OK && w.view().ndef_queue == 0);
    assert(command(w,Command::HfRead) == OK && w.view().ndef_count == 1);
    assert(field(w,Field::NdefField0,"Appended") == OK && command(w,Command::HfAddRecord) == OK);
    assert(command(w,Command::HfPrepareNdef) == OK);
    assert(command(w,Command::Confirm,0,w.view().review_token) == OK);
    assert(command(w,Command::HfRead) == OK && w.view().ndef_count == 2);
    for (unsigned i = 0; i < 8; ++i) assert(command(w,Command::HfAddRecord) == OK);
    assert(command(w,Command::HfAddRecord) == INVALID_ARGUMENT && w.view().ndef_queue == 8);
    assert(command(w,Command::HfClearRecords) == OK && w.view().ndef_queue == 0 && w.view().ndef_count == 2);
}
void bulk_records() {
    Mock p; Workbench w(p); p.workbench = &w;
    assert(command(w,Command::OpenHf) == OK);
    assert(!w.view().bulk_overwrite && !w.view().bulk_formatting);
    assert(field(w,Field::BulkFamily,"2") == INVALID_ARGUMENT);
    assert(field(w,Field::BulkFamily,"3") == OK);
    assert(field(w,Field::NdefField0,"Bulk") == OK);
    for (unsigned i = 0; i < 4; ++i) assert(command(w,Command::HfAddRecord) == OK);
    assert(command(w,Command::HfArmBulk) == INVALID_ARGUMENT);
    assert(command(w,Command::HfRemoveRecord,3) == OK);
    assert(command(w,Command::HfMoveRecordUp,2) == OK);
    assert(command(w,Command::HfArmBulk) == OK && w.view().bulk_active && !p.writes);
    assert(field(w,Field::NdefField0,"Changed") == BUSY && command(w,Command::OpenUhf) == BUSY);
    assert(command(w,Command::HfRead) == OK);
    assert(command(w,Command::HfPrepareBulk) == PERMISSION_DENIED && !p.writes);
    p.hf_present = false; assert(command(w,Command::HfRead) == NO_CARD && w.view().bulk_phase == 2);
    p.hf_present = true; assert(command(w,Command::HfRead) == OK);
    assert(command(w,Command::HfPrepareBulk) == OK && w.view().review && !p.writes);
    auto token = w.view().review_token;
    assert(command(w,Command::CancelReview) == OK && !p.writes);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && !p.writes);
    assert(command(w,Command::HfPrepareBulk) == PERMISSION_DENIED);
    p.hf_present = false; command(w,Command::HfRead); p.hf_present = true; command(w,Command::HfRead);
    assert(command(w,Command::HfPrepareBulk) == OK && w.view().review_token > token);
    token = w.view().review_token;
    assert(command(w,Command::Confirm,0,token) == OK && w.view().bulk_written == 1 && w.view().ndef_queue == 3);
    auto writes = p.writes;
    assert(command(w,Command::HfRead) == OK && command(w,Command::HfPrepareBulk) == PERMISSION_DENIED);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.writes == writes);
    p.hf_present = false; command(w,Command::HfRead); p.hf_present = true; command(w,Command::HfRead);
    assert(command(w,Command::HfPrepareBulk) == OK);
    token = w.view().review_token; p.card.uid[1] ^= 1;
    assert(command(w,Command::Confirm,0,token) == STALE_DATA && p.writes == writes);
    assert(command(w,Command::HfPrepareBulk) == PERMISSION_DENIED);
    assert(command(w,Command::Stop) == OK && !w.view().bulk_active);
    // Canonical initialization remains distinct from the ordinary append path.
    command(w,Command::ClearSession); p.pages = {}; p.pages[41][3] = 255;
    field(w,Field::BulkFamily,"3"); field(w,Field::BulkFormatting,"1");
    field(w,Field::NdefField0,"Initialize"); command(w,Command::HfAddRecord); command(w,Command::HfArmBulk);
    p.hf_present = false; command(w,Command::HfRead); p.hf_present = true; command(w,Command::HfRead);
    assert(command(w,Command::HfPrepareBulk) == OK && !p.pages[3][0]);
    token = w.view().review_token; p.fail_write = true;
    assert(command(w,Command::Confirm,0,token) == WRITE_UNVERIFIED && w.view().bulk_failed == 1);
    p.fail_write = false;
    assert(command(w,Command::Stop) == WRITE_UNVERIFIED && w.view().bulk_active);
    assert(command(w,Command::HfRead) == WRITE_UNVERIFIED);
    assert(command(w,Command::HfArmBulk) == WRITE_UNVERIFIED);
    assert(command(w,Command::ClearSession) == OK && !w.view().bulk_active);
}
void select_uhf(Workbench& w) {
    assert(command(w,Command::OpenUhf) == OK && command(w,Command::UhfStart) == OK && w.view().inventory);
    assert(w.tick() == OK && w.view().tag_count == 1);
    assert(w.view().busy);
    assert(command(w,Command::UhfSelectTag,0) == BUSY);
    assert(command(w,Command::Stop) == OK && w.view().tag_count == 1 && !w.view().inventory);
    assert(command(w,Command::UhfSelectTag,0) == OK && w.view().selected_tag == 0);
}
void uhf_reviews() {
    Mock p; Workbench w(p); p.workbench = &w; select_uhf(w);
    assert(command(w,Command::UhfRead) == OK && w.view().uhf_memory);
    assert(field(w,Field::UhfValue,"ABCD") == OK && command(w,Command::UhfPrepareUser) == OK && !p.writes);
    auto token = w.view().review_token; p.user[0] = 9;
    assert(command(w,Command::Confirm,0,token) == STALE_DATA && !p.writes);
    assert(command(w,Command::UhfRead) == OK && field(w,Field::UhfValue,"ABCD") == OK);
    assert(command(w,Command::UhfPrepareUser) == OK);
    token = w.view().review_token; assert(command(w,Command::Confirm,0,token) == OK && p.writes == 1);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.writes == 1);
    assert(field(w,Field::UhfBank,"0") == OK && command(w,Command::UhfRead) == OK);
    assert(command(w,Command::UhfPrepareUser) == PERMISSION_DENIED && p.writes == 1);
    assert(field(w,Field::UhfEpc,"0102030405060708") == OK && command(w,Command::UhfPrepareEpc) == OK);
    assert(p.epc_read_words == 5 && p.writes == 1);
    token = w.view().review_token; assert(command(w,Command::Confirm,0,token) == OK && p.writes == 2 && p.epc.size() == 8);
    assert(field(w,Field::UhfPower,"26") == OK && command(w,Command::UhfPreparePower) == OK && !p.power_writes);
    token = w.view().review_token; assert(command(w,Command::Confirm,0,token) == OK && p.power_writes == 1 && p.rf == 2600);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.power_writes == 1);
    assert(field(w,Field::UhfPower,"17") == INVALID_ARGUMENT);
    assert(command(w,Command::UhfReadPower) == OK && w.view().uhf_power == 26);
    assert(command(w,Command::UhfPreparePower) == OK); token = w.view().review_token;
    assert(field(w,Field::UhfPassword,"AABBCCDD") == OK && !w.view().review);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.power_writes == 1);
    assert(command(w,Command::UhfStart) == OK);
    Bytes report(p.pc.begin(),p.pc.end()); report.insert(report.end(),p.epc.begin(),p.epc.end()); auto tag_crc = crc(report);
    report.insert(report.begin(),0xc9); report.push_back(uint8_t(tag_crc>>8)); report.push_back(uint8_t(tag_crc));
    auto wire = m100(0x22,report,2); unsigned calls = p.calls;
    w.feed_uhf(Bytes(wire.begin(),wire.begin()+3)); w.feed_uhf(Bytes(wire.begin()+3,wire.end()));
    assert(w.view().tags[0].reads >= 2 && p.calls == calls);
    w.cancel();
    assert(!w.view().inventory && p.calls == calls); assert(w.tick() == OK && p.calls == calls);
    auto reads = w.view().tags[0].reads; w.feed_uhf(wire); assert(w.view().tags[0].reads == reads && p.calls == calls);
}
void lf_reviews() {
    Mock p; Workbench w(p); p.workbench = &w; command(w,Command::OpenLf);
    assert(w.view().lf_scan_mask == 255 && !(w.view().lf_write_mask&(1u<<2)) && !(w.view().lf_write_mask&(1u<<7)));
    assert(command(w,Command::LfScan) == OK && w.view().lf_present && !w.view().lf_card_valid);
    assert(command(w,Command::LfCapture) == OK && w.view().lf_card_valid && !w.view().lf_write_armed);
    auto desired = std::string(w.view().lf_value.data());
    assert(command(w,Command::LfPrepareWrite) == PERMISSION_DENIED && !p.lf_writes);
    p.lf_present = false; p.reader_mode_no_card = true;
    assert(command(w,Command::LfScan) == NO_CARD && !w.view().lf_write_armed);
    p.reader_mode_no_card = false;
    assert(command(w,Command::LfScan) == NO_CARD && w.view().lf_write_armed);
    p.lf_present = true; p.actual_lf.data[0] = 8;
    assert(command(w,Command::LfScan) == OK && std::string(w.view().lf_value.data()) == desired);
    assert(command(w,Command::LfPrepareWrite) == OK && !p.lf_writes);
    auto token = w.view().review_token; p.actual_lf.data[1] ^= 1;
    assert(command(w,Command::Confirm,0,token) == STALE_DATA && !p.lf_writes && !w.view().lf_write_armed);
    assert(command(w,Command::LfTestId) == OK && std::string(w.view().lf_value.data()) == "0102030405");
    p.lf_present = false; assert(command(w,Command::LfScan) == NO_CARD && w.view().lf_write_armed);
    assert(command(w,Command::LfPrepareWrite) == OK && !p.lf_writes);
    token = w.view().review_token; assert(command(w,Command::Confirm,0,token) == OK && p.lf_writes == 3 && !w.view().lf_write_armed);
    assert(command(w,Command::Confirm,0,token) == PERMISSION_DENIED && p.lf_writes == 3);
    assert(field(w,Field::LfFormat,"5") == OK && field(w,Field::LfId,"12345678") == OK);
    assert(std::string(w.view().lf_value.data()) == "12345678");
    assert(field(w,Field::LfHint,"31") == INVALID_ARGUMENT && field(w,Field::LfHint,"42") == OK);
    assert(field(w,Field::LfFormat,"2") == OK && field(w,Field::LfId,"1A000000000000000000000000") == OK);
    p.lf_present = false; assert(command(w,Command::LfScan) == NO_CARD);
    assert(command(w,Command::LfPrepareWrite) == UNSUPPORTED && p.lf_writes == 3);
    assert(command(w,Command::LfTestId) == OK && command(w,Command::LfScan) == NO_CARD);
    assert(command(w,Command::LfPrepareWrite) == OK); token = w.view().review_token; ++p.gens[2];
    assert(command(w,Command::Confirm,0,token) == STALE_GENERATION && p.lf_writes == 3);
    p.lf_writer = false; w.refresh(); assert(!w.view().lf_write_mask);
    w.clear(); assert(!w.view().lf_card_valid && !w.view().lf_write_armed && !w.view().hf_source && w.view().selected_tag == 64);
}
}
int main() {
    hf_reviews(); hf_copy_and_records(); bulk_records(); uhf_reviews(); lf_reviews();
    Mock port; Workbench workbench(port);
    for (auto open : {Command::OpenHf,Command::OpenUhf,Command::OpenLf}) {
        assert(command(workbench,open) == OK);
        const auto tool = workbench.view().tool;
        assert(command(workbench,Command::ClearSession) == OK && workbench.view().tool == tool);
        assert(!workbench.view().review && !workbench.view().hf_ready && !workbench.view().lf_present);
        assert(workbench.view().tag_count == 0 && workbench.view().selected_tag == 64);
    }
    assert(command(workbench,Command::Exit) == OK && workbench.view().tool == Tool::None);
    std::cout << "Portable controller tests passed: reviews, replay, stale snapshots, fields and removal gates.\n";
}
