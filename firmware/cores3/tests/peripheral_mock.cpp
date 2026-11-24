#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <cstring>
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 2
#define CONFIG_BT_NIMBLE_MAX_BONDS 2
#define CONFIG_BT_NIMBLE_SM_LEGACY 0
#include "../main/ble.cpp"

namespace {
struct Queue { size_t capacity,width; std::deque<std::vector<uint8_t>> entries; };
std::vector<std::unique_ptr<Queue>> queues;
std::deque<ble_npl_event*> events;
std::map<uint16_t,ble_gap_conn_desc> peers;
std::map<std::string,std::vector<uint8_t>> storage;
std::vector<ble_addr_t> stored_bonds, deleted_bonds;
std::vector<uint16_t> terminated;
unsigned adverts=0, disconnects=0, received=0;
bool nvs_failure=false;
void pump() { while(!events.empty()){auto* e=events.front();events.pop_front();e->callback(e);} }
void persist() {
    qb::BondUpdate update{};
    while(xQueueReceive(qb::bond_updates,&update,0)==pdTRUE) qb::persist_bond(update);
}
void connect(uint16_t h) {
    ble_gap_event e{}; e.type=BLE_GAP_EVENT_CONNECT; e.connect.conn_handle=h; qb::gap_event(&e,nullptr);
}
void encrypt(uint16_t h,int status=0) {
    ble_gap_event e{}; e.type=BLE_GAP_EVENT_ENC_CHANGE; e.enc_change={h,status}; qb::gap_event(&e,nullptr);
}
void disconnect(uint16_t h) {
    ble_gap_event e{}; e.type=BLE_GAP_EVENT_DISCONNECT; e.disconnect.conn=peers[h]; qb::gap_event(&e,nullptr);
}
}
namespace qb {
std::mutex state_mutex;
Session session;
std::atomic<bool> link_authorized{false},pairing_pending{false};
std::atomic<uint32_t> pairing_code{0};
std::atomic<uint64_t> pairing_challenge{0};
std::atomic<int64_t> pairing_until{0};
void nus_init() {}
void receive(const uint8_t*,size_t) { ++received; }
void disconnected() { ++disconnects; }
}
QueueHandle_t xQueueCreate(size_t capacity,size_t width) {
    queues.emplace_back(std::make_unique<Queue>(Queue{capacity,width,{}}));return queues.back().get();
}
int xQueueSend(QueueHandle_t h,const void* p,unsigned) {
    auto& q=*static_cast<Queue*>(h);if(q.entries.size()==q.capacity)return 0;
    auto* bytes=static_cast<const uint8_t*>(p);q.entries.emplace_back(bytes,bytes+q.width);return pdTRUE;
}
int xQueueReceive(QueueHandle_t h,void* p,unsigned) {
    auto& q=*static_cast<Queue*>(h);if(q.entries.empty())return 0;
    std::memcpy(p,q.entries.front().data(),q.width);q.entries.pop_front();return pdTRUE;
}
int xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,void*) { return pdPASS; }
SemaphoreHandle_t xSemaphoreCreateBinary() { return reinterpret_cast<void*>(1); }
int xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t,unsigned) { return pdTRUE; }
uint32_t esp_random() { return 0; }
int64_t esp_timer_get_time() { return 1; }
int nvs_open(const char*,int,nvs_handle_t* h) { *h=1;return nvs_failure?2:ESP_OK; }
void nvs_close(nvs_handle_t) {}
int nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* size) {
    auto it=storage.find(key);if(it==storage.end())return ESP_ERR_NVS_NOT_FOUND;
    if(*size<it->second.size())return 2;
    *size=it->second.size();std::memcpy(out,it->second.data(),*size);return ESP_OK;
}
int nvs_set_blob(nvs_handle_t,const char* key,const void* data,size_t size) {
    auto* p=static_cast<const uint8_t*>(data);storage[key]={p,p+size};return ESP_OK;
}
int nvs_erase_key(nvs_handle_t,const char* key) { storage.erase(key);return ESP_OK; }
int nvs_get_u8(nvs_handle_t h,const char* key,uint8_t* out) { size_t size=1;return nvs_get_blob(h,key,out,&size); }
int nvs_set_u8(nvs_handle_t h,const char* key,uint8_t value) { return nvs_set_blob(h,key,&value,1); }
int nvs_commit(nvs_handle_t) { return ESP_OK; }
extern "C" {
ble_hs_cfg_mock ble_hs_cfg{};
void ble_npl_event_init(ble_npl_event* e,void (*cb)(ble_npl_event*),void*) { e->callback=cb; }
void* nimble_port_get_dflt_eventq() { return nullptr; }
void ble_npl_eventq_put(void*,ble_npl_event* e) { events.push_back(e); }
int nimble_port_init() { return 0; }
void nimble_port_run() {}
void nimble_port_freertos_init(void (*)(void*)) {}
void nimble_port_freertos_deinit() {}
void ble_svc_gap_init() {}
void ble_svc_gatt_init() {}
void ble_svc_gap_device_name_set(const char*) {}
int ble_gap_conn_find(uint16_t h,ble_gap_conn_desc* out) {
    auto it=peers.find(h);if(it==peers.end())return 1;*out=it->second;return 0;
}
int ble_gap_terminate(uint16_t h,uint8_t) { terminated.push_back(h);return 0; }
int ble_gap_security_initiate(uint16_t) { return 0; }
int ble_hs_id_infer_auto(int,uint8_t* out) { *out=0;return 0; }
int ble_gap_adv_set_fields(const ble_hs_adv_fields*) { return 0; }
int ble_gap_adv_rsp_set_fields(const ble_hs_adv_fields*) { return 0; }
int ble_gap_adv_start(uint8_t,const void*,int32_t,const ble_gap_adv_params*,ble_gap_event_fn,void*) { ++adverts;return 0; }
int ble_gatts_count_cfg(const ble_gatt_svc_def*) { return 0; }
int ble_gatts_add_svcs(const ble_gatt_svc_def*) { return 0; }
os_mbuf* ble_hs_mbuf_from_flat(const void*,uint16_t) { return nullptr; }
int ble_gatts_indicate_custom(uint16_t,uint16_t,os_mbuf*) { return 0; }
int ble_hs_mbuf_to_flat(const os_mbuf* om,void* out,uint16_t n,uint16_t* copied) {
    if(om->size>n)return 1;std::memcpy(out,om->bytes,om->size);*copied=om->size;return 0;
}
int ble_sm_inject_io(uint16_t,const ble_sm_io*) { return 0; }
void ble_store_config_init() {}
int ble_store_util_bonded_peers(ble_addr_t* out,int* count,int capacity) {
    assert(int(stored_bonds.size())<=capacity);*count=stored_bonds.size();std::copy(stored_bonds.begin(),stored_bonds.end(),out);return 0;
}
int ble_store_util_delete_peer(const ble_addr_t* peer) {
    deleted_bonds.push_back(*peer);return 0;
}
}
int main() {
    using namespace qb;
    for(uint16_t h : {7,8,9}) {
        peers[h]={};peers[h].conn_handle=h;peers[h].peer_id_addr.val[0]=h;
        peers[h].sec_state={1,1,1,16};peers[h].role=h==8;
    }
    stored_bonds.push_back(peers[7].peer_id_addr);
    ble_init();ble_hs_cfg.sync_cb();persist();
    assert(role_bonds[0].valid && role_bonds[0].peer.val[0]==7 && !role_bonds[1].valid);
    assert(ble_hs_cfg.sm_sc && ble_hs_cfg.sm_mitm && ble_hs_cfg.sm_bonding);
    assert(ble_hs_cfg.store_status_cb(nullptr,nullptr)==BLE_HS_ENOMEM && deleted_bonds.empty());
    connect(7);encrypt(7);persist();assert(connection==7 && link_authorized);
    assert(ble_role_allowed(peers[8],true,true));persist();
    assert(role_bonds[1].valid && role_bonds[1].peer.val[0]==8);
    assert(!ble_role_allowed(peers[9],false) && !ble_role_allowed(peers[7],true));
    connect(9);assert(terminated.back()==9 && connection==7 && link_authorized);
    connect(8);assert(terminated.back()==8 && connection==7);
    encrypt(8,1);disconnect(8);assert(connection==7 && link_authorized && !disconnects);
    ble_gap_event e{};e.type=BLE_GAP_EVENT_SUBSCRIBE;e.subscribe={8,tx_handle,false};gap_event(&e,nullptr);
    assert(connection==7 && link_authorized);
    e.subscribe={7,tx_handle,true};gap_event(&e,nullptr);
    os_mbuf om{};om.size=1;ble_gatt_access_ctxt ctx{BLE_GATT_ACCESS_OP_WRITE_CHR,&om};
    assert(access(8,0,&ctx,nullptr)==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    assert(access(7,0,&ctx,nullptr)==0 && received==1);
    for(unsigned property=0;property<4;++property) {
        auto original=peers[7].sec_state;
        if(property==0)peers[7].sec_state.encrypted=0;
        if(property==1)peers[7].sec_state.authenticated=0;
        if(property==2)peers[7].sec_state.bonded=0;
        if(property==3)peers[7].sec_state.key_size=12;
        assert(access(7,0,&ctx,nullptr)==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
        peers[7].sec_state=original;
    }
    ble_clear_bonds();assert(desktop_reset_pending && role_bonds[1].valid);persist();
    assert(deleted_bonds.size()==1 && deleted_bonds[0].val[0]==7 && storage.count("ultra"));
    // A delayed disconnect request must not terminate a new desktop reusing the role.
    const auto before=terminated.size();
    disconnect(7);connect(9);assert(connection==9);pump();
    assert(terminated.size()==before && role_bonds[1].valid);
    nvs_failure=true;
    BondUpdate update{0,{true,peers[9].peer_id_addr}};persist_bond(update);
    assert(bond_roles_ambiguous && !ble_role_allowed(peers[8],true));
    std::puts("Production peripheral callbacks passed: two-role isolation, one desktop, security, scoped bonds and stale disconnect.");
}
