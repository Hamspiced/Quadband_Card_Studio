#include "runtime.hpp"
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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <cstring>

namespace qb {
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS == 1 && CONFIG_BT_NIMBLE_MAX_BONDS == 1,
              "Bridge requires one connection and one bond");
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
static bool authenticated(uint16_t handle) {
    ble_gap_conn_desc d{};
    return ble_gap_conn_find(handle, &d) == 0 && d.sec_state.encrypted &&
        d.sec_state.authenticated && d.sec_state.bonded && d.sec_state.key_size == 16;
}
void ble_disconnect() {
    if (!disconnect_scheduled.exchange(true))
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &disconnect_event);
}
static int access(uint16_t c, uint16_t, ble_gatt_access_ctxt* context, void*) {
    if (!authenticated(c) || !link_authorized) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
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
void ble_clear_bonds() { ble_disconnect(); ble_store_clear(); }
static int gap_event(ble_gap_event* e, void*) {
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            connection = e->connect.conn_handle;
            ble_gap_security_initiate(connection);
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        connection = BLE_HS_CONN_HANDLE_NONE; subscribed = false; link_authorized = false;
        pairing_pending = false; disconnected();
        xSemaphoreGive(indication_done);
        advertise(); break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        link_authorized = e->enc_change.status == 0 && authenticated(connection);
        if (!link_authorized) ble_disconnect();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == tx_handle) subscribed = e->subscribe.cur_indicate;
        if (!subscribed && link_authorized) ble_disconnect();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
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
    ESP_ERROR_CHECK(nimble_port_init());
    ble_npl_event_init(&pairing_event, apply_pairing_reply, nullptr);
    ble_npl_event_init(&disconnect_event, [](ble_npl_event*) {
        disconnect_scheduled = false;
        auto c = connection.load();
        if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
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
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
    ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_mitm = 1; ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    ble_hs_cfg.sync_cb = [] { assert(ble_hs_id_infer_auto(0, &address_type) == 0); advertise(); };
    xTaskCreate(tx_task, "ble-tx", 4096, nullptr, 4, nullptr);
    nimble_port_freertos_init([](void*) { nimble_port_run(); nimble_port_freertos_deinit(); });
}
}
