#pragma once
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef union { ble_uuid_t u; ble_uuid128_t u128; } ble_uuid_any_t;
#define BLE_UUID128_INIT(...) {{128}, {__VA_ARGS__}}
typedef struct { uint8_t type, val[6]; } ble_addr_t;
struct ble_gap_sec_state { unsigned encrypted, authenticated, bonded, key_size; };
struct ble_gap_conn_desc { struct ble_gap_sec_state sec_state; ble_addr_t peer_id_addr, peer_ota_addr; uint16_t conn_handle; uint8_t role; };
struct os_mbuf { uint16_t size; uint8_t bytes[247]; };
#define OS_MBUF_PKTLEN(om) ((om)->size)
struct ble_gatt_error { int status; };
struct ble_gatt_attr { uint16_t handle; };
struct ble_gatt_svc { uint16_t start_handle, end_handle; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_hs_adv_fields { uint8_t num_uuids128; ble_uuid128_t* uuids128; uint8_t flags, uuids128_is_complete; const uint8_t* name; size_t name_len; uint8_t name_is_complete; };
struct ble_gap_disc_params { uint8_t passive, filter_duplicates; };
struct ble_gap_event {
    int type;
    struct { uint16_t conn_handle, attr_handle; bool cur_indicate; } subscribe;
    struct { uint16_t conn_handle; bool indication; int status; } notify_tx;
    struct { int status; uint16_t conn_handle; } connect;
    struct { struct ble_gap_conn_desc conn; } disconnect;
    struct { uint16_t conn_handle; int status; } enc_change;
    struct { uint16_t conn_handle; struct { uint8_t action; uint32_t numcmp; } params; } passkey;
    struct { uint16_t conn_handle, attr_handle; struct os_mbuf* om; } notify_rx;
    struct { ble_addr_t addr; uint8_t* data; uint8_t length_data; } disc;
};
enum { BLE_GAP_EVENT_DISC = 1, BLE_GAP_EVENT_DISC_COMPLETE, BLE_GAP_EVENT_CONNECT,
    BLE_GAP_EVENT_PASSKEY_ACTION, BLE_GAP_EVENT_ENC_CHANGE, BLE_GAP_EVENT_NOTIFY_RX,
    BLE_GAP_EVENT_REPEAT_PAIRING, BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_SUBSCRIBE, BLE_GAP_EVENT_NOTIFY_TX };
enum { BLE_HS_EDONE = 14, BLE_HS_CONN_HANDLE_NONE = 65535, BLE_ERR_REM_USER_CONN_TERM = 19,
    BLE_GATT_CHR_PROP_WRITE = 8, BLE_GATT_CHR_PROP_NOTIFY = 16, BLE_GAP_REPEAT_PAIRING_IGNORE = 2 };
typedef int (*ble_gap_event_fn)(struct ble_gap_event*, void*);
typedef int (*ble_gatt_attr_fn)(uint16_t, const struct ble_gatt_error*, struct ble_gatt_attr*, void*);
typedef int (*ble_gatt_svc_fn)(uint16_t, const struct ble_gatt_error*, const struct ble_gatt_svc*, void*);
typedef int (*ble_gatt_chr_fn)(uint16_t, const struct ble_gatt_error*, const struct ble_gatt_chr*, void*);
typedef int (*ble_gatt_dsc_fn)(uint16_t, const struct ble_gatt_error*, uint16_t, const struct ble_gatt_dsc*, void*);
int ble_gap_terminate(uint16_t, uint8_t);
int ble_gap_conn_find(uint16_t, struct ble_gap_conn_desc*);
int ble_gap_security_initiate(uint16_t);
int ble_hs_id_infer_auto(int, uint8_t*);
int ble_gap_disc(uint8_t, int32_t, const struct ble_gap_disc_params*, ble_gap_event_fn, void*);
int ble_gap_disc_cancel(void);
int ble_gap_conn_cancel(void);
int ble_gap_conn_active(void);
int ble_gap_disc_active(void);
int ble_gap_connect(uint8_t, const ble_addr_t*, int32_t, const void*, ble_gap_event_fn, void*);
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields*, const uint8_t*, uint8_t);
int ble_uuid_cmp(const ble_uuid_t*, const ble_uuid_t*);
uint16_t ble_uuid_u16(const ble_uuid_t*);
int ble_gattc_disc_svc_by_uuid(uint16_t, const ble_uuid_t*, ble_gatt_svc_fn, void*);
int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn, void*);
int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn, void*);
int ble_gattc_write_flat(uint16_t, uint16_t, const void*, uint16_t, ble_gatt_attr_fn, void*);
int ble_hs_mbuf_to_flat(const struct os_mbuf*, void*, uint16_t, uint16_t*);
struct ble_gatt_access_ctxt { int op; struct os_mbuf* om; };
typedef int (*access_fn)(uint16_t,uint16_t,struct ble_gatt_access_ctxt*,void*);
struct ble_gatt_chr_def { const ble_uuid_t* uuid; access_fn access_cb; unsigned flags; uint8_t min_key_size; uint16_t* val_handle; };
struct ble_gatt_svc_def { int type; const ble_uuid_t* uuid; struct ble_gatt_chr_def* characteristics; };
struct ble_gap_adv_params { int conn_mode, disc_mode; };
struct ble_store_status_event;
struct ble_hs_cfg_mock { int sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_our_key_dist,sm_their_key_dist;
    int (*store_status_cb)(struct ble_store_status_event*,void*); void (*sync_cb)(void); };
extern struct ble_hs_cfg_mock ble_hs_cfg;
enum { BLE_ATT_ERR_INSUFFICIENT_AUTHEN=5, BLE_ATT_ERR_UNLIKELY=14, BLE_ATT_ERR_READ_NOT_PERMITTED=2,
    BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN=13, BLE_GATT_ACCESS_OP_WRITE_CHR=2,
    BLE_HS_ADV_F_DISC_GEN=2, BLE_HS_ADV_F_BREDR_UNSUP=4, BLE_GAP_CONN_MODE_UND=2,
    BLE_GAP_DISC_MODE_GEN=2, BLE_HS_FOREVER=2147483647, BLE_GATT_SVC_TYPE_PRIMARY=1,
    BLE_GATT_CHR_F_WRITE=1, BLE_GATT_CHR_F_WRITE_ENC=2, BLE_GATT_CHR_F_WRITE_AUTHEN=4,
    BLE_GATT_CHR_F_INDICATE=8, BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC=16,
    BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN=32 };
#define BLE_HS_ENOMEM 6
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields*);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields*);
int ble_gap_adv_start(uint8_t,const void*,int32_t,const struct ble_gap_adv_params*,ble_gap_event_fn,void*);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def*);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def*);
struct os_mbuf* ble_hs_mbuf_from_flat(const void*,uint16_t);
int ble_gatts_indicate_custom(uint16_t,uint16_t,struct os_mbuf*);
#ifdef __cplusplus
}
#endif
