#include "runtime.hpp"
#include "nus.hpp"
#include "ble_policy.hpp"
extern "C" {
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
#include <host/ble_store.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
void ble_store_config_init(void);
}
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <cstring>

namespace qb {
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= 2 && CONFIG_BT_NIMBLE_MAX_BONDS >= 2,
              "Desktop peripheral plus selected Ultra require two links and scoped bonds");
#if CONFIG_BT_NIMBLE_SM_LEGACY
#error "Bridge requires Secure Connections, not legacy pairing"
#endif
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x66,0x34,0x09,0x26,0x5a,0xf3,0x12,0xa1,0x7c,0x56,0xc6,0x59,0x55,0x8a,0x87,0xb8);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0xa3,0xb3,0xa3,0x88,0x13,0xed,0x01,0x87,0xaa,0x50,0xc7,0xe3,0xeb,0xc3,0xa2,0x82);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x2e,0xf9,0x30,0xcf,0x8c,0x80,0xe3,0xb1,0x31,0x5f,0x55,0x53,0x77,0xa1,0xc9,0x37);
static std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
static std::atomic<bool> subscribed{false};
static uint16_t tx_handle;
static uint8_t address_type;
static uint8_t pairing_action;
static QueueHandle_t tx_queue;
static SemaphoreHandle_t indication_done;
static ble_npl_event disconnect_event;
static ble_npl_event pairing_event;
static std::atomic<bool> pairing_reply_scheduled{false}, pairing_reply_yes{false};
static std::atomic<uint64_t> pairing_reply_challenge{0};
static uint64_t pairing_epoch = 0;
static std::atomic<bool> disconnect_scheduled{false};
static std::atomic<uint64_t> connection_epoch{0}, disconnect_epoch{0};
static std::atomic<uint16_t> disconnect_handle{BLE_HS_CONN_HANDLE_NONE};
static std::mutex bond_mutex;
using RoleBond = ScopedBondTable<ble_addr_t>::Bond;
static ScopedBondTable<ble_addr_t> bond_scope;
static auto& role_bonds = bond_scope.roles;
static bool legacy_roles = false;
static std::atomic<bool> bond_roles_ambiguous{false}, desktop_reset_pending{false};
static QueueHandle_t bond_updates;
struct BondUpdate { uint8_t role; RoleBond bond; bool erase = false; };
static bool same_peer(const ble_addr_t& a, const ble_addr_t& b) {
    return a.type == b.type && !std::memcmp(a.val, b.val, 6);
}
bool ble_role_allowed(const ble_gap_conn_desc& desc, bool central, bool persist) {
    std::lock_guard<std::mutex> lock(bond_mutex);
    if (bond_roles_ambiguous || (!central && desktop_reset_pending) ||
        !bond_scope.allows(central, desc.peer_id_addr, same_peer)) return false;
    auto& own = role_bonds[central ? 1 : 0];
    if (persist && !own.valid) {
        RoleBond bond{true, desc.peer_id_addr};
        BondUpdate update{uint8_t(central ? 1 : 0), bond};
        if (xQueueSend(bond_updates, &update, 0) != pdTRUE) return false;
        own = bond;
    }
    return true;
}
static void persist_bond(const BondUpdate& update) {
    if (update.erase && ble_store_util_delete_peer(&update.bond.peer)) bond_roles_ambiguous = true;
    nvs_handle_t storage;
    if (nvs_open("qb_ble_roles", NVS_READWRITE, &storage) != ESP_OK) { bond_roles_ambiguous = true; return; }
    const char* key = update.role ? "ultra" : "desktop";
    const auto result = update.erase ? nvs_erase_key(storage, key) :
        nvs_set_blob(storage, key, &update.bond.peer, sizeof(update.bond.peer));
    if (result != ESP_OK || nvs_commit(storage) != ESP_OK) bond_roles_ambiguous = true;
    nvs_close(storage);
    if (update.erase) desktop_reset_pending = false;
}
static void bond_task(void*) {
    BondUpdate update{};
    for (;;) {
        xQueueReceive(bond_updates, &update, portMAX_DELAY);
        persist_bond(update);
    }
}
static bool authenticated(uint16_t handle) {
    ble_gap_conn_desc d{};
    return ble_gap_conn_find(handle, &d) == 0 && authenticated_link(d.sec_state.encrypted,
        d.sec_state.authenticated, d.sec_state.bonded, d.sec_state.key_size);
}
void ble_disconnect() {
    if (!disconnect_scheduled.exchange(true)) {
        disconnect_epoch = connection_epoch.load(); disconnect_handle = connection.load();
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &disconnect_event);
    }
}
static int access(uint16_t c, uint16_t, ble_gatt_access_ctxt* context, void*) {
    if (c != connection || !authenticated(c) || !link_authorized) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!subscribed) return BLE_ATT_ERR_UNLIKELY;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    uint16_t count = OS_MBUF_PKTLEN(context->om), copied = 0;
    if (!count || count > 512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t bytes[512]{};
    if (ble_hs_mbuf_to_flat(context->om, bytes, sizeof(bytes), &copied)) return BLE_ATT_ERR_UNLIKELY;
    receive(bytes, copied); memset(bytes, 0, sizeof(bytes)); return 0;
}
static ble_gatt_chr_def characteristics[3]{};
static ble_gatt_svc_def services[2]{};
static int gap_event(ble_gap_event*, void*);
static void advertise() {
    if (connection != BLE_HS_CONN_HANDLE_NONE) return;
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&service_uuid);
    fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    ble_hs_adv_fields scan{};
    const char* name = "Quadband CoreS3";
    scan.name = reinterpret_cast<const uint8_t*>(name); scan.name_len = strlen(name); scan.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&scan);
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(address_type, nullptr, BLE_HS_FOREVER, &params, gap_event, nullptr);
}
static void apply_pairing_reply(ble_npl_event*) {
    const bool yes = pairing_reply_yes.load() && esp_timer_get_time() <= pairing_until;
    const uint64_t expected = pairing_reply_challenge.load();
    if (!pairing_reply_matches(yes, expected, pairing_challenge.load())) {
        pairing_reply_scheduled = false;
        return;
    }
    pairing_reply_scheduled = false;
    if (!pairing_pending.exchange(false)) return;
    ble_sm_io io{}; io.action = pairing_action;
    if (pairing_action == BLE_SM_IOACT_NUMCMP) io.numcmp_accept = yes;
    else if (pairing_action == BLE_SM_IOACT_DISP) {
        if (!yes) { ble_disconnect(); return; }
        io.passkey = pairing_code;
    } else { ble_disconnect(); return; }
    if (ble_sm_inject_io(connection, &io)) ble_disconnect();
}
void ble_pair_approve(bool yes, uint64_t expected_challenge) {
    if (!pairing_pending || pairing_reply_scheduled.exchange(true)) return;
    pairing_reply_challenge = expected_challenge; pairing_reply_yes = yes;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &pairing_event);
}
void ble_clear_bonds() {
    // Local reset is scoped to the desktop role; the Ultra bond is never removed.
    ble_disconnect();
    std::lock_guard<std::mutex> lock(bond_mutex);
    if (!role_bonds[0].valid) return;
    BondUpdate update{0, role_bonds[0], true};
    if (xQueueSend(bond_updates, &update, 0) == pdTRUE) { role_bonds[0] = {}; desktop_reset_pending = true; }
}
static int gap_event(ble_gap_event* e, void*) {
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            ble_gap_conn_desc desc{};
            if (connection != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(e->connect.conn_handle, &desc) ||
                desc.role || !ble_role_allowed(desc, false)) {
                ble_gap_terminate(e->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM); break;
            }
            ++connection_epoch; connection = e->connect.conn_handle;
            ble_gap_security_initiate(connection);
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        if (e->disconnect.conn.conn_handle != connection) break;
        ++connection_epoch; connection = BLE_HS_CONN_HANDLE_NONE; subscribed = false; link_authorized = false;
        pairing_pending = false; pairing_code = 0; pairing_challenge = 0; disconnected();
        xSemaphoreGive(indication_done);
        advertise(); break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (e->enc_change.conn_handle != connection) break;
        link_authorized = e->enc_change.status == 0 && authenticated(connection);
        if (link_authorized) {
            ble_gap_conn_desc desc{};
            link_authorized = ble_gap_conn_find(connection, &desc) == 0 && ble_role_allowed(desc, false, true);
        }
        if (!link_authorized) ble_disconnect();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.conn_handle != connection) break;
        if (e->subscribe.attr_handle == tx_handle) subscribed = e->subscribe.cur_indicate;
        if (!subscribed && link_authorized) ble_disconnect();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (e->passkey.conn_handle != connection) break;
        if (esp_timer_get_time() > pairing_until) { ble_disconnect(); break; }
        pairing_action = e->passkey.params.action;
        if (pairing_action != BLE_SM_IOACT_NUMCMP && pairing_action != BLE_SM_IOACT_DISP) {
            ble_disconnect(); break;
        }
        pairing_code = pairing_action == BLE_SM_IOACT_NUMCMP ? e->passkey.params.numcmp : esp_random() % 1000000;
        pairing_challenge = (++pairing_epoch << 20) | pairing_code.load();
        pairing_pending = true; break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (e->notify_tx.conn_handle != connection) break;
        if (e->notify_tx.indication && e->notify_tx.status != 0) {
            if (e->notify_tx.status != BLE_HS_EDONE) ble_disconnect();
            xSemaphoreGive(indication_done);
        }
        break;
    default: break;
    }
    return 0;
}
void ble_send(const Frame& f) {
    if (!link_authorized || !subscribed) return;
    auto* frame = new Frame(f);
    if (xQueueSend(tx_queue, &frame, 0) != pdTRUE) { delete frame; ble_disconnect(); }
}
static void tx_task(void*) {
    for (;;) {
        Frame* f = nullptr;
        xQueueReceive(tx_queue, &f, portMAX_DELAY);
        auto bytes = f->encode(); uint64_t nonce = f->session; delete f;
        for (size_t offset = 0; offset < bytes.size();) {
            auto c = connection.load();
            if (!authenticated(c)) break;
            {
                std::lock_guard<std::mutex> guard(state_mutex);
                if (!link_authorized || !subscribed || session.nonce != nonce) break;
            }
            size_t n = std::min<size_t>(20, bytes.size() - offset);
            xSemaphoreTake(indication_done, 0);
            auto* om = ble_hs_mbuf_from_flat(bytes.data() + offset, n);
            if (!om || ble_gatts_indicate_custom(c, tx_handle, om) != 0 ||
                xSemaphoreTake(indication_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
                ble_disconnect(); break;
            }
            offset += n;
        }
        std::fill(bytes.begin(), bytes.end(), 0);
    }
}
void ble_init() {
    tx_queue = xQueueCreate(4, sizeof(Frame*)); indication_done = xSemaphoreCreateBinary();
    bond_updates = xQueueCreate(4, sizeof(BondUpdate));
    assert(tx_queue && indication_done && bond_updates);
    ESP_ERROR_CHECK(nimble_port_init());
    nus_init();
    ble_npl_event_init(&pairing_event, apply_pairing_reply, nullptr);
    ble_npl_event_init(&disconnect_event, [](ble_npl_event*) {
        disconnect_scheduled = false;
        auto c = connection.load();
        if (c != BLE_HS_CONN_HANDLE_NONE && c == disconnect_handle && connection_epoch == disconnect_epoch)
            ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    }, nullptr);
    ble_svc_gap_init(); ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Quadband CoreS3");
    characteristics[0].uuid = &rx_uuid.u; characteristics[0].access_cb = access;
    characteristics[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
    characteristics[0].min_key_size = 16;
    characteristics[1].uuid = &tx_uuid.u; characteristics[1].access_cb = access;
    characteristics[1].flags = BLE_GATT_CHR_F_INDICATE |
        BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN;
    characteristics[1].min_key_size = 16; characteristics[1].val_handle = &tx_handle;
    services[0].type = BLE_GATT_SVC_TYPE_PRIMARY; services[0].uuid = &service_uuid.u;
    services[0].characteristics = characteristics;
    assert(ble_gatts_count_cfg(services) == 0); assert(ble_gatts_add_svcs(services) == 0);
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_mitm = 1; ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = [](ble_store_status_event*, void*) { return BLE_HS_ENOMEM; };
    ble_store_config_init();
    nvs_handle_t storage;
    if (nvs_open("qb_ble_roles", NVS_READWRITE, &storage) == ESP_OK) {
        uint8_t schema = 0;
        legacy_roles = nvs_get_u8(storage, "schema", &schema) == ESP_ERR_NVS_NOT_FOUND;
        for (unsigned i = 0; i < 2; ++i) {
            size_t size = sizeof(ble_addr_t);
            role_bonds[i].valid = nvs_get_blob(storage, i ? "ultra" : "desktop", &role_bonds[i].peer, &size) == ESP_OK &&
                size == sizeof(ble_addr_t);
        }
        // A power loss after first central pairing must never relabel its bond as a legacy desktop.
        if (nvs_set_u8(storage, "schema", 1) != ESP_OK || nvs_commit(storage) != ESP_OK)
            bond_roles_ambiguous = true;
        nvs_close(storage);
    } else bond_roles_ambiguous = true;
    ble_hs_cfg.sync_cb = [] {
        assert(ble_hs_id_infer_auto(0, &address_type) == 0);
        // Reconcile every stored identity; unknown bonds are not relabeled or evicted.
        ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS]{}; int count = 0;
        const int status = ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS);
        const bool had_desktop = role_bonds[0].valid;
        if (status) bond_roles_ambiguous = true;
        else {
            bond_scope.reconcile(peers, size_t(count), legacy_roles, same_peer);
            if (!had_desktop && role_bonds[0].valid) {
                BondUpdate update{0, role_bonds[0]};
                if (xQueueSend(bond_updates, &update, 0) != pdTRUE) bond_roles_ambiguous = true;
            }
        }
        advertise();
    };
    xTaskCreate(bond_task, "ble-bonds", 3072, nullptr, 3, nullptr);
    xTaskCreate(tx_task, "ble-tx", 4096, nullptr, 4, nullptr);
    nimble_port_freertos_init([](void*) { nimble_port_run(); nimble_port_freertos_deinit(); });
}
}
