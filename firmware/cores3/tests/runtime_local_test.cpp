#include "local_runtime.hpp"
#include "runtime.hpp"
#include "standalone/device.hpp"
#include "standalone/format.hpp"
#include <cassert>
#include <cstring>
#include <iostream>

namespace fake {
uint64_t epoch = 1;
bool allowed = true, interrupt_write = false, no_card = false;
unsigned writes = 0, reads = 0, polls = 0, releases = 0, channel_releases = 0;
qb::Status release_status = qb::OK;
std::array<std::array<uint8_t, 16>, 64> memory{};
std::function<void(const qb::Bytes&)> notify;
unsigned busy_checks = 0;
void expect_busy() {
    qb::local::View snapshot; qb::local_runtime_snapshot(snapshot);
    assert(snapshot.busy && !snapshot.review && !snapshot.review_token);
    ++busy_checks;
}
qb::local::HfIdentity identity() {
    qb::local::HfIdentity card;
    card.family = qb::local::HfFamily::Classic1K;
    card.uid_size = 4; card.uid = {1, 2, 3, 4};
    card.atqa = {4, 0}; card.sak = 8;
    return card;
}
void factory() {
    for (unsigned i = 0; i < memory.size(); ++i) {
        memory[i].fill(uint8_t(i));
        if (qb::trailer(i) == i) {
            memory[i].fill(0xff);
            memory[i][6] = 0xff; memory[i][7] = 0x07;
            memory[i][8] = 0x80; memory[i][9] = 0x69;
        }
    }
}
qb::Bytes m100(uint8_t cmd, const qb::Bytes& payload, uint8_t kind = 1) {
    qb::Bytes out{0xbb, kind, cmd, uint8_t(payload.size() >> 8), uint8_t(payload.size())};
    out.insert(out.end(), payload.begin(), payload.end());
    uint8_t sum = 0;
    for (size_t i = 1; i < out.size(); ++i) sum += out[i];
    out.push_back(sum); out.push_back(0x7e);
    return out;
}
qb::Bytes report() {
    return m100(0x22, {0xc9, 0x34, 0x00, 0x30, 0x75, 0x1f, 0xeb, 0x70,
                       0x5c, 0x59, 0x04, 0xe3, 0xd5, 0x0d, 0x70, 0x3a, 0x76}, 2);
}
}
namespace qb {
std::mutex state_mutex;
std::array<Slot, 3> slots;
Session session;
std::atomic<bool> stop_operation{false};
bool local_operation_allowed(uint64_t epoch) { return fake::allowed && fake::epoch == epoch; }
void hardware_local_uhf_handler(std::function<void(const Bytes&)> handler) { fake::notify = std::move(handler); }
Status hardware_release_channel(uint8_t channel) {
    assert(channel >= 1 && channel <= 3);
    ++fake::channel_releases;
    return fake::release_status;
}
Status hardware_release() { ++fake::releases; return fake::release_status; }
namespace local {
bool DevicePort::cancelled() const { return stop_operation || !local_operation_allowed(epoch_); }
uint32_t DevicePort::generation(uint8_t channel) const {
    if (channel < 1 || channel > 3 || cancelled()) return 0;
    std::lock_guard<std::mutex> guard(state_mutex);
    const auto& slot = slots[channel - 1];
    return slot.present && slot.generation == generations_[channel - 1] ? slot.generation : 0;
}
bool DevicePort::supports(uint8_t channel, uint16_t command) const {
    if (!generation(channel)) return false;
    if (channel == 1) return command >= 16 && command <= 22;
    if (channel == 2) {
        switch (command) {
        case 0x0b: case 0x0c: case 0x12: case 0x22: case 0x28: case 0x39: case 0x49: case 0xb6: case 0xb7:
            return true;
        default: return false;
        }
    }
    return false;
}
void DevicePort::progress(unsigned done, unsigned total, const char* detail) {
    if (progress_) progress_(done, total, detail);
}
void DevicePort::wait_ms(unsigned milliseconds) { assert(milliseconds <= 50); }
Status DevicePort::identify(HfIdentity& card) {
    fake::expect_busy();
    if (cancelled()) return CANCELLED;
    if (!generation(1)) return STALE_GENERATION;
    if (fake::no_card) return NO_CARD;
    used_ |= 1;
    card = fake::identity();
    return OK;
}
Status DevicePort::read_classic(const HfIdentity& card, unsigned block, const ClassicKey& key,
                                std::array<uint8_t, 16>& output) {
    if (cancelled()) return CANCELLED;
    if (!generation(1)) return STALE_GENERATION;
    assert(card == fake::identity() && block < fake::memory.size());
    assert(key.type == 0x60);
    for (auto byte : key.bytes) assert(byte == 0xff);
    used_ |= 1; ++fake::reads;
    output = fake::memory[block];
    return OK;
}
Status DevicePort::write_classic(const HfIdentity& card, unsigned block, const ClassicKey& key,
                                 const std::array<uint8_t, 16>& previous,
                                 const std::array<uint8_t, 16>& desired,
                                 std::array<uint8_t, 16>& verified) {
    if (cancelled()) return CANCELLED;
    if (!generation(1)) return STALE_GENERATION;
    assert(card == fake::identity() && classic_data(64, block) && key.type == 0x60);
    fake::expect_busy();
    if (previous != fake::memory[block]) return STALE_DATA;
    used_ |= 1; ++fake::writes; fake::memory[block] = desired;
    if (fake::interrupt_write) {
        stop_operation = true; ++fake::epoch;
        assert(generation(1) == 0);
        return WRITE_UNVERIFIED;
    }
    verified = desired;
    return OK;
}
Status DevicePort::read_pages(const HfIdentity&, unsigned, unsigned, const NtagAuth&, Bytes&) {
    assert(false); return UNSUPPORTED;
}
Status DevicePort::write_page(const HfIdentity&, unsigned, const NtagAuth&, const std::array<uint8_t, 4>&,
                              const std::array<uint8_t, 4>&, std::array<uint8_t, 4>&) {
    assert(false); return UNSUPPORTED;
}
Status DevicePort::reset_hf() { used_ |= 1; return cancelled() ? CANCELLED : OK; }
Status DevicePort::format_ndef(const HfIdentity& card, uint8_t flags, const Bytes& snapshot) {
    if (card != fake::identity()) return STALE_DATA;
    local::FormatIO io;
    io.guard = [&]() { return cancelled() ? CANCELLED : !generation(1) ? STALE_GENERATION : OK; };
    io.read = [&](unsigned b,const ClassicKey&,Bytes& data) {
        data.assign(fake::memory[b].begin(),fake::memory[b].end()); return OK;
    };
    io.write = [&](unsigned b,const ClassicKey&,const Bytes& data) {
        ++fake::writes; std::copy(data.begin(),data.end(),fake::memory[b].begin()); return OK;
    };
    return local::canonical_format(card,flags,snapshot,io);
}
Status DevicePort::transact(uint8_t channel, const Bytes& command, Bytes& response) {
    if (cancelled()) return CANCELLED;
    if (!generation(channel)) return STALE_GENERATION;
    assert(channel == 2 && validate_uhf(command) == OK);
    used_ |= 2;
    switch (command[2]) {
    case 0x12: response = fake::m100(0x12, {0}); return OK;
    case 0x22:
        ++fake::polls;
        response = fake::report();
        if (fake::notify) fake::notify(fake::report());
        return OK;
    case 0x28: response = fake::m100(0x28, {0}); return OK;
    default: assert(false); return UNSUPPORTED;
    }
}
}
}
extern "C" int64_t esp_timer_get_time(void) {
    static int64_t clock = 0;
    return clock += 100001;
}
using namespace qb;
using namespace qb::local;
static View view() { View snapshot; local_runtime_snapshot(snapshot); return snapshot; }
static Status process(Command command, unsigned index = 0, uint64_t token = 0) {
    Intent intent;
    intent.command = command; intent.index = index; intent.review_token = token;
    return local_runtime_process(intent, fake::epoch);
}
static Status field(Field field, const char* text) {
    Intent intent; intent.command = Command::SetField; intent.field = field;
    assert(std::strlen(text) < intent.text.size());
    std::memcpy(intent.text.data(), text, std::strlen(text));
    return local_runtime_process(intent, fake::epoch);
}
static uint64_t prepare_hf() {
    assert(process(Command::OpenHf) == OK);
    assert(process(Command::HfRead) == OK);
    auto current = view();
    assert(current.hf_ready && current.hf_read_count == 64 && current.hf_units == 64);
    assert(field(Field::HfUnit, "4") == OK);
    assert(view().hf_writable);
    assert(field(Field::HfValue, "0102030405060708090a0b0c0d0e0f10") == OK);
    assert(process(Command::HfPrepareUnit) == OK);
    assert(view().review && view().review_token && local_runtime_active());
    return view().review_token;
}
static void interrupted_hf() {
    const auto token = prepare_hf();
    fake::interrupt_write = true;
    assert(process(Command::Confirm, 0, token) == WRITE_UNVERIFIED);
    assert(fake::writes == 1 && stop_operation && view().status == WRITE_UNVERIFIED);
    for (unsigned i = 0; i < 16; ++i) assert(fake::memory[4][i] == i + 1);
    assert(local_runtime_cancel(fake::epoch, false) == OK);
    assert(view().status == WRITE_UNVERIFIED && !view().review && !view().review_token);
    stop_operation = false;
    local_runtime_refresh(fake::epoch);
    auto current = view();
    assert(current.status == WRITE_UNVERIFIED);
    assert(std::string(current.message.data()).find("partly changed") != std::string::npos);
    assert(!current.review && !current.busy && !current.hf_copy_armed && !local_runtime_active());
    const auto writes = fake::writes;
    assert(process(Command::Confirm, 0, token) == WRITE_UNVERIFIED && fake::writes == writes);
    fake::interrupt_write = false;
    assert(process(Command::ClearSession) == OK);
    assert(view().tool == Tool::Hf && !view().hf_ready);
}
static void unverified_bulk_survives_lifecycle_changes() {
    fake::factory();
    for (unsigned b = 1; b < 64; ++b) if (classic_data(64,b)) fake::memory[b].fill(0);
    Bytes mad(32,0); mad[1] = 1;
    for (unsigned sector = 1; sector < 16; ++sector) {
        mad[sector*2] = 3; mad[sector*2+1] = 0xe1;
        fake::memory[sector*4+3][9] = 0x40;
    }
    uint8_t crc = 0xc7;
    for (unsigned i = 1; i < mad.size(); ++i) {
        crc ^= mad[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = uint8_t((crc<<1) ^ (crc&128 ? 0x1d : 0));
    }
    mad[0] = crc;
    std::copy_n(mad.begin(),16,fake::memory[1].begin());
    std::copy_n(mad.begin()+16,16,fake::memory[2].begin());
    fake::memory[3][9] = 0xc1;
    fake::memory[4][0] = 3; fake::memory[4][2] = 0xfe;
    assert(process(Command::OpenHf) == OK);
    assert(field(Field::NdefField0,"Sticky bulk warning") == OK);
    assert(process(Command::HfAddRecord) == OK);
    assert(process(Command::HfArmBulk) == OK);
    fake::no_card = true; assert(process(Command::HfRead) == NO_CARD);
    fake::no_card = false; assert(process(Command::HfRead) == OK);
    assert(process(Command::HfPrepareBulk) == OK);
    const auto token = view().review_token;
    const auto writes = fake::writes;
    fake::interrupt_write = true;
    assert(process(Command::Confirm,0,token) == WRITE_UNVERIFIED && fake::writes == writes+1);
    fake::interrupt_write = false; stop_operation = false;
    assert(local_runtime_cancel(fake::epoch,false) == OK);
    ++slots[1].generation;
    local_runtime_refresh(fake::epoch);
    assert(view().status == WRITE_UNVERIFIED && !view().review);
    assert(process(Command::HfArmBulk) == WRITE_UNVERIFIED && fake::writes == writes+1);
    assert(local_runtime_cancel(fake::epoch,true) == OK);
    assert(process(Command::Exit) == OK && view().status == WRITE_UNVERIFIED);
    assert(process(Command::OpenHf) == OK && view().status == WRITE_UNVERIFIED);
    assert(process(Command::HfRead) == OK && view().status == WRITE_UNVERIFIED);
    assert(process(Command::HfPrepareUnit) == WRITE_UNVERIFIED && fake::writes == writes+1);
    fake::release_status = IO_ERROR;
    assert(process(Command::ClearSession) == OK);
    assert(local_runtime_release_status() == IO_ERROR && view().status == WRITE_UNVERIFIED);
    fake::release_status = OK;
    assert(process(Command::ClearSession) == OK && view().status == OK);
    fake::factory();
}
static void uhf_stop_restart() {
    assert(process(Command::OpenUhf) == OK);
    assert(process(Command::UhfStart) == OK && local_runtime_inventory());
    assert(local_runtime_tick(fake::epoch) == OK);
    const auto before = view();
    assert(before.inventory && before.tag_count == 1 && before.tags[0].reads == 2);
    assert(before.tags[0].size == 12 && before.tags[0].rssi == -55);
    const std::array<uint8_t, 12> epc{{0x30,0x75,0x1f,0xeb,0x70,0x5c,0x59,0x04,0xe3,0xd5,0x0d,0x70}};
    assert(std::equal(epc.begin(), epc.end(), before.tags[0].epc.begin()));
    stop_operation = true; ++fake::epoch;
    assert(local_runtime_cancel(fake::epoch, false) == OK);
    stop_operation = false;
    local_runtime_refresh(fake::epoch);
    const auto after = view();
    assert(!after.inventory && !local_runtime_inventory() && !local_runtime_active());
    assert(after.tag_count == before.tag_count && after.tags[0].epc == before.tags[0].epc);
    assert(after.tags[0].reads == before.tags[0].reads);
    assert(process(Command::UhfSelectTag, 0) == OK && view().selected_tag == 0);
    assert(process(Command::UhfStart) == OK && local_runtime_inventory());
    assert(local_runtime_tick(fake::epoch) == OK && view().tags[0].reads == 4);
    assert(fake::polls == 2);
    stop_operation = true; ++fake::epoch;
    assert(local_runtime_cancel(fake::epoch, false) == OK);
    stop_operation = false;
    local_runtime_refresh(fake::epoch);
    assert(view().tag_count == 1 && !view().inventory);
    assert(process(Command::ClearSession) == OK);
}
static void stale_confirmation() {
    fake::memory[4].fill(0x44);
    const auto token = prepare_hf();
    const auto writes = fake::writes;
    ++slots[0].generation;
    assert(process(Command::Confirm, 0, token) == STALE_GENERATION);
    const auto stale = view();
    assert(stale.status == STALE_GENERATION && !stale.review && !stale.review_token);
    assert(!stale.hf_ready && fake::writes == writes && !local_runtime_active());
    assert(std::string(stale.message.data()).find("Reader changed") != std::string::npos);
    assert(process(Command::Confirm, 0, token) == PERMISSION_DENIED && fake::writes == writes);
    assert(process(Command::ClearSession) == OK);
    ++slots[0].generation;
    local_runtime_refresh(fake::epoch);
    assert(view().status == STALE_GENERATION && !view().review);
}
static void cleanup_failure() {
    assert(process(Command::OpenHf) == OK);
    fake::release_status = IO_ERROR;
    assert(process(Command::HfRead) == OK);
    assert(local_runtime_release_status() == IO_ERROR);
    auto failure = view();
    assert(failure.status == IO_ERROR && !failure.review && !failure.busy);
    assert(std::string(failure.message.data()).find("release unverified") != std::string::npos);
    stop_operation = true; ++fake::epoch;
    assert(local_runtime_cancel(fake::epoch, false) == IO_ERROR);
    assert(local_runtime_release_status() == IO_ERROR && view().status == IO_ERROR);
    assert(std::string(view().message.data()).find("release unverified") != std::string::npos);
    fake::release_status = OK; stop_operation = false;
    local_runtime_refresh(fake::epoch);
    assert(process(Command::ClearSession) == OK);
}
int main() {
    fake::factory();
    slots[0] = {7, true, "rfid", {"identify", "classic1k"}};
    slots[1] = {9, true, "jrd4035", {"m100"}};
    slots[2] = {11, false, "chameleon", {}};
    local_runtime_initialize();
    interrupted_hf(); unverified_bulk_survives_lifecycle_changes();
    uhf_stop_restart(); stale_confirmation(); cleanup_failure();
    assert(fake::releases >= 4 && fake::channel_releases > 0 && fake::busy_checks > 0);
    std::cout << "Actual local runtime tests passed: interrupted HF write+Stop preserves unverified; "
                 "UHF Stop retains tags and restarts; stale Confirm cannot write; release failure published\n";
}
