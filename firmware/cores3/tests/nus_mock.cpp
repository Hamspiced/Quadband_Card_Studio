#include <algorithm>
#include <deque>
#include <memory>
#include <cstring>
#include <cstdio>
#include "../main/nus.cpp"

namespace {
struct Queue { size_t capacity, width; std::deque<std::vector<uint8_t>> entries; };
std::vector<std::unique_ptr<Queue>> queues;
std::deque<ble_npl_event*> events;
std::deque<std::function<void()>> callbacks;
int64_t now = 0;
unsigned scans = 0, connects = 0, terminates = 0, writes = 0, injected = 0;
bool role_allowed = true, reply_on_write = false, fail_write = false, interrupt_write = false;
ble_gap_conn_desc peer{};
ble_gap_event_fn radio = nullptr;
void* radio_arg = nullptr;
ble_gatt_svc_fn service_cb = nullptr;
ble_gatt_chr_fn chr_cb = nullptr;
ble_gatt_dsc_fn dsc_cb = nullptr;
void* discovery_arg = nullptr;
qb::Bytes next_reply;
void pump() {
    while (!events.empty()) { auto* e = events.front(); events.pop_front(); e->callback(e); }
    while (!callbacks.empty()) { auto f = callbacks.front(); callbacks.pop_front(); f(); }
}
void event(ble_gap_event e) { assert(radio); radio(&e, radio_arg); }
void notification(const qb::Bytes& bytes, uint16_t handle = 7) {
    os_mbuf om{}; assert(bytes.size() <= sizeof(om.bytes));
    om.size = bytes.size(); std::copy(bytes.begin(), bytes.end(), om.bytes);
    ble_gap_event e{}; e.type = BLE_GAP_EVENT_NOTIFY_RX; e.notify_rx = {handle, 4, &om}; event(e);
}
qb::Bytes frame(unsigned command, const qb::Bytes& payload = {}) {
    qb::Bytes b{0x11,0xef,uint8_t(command>>8),uint8_t(command),0,0x68,
        uint8_t(payload.size()>>8),uint8_t(payload.size()),0};
    uint8_t sum = 0; for (auto v : b) sum += v; b[8] = -sum;
    b.insert(b.end(), payload.begin(), payload.end()); sum = 0;
    for (auto v : payload) sum += v; b.push_back(-sum); return b;
}
void emit_disconnect() {
    ble_gap_event e{}; e.type = BLE_GAP_EVENT_DISCONNECT; e.disconnect.conn = peer; event(e);
}
void reset() {
    events.clear(); callbacks.clear();
    qb::connection = BLE_HS_CONN_HANDLE_NONE; qb::cancel_pending = false;
    qb::receiving = qb::write_pending = false; qb::lifecycle.reset();
    xQueueReset(qb::fragments); xQueueReset(qb::commands);
    peer = {}; peer.role = 1; peer.conn_handle = 7; peer.peer_id_addr.val[0] = 0x42;
    peer.peer_ota_addr = peer.peer_id_addr; peer.sec_state = {1,1,1,16};
    role_allowed = true; reply_on_write = fail_write = interrupt_write = false;
}
void select() {
    assert(qb::nus_scan()); pump();
    ble_gap_event e{}; e.type = BLE_GAP_EVENT_DISC; e.disc.addr = peer.peer_ota_addr;
    event(e); event(e);
    assert(qb::lifecycle.view.count == 1);
    e.type = BLE_GAP_EVENT_DISC_COMPLETE; event(e);
    assert(!qb::nus_select(qb::lifecycle.view.scan_epoch - 1, 0));
    assert(qb::nus_select(qb::lifecycle.view.scan_epoch, 0)); pump();
    e = {}; e.type = BLE_GAP_EVENT_CONNECT; e.connect.conn_handle = 7; event(e);
}
void secure() {
    ble_gap_event e{}; e.type = BLE_GAP_EVENT_ENC_CHANGE; e.enc_change.conn_handle = 7; event(e);
}
void ready(bool writable = true, bool cccd_present = true, bool subscription_error = false) {
    select(); secure(); assert(service_cb);
    ble_gatt_error ok{}, done{BLE_HS_EDONE};
    ble_gatt_svc svc{1,6}; service_cb(7,&ok,&svc,discovery_arg); service_cb(7,&done,nullptr,discovery_arg);
    ble_gatt_chr rx{}; rx.def_handle = 1; rx.val_handle = 2;
    rx.properties = writable ? BLE_GATT_CHR_PROP_WRITE : 0; rx.uuid.u128 = qb::rx;
    chr_cb(7,&ok,&rx,discovery_arg);
    if (!writable) return;
    ble_gatt_chr tx{}; tx.def_handle = 3; tx.val_handle = 4; tx.properties = BLE_GATT_CHR_PROP_NOTIFY; tx.uuid.u128 = qb::tx;
    chr_cb(7,&ok,&tx,discovery_arg); chr_cb(7,&done,nullptr,discovery_arg);
    ble_gatt_dsc dsc{}; dsc.handle = 5; dsc.uuid.u.type = 16;
    if (cccd_present) dsc_cb(7,&ok,4,&dsc,discovery_arg);
    fail_write = subscription_error;
    dsc_cb(7,&done,4,nullptr,discovery_arg); pump(); fail_write = false;
    if (cccd_present && !subscription_error) { assert(qb::nus_subscribed()); qb::nus_validated(true); }
}
}

QueueHandle_t xQueueCreate(size_t capacity, size_t width) {
    queues.emplace_back(std::make_unique<Queue>(Queue{capacity,width,{}})); return queues.back().get();
}
int xQueueSend(QueueHandle_t h, const void* p, unsigned) {
    auto& q = *static_cast<Queue*>(h); if (q.entries.size() == q.capacity) return 0;
    const auto* b = static_cast<const uint8_t*>(p); q.entries.emplace_back(b,b+q.width); return pdTRUE;
}
int xQueueReceive(QueueHandle_t h, void* p, unsigned wait) {
    auto& q = *static_cast<Queue*>(h);
    if (q.entries.empty()) { now += int64_t(wait) * 1000; return 0; }
    std::memcpy(p,q.entries.front().data(),q.width); q.entries.pop_front(); return pdTRUE;
}
void xQueueReset(QueueHandle_t h) { static_cast<Queue*>(h)->entries.clear(); }
unsigned uxQueueMessagesWaiting(QueueHandle_t h) { return static_cast<Queue*>(h)->entries.size(); }
void vTaskDelay(unsigned ms) { now += int64_t(ms)*1000; pump(); }
int64_t esp_timer_get_time() { return now; }
namespace qb {
bool ble_role_allowed(const ble_gap_conn_desc&, bool, bool) { return role_allowed; }
uint32_t usb_timeout_ms(unsigned) { return 500; }
}
extern "C" {
void ble_npl_event_init(ble_npl_event* e, void (*cb)(ble_npl_event*), void*) { e->callback = cb; }
void* nimble_port_get_dflt_eventq() { return nullptr; }
void ble_npl_eventq_put(void*, ble_npl_event* e) {
    if (std::find(events.begin(), events.end(), e) == events.end()) events.push_back(e);
}
int ble_gap_terminate(uint16_t, uint8_t) { ++terminates; return 0; }
int ble_gap_conn_find(uint16_t h, ble_gap_conn_desc* out) { if (h != 7) return 1; *out = peer; return 0; }
int ble_gap_security_initiate(uint16_t) { return 0; }
int ble_hs_id_infer_auto(int, uint8_t* out) { *out = 0; return 0; }
int ble_gap_disc(uint8_t, int32_t timeout, const ble_gap_disc_params* p, ble_gap_event_fn cb, void* arg) {
    assert(timeout == 8000 && p->passive); ++scans; radio = cb; radio_arg = arg; return 0;
}
int ble_gap_disc_cancel() { return 0; }
int ble_gap_conn_cancel() { return 0; }
int ble_gap_conn_active() { return 0; }
int ble_gap_disc_active() { return 0; }
int ble_gap_connect(uint8_t, const ble_addr_t* addr, int32_t, const void*, ble_gap_event_fn cb, void* arg) {
    assert(!std::memcmp(addr,&peer.peer_ota_addr,sizeof(*addr))); ++connects; radio=cb; radio_arg=arg; return 0;
}
int ble_hs_adv_parse_fields(ble_hs_adv_fields* out, const uint8_t*, uint8_t) {
    out->num_uuids128 = 1; out->uuids128 = const_cast<ble_uuid128_t*>(&qb::service); return 0;
}
int ble_uuid_cmp(const ble_uuid_t* a, const ble_uuid_t* b) { return std::memcmp(a,b,sizeof(ble_uuid128_t)); }
uint16_t ble_uuid_u16(const ble_uuid_t* u) { return u->type == 16 ? 0x2902 : 0; }
int ble_gattc_disc_svc_by_uuid(uint16_t, const ble_uuid_t*, ble_gatt_svc_fn cb, void* arg) { service_cb=cb; discovery_arg=arg; return 0; }
int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn cb, void* arg) { chr_cb=cb; discovery_arg=arg; return 0; }
int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn cb, void* arg) { dsc_cb=cb; discovery_arg=arg; return 0; }
int ble_gattc_write_flat(uint16_t h, uint16_t attr, const void*, uint16_t n, ble_gatt_attr_fn cb, void* arg) {
    assert(n <= 20); if (attr == 2) ++writes;
    const int error = fail_write ? 1 : 0;
    callbacks.push_back([=] {
        if (interrupt_write) { qb::nus_cancel(); return; }
        ble_gatt_error status{error}; cb(h,&status,nullptr,arg);
        if (!error && attr == 2 && reply_on_write) {
            notification(qb::Bytes(next_reply.begin(),next_reply.begin()+5));
            notification(qb::Bytes(next_reply.begin()+5,next_reply.end()));
        }
    }); return 0;
}
int ble_hs_mbuf_to_flat(const os_mbuf* om, void* out, uint16_t n, uint16_t* copied) {
    if (om->size > n) return 1;
    std::memcpy(out,om->bytes,om->size); *copied=om->size; return 0;
}
int ble_sm_inject_io(uint16_t, const ble_sm_io* io) { assert(io->action == BLE_SM_IOACT_INPUT); ++injected; return 0; }
}
int main() {
    using namespace qb;
    ScopedBondTable<unsigned> bonds; const auto eq=[](unsigned a,unsigned b){return a==b;};
    unsigned old_desktop=11;
    bonds.reconcile(&old_desktop,1,true,eq);
    assert(bonds.roles[0].valid && bonds.roles[0].peer==old_desktop && !bonds.ambiguous);
    assert(bonds.allows(false,11,eq) && !bonds.allows(false,12,eq));
    assert(!bonds.allows(true,11,eq) && bonds.allows(true,22,eq));
    bonds.roles[1]={true,22}; unsigned both[]{11,22}; bonds.reconcile(both,2,false,eq);
    assert(!bonds.ambiguous && !bonds.allows(true,23,eq));
    bonds.roles[0]={}; // Forget desktop only; Ultra remains reserved.
    assert(bonds.roles[1].valid && bonds.roles[1].peer==22 && bonds.allows(false,12,eq));
    ScopedBondTable<unsigned> orphan; orphan.reconcile(&old_desktop,1,false,eq);
    assert(orphan.ambiguous && !orphan.allows(false,11,eq));
    ScopedBondTable<unsigned> ambiguous; ambiguous.reconcile(both,2,true,eq);
    assert(ambiguous.ambiguous && !ambiguous.allows(true,22,eq));
    nus_init(); reset();
    ready(); assert(lifecycle.view.phase == UltraPhase::Ready);
    auto command = frame(1000), out = Bytes{};
    next_reply = frame(1000,{2,0}); reply_on_write = true;
    const unsigned before = writes;
    assert(nus_transaction(command,out) == OK && out == next_reply && writes == before+1);
    // A second desktop handle cannot deliver notifications into the selected Ultra.
    notification({1,2,3},9); assert(lifecycle.view.phase == UltraPhase::Ready);
    // Overflow, malformed/coalesced replies and unsolicited data fail closed.
    receiving = true; for (unsigned i=0;i<13;++i) notification({0x11});
    assert(lifecycle.view.phase == UltraPhase::Failed);
    reset(); ready(); reply_on_write = true; next_reply = frame(1033,{0});
    assert(nus_transaction(command,out) == IO_ERROR);
    reset(); ready(); fail_write = true;
    const unsigned failed_before = writes;
    assert(nus_transaction(command,out) == IO_ERROR && writes == failed_before+1);
    reset(); ready(); interrupt_write = true;
    assert(nus_transaction(command,out) == IO_ERROR);
    reset(); ready(false); assert(lifecycle.view.phase == UltraPhase::Failed);
    reset(); ready(true,false); assert(lifecycle.view.phase == UltraPhase::Failed);
    reset(); ready(true,true,true); assert(lifecycle.view.phase == UltraPhase::Failed);
    // Each security property is mandatory; there is no unencrypted fallback.
    for (unsigned field=0;field<4;++field) {
        reset(); select();
        if (field==0) peer.sec_state.encrypted=0;
        if (field==1) peer.sec_state.authenticated=0;
        if (field==2) peer.sec_state.bonded=0;
        if (field==3) peer.sec_state.key_size=12;
        secure(); assert(lifecycle.view.phase == UltraPhase::Failed);
    }
    reset(); role_allowed=false; select(); assert(lifecycle.view.phase == UltraPhase::Failed);
    reset(); select();
    ble_gap_event pin{}; pin.type=BLE_GAP_EVENT_PASSKEY_ACTION; pin.passkey.conn_handle=7; pin.passkey.params.action=BLE_SM_IOACT_INPUT;
    event(pin); auto challenge=lifecycle.view.challenge, epoch=lifecycle.view.epoch;
    char input[7]; std::fill_n(input,6,'0'); input[6]=0;
    assert(!nus_input(epoch,challenge+1,input));
    assert(!nus_input(epoch+1,challenge,input));
    input[0]='x'; assert(!nus_input(epoch,challenge,input)); input[0]='0';
    now=lifecycle.view.expires+1; assert(!nus_input(epoch,challenge,input)); assert(!nus_subscribed());
    reset(); select(); event(pin); challenge=lifecycle.view.challenge; epoch=lifecycle.view.epoch;
    assert(nus_input(epoch,challenge,input)); pump(); assert(injected==1 && !lifecycle.view.challenge);
    auto* old_arg=radio_arg;
    nus_cancel(); assert(!nus_scan()); pump(); emit_disconnect(); assert(nus_idle());
    assert(nus_scan()); pump();
    ble_gap_event late{}; late.type=BLE_GAP_EVENT_ENC_CHANGE; late.enc_change.conn_handle=7;
    gap(&late,old_arg); assert(lifecycle.view.phase==UltraPhase::Scanning);
    // Bounded candidate lists, expiry, fresh scan epochs and strict reply parser.
    UltraLifecycle policy; policy.scan(now); UltraPeer p{};
    for(unsigned i=0;i<7;++i) { p.address[0]=i; assert(policy.found(policy.view.epoch,p)==(i<6)); }
    policy.view.phase=UltraPhase::Selecting;
    assert(!policy.select(policy.view.scan_epoch,0,policy.view.expires+1));
    UltraReply parser; parser.begin(1000); auto response=frame(1000,{2,0});
    for(auto b:response) assert(parser.feed(&b,1));
    assert(parser.take(out)==OK && out==response);
    parser.begin(1000); auto duplicate = response; response.insert(response.end(),duplicate.begin(),duplicate.end());
    assert(!parser.feed(response.data(),response.size()));
    assert(connects > 0 && scans > 0 && terminates > 0);
    std::puts("Production NUS callbacks passed: selection/GATT/security/PIN/fragmentation/overflow/stale epoch/no replay.");
}
