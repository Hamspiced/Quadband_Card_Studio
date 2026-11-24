#include "nus.hpp"
#include "runtime.hpp"
#include "ble_policy.hpp"
extern "C" {
#include <nimble/nimble_port.h>
#include <host/ble_hs.h>
#include <host/ble_sm.h>
}
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

namespace qb {
namespace {
const ble_uuid128_t service = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
const ble_uuid128_t rx = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
const ble_uuid128_t tx = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);
UltraLifecycle lifecycle;
std::mutex mutex;
uint16_t connection = BLE_HS_CONN_HANDLE_NONE, first = 0, last = 0, write_handle = 0,
    notify_handle = 0, notify_end = 0, cccd = 0;
uint64_t challenge_serial = 0, connection_epoch = 0;
uint8_t own_address_type = 0;
bool receiving = false, write_pending = false;
bool cancel_pending = false;
int write_status = 0;
struct Fragment { uint64_t epoch; uint16_t size; uint8_t data[247]; };
QueueHandle_t fragments;
enum class Action { Scan, Select, Cancel, Input, Write };
struct Command {
    Action action;
    uint64_t epoch, challenge = 0;
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    uint32_t passkey = 0;
    uint8_t size = 0, data[20]{};
};
QueueHandle_t commands;
ble_npl_event command_event;
void* token(uint64_t epoch) { return reinterpret_cast<void*>(uintptr_t(epoch)); }
uint64_t epoch_of(void* arg) { return uintptr_t(arg); }
void message(const char* text) { std::snprintf(lifecycle.view.message, sizeof(lifecycle.view.message), "%s", text); }
bool valid(uint16_t handle, void* arg) {
    return lifecycle.current(epoch_of(arg)) && connection == handle && handle != BLE_HS_CONN_HANDLE_NONE;
}
void fail(const char* text) {
    const auto handle = connection;
    lifecycle.reset(UltraPhase::Failed); receiving = false; write_pending = false;
    message(text);
    if (handle != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
}
bool enqueue(const Command& command) {
    if (xQueueSend(commands, &command, 0) != pdTRUE) return false;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event); return true;
}
int write_complete(uint16_t handle, const ble_gatt_error* error, ble_gatt_attr*, void* arg) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!valid(handle, arg)) return 0;
    write_status = error->status; write_pending = false;
    if (error->status) fail("GATT write failed. Reconnect explicitly; command not replayed.");
    return 0;
}
int subscribed(uint16_t handle, const ble_gatt_error* error, ble_gatt_attr*, void* arg) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!valid(handle, arg)) return 0;
    if (error->status) fail("Notification subscription failed.");
    else { lifecycle.view.phase = UltraPhase::Validating; message("Validating reader identity and capabilities."); }
    return 0;
}
int descriptor(uint16_t handle, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc, void* arg) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!valid(handle, arg)) return 0;
    if (!error->status && dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
        if (cccd) { fail("Ambiguous notification descriptor."); return 0; }
        cccd = dsc->handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!cccd) { fail("Missing notification descriptor."); return 0; }
        const uint8_t enable[]{1, 0};
        lifecycle.view.phase = UltraPhase::Subscribing;
        if (ble_gattc_write_flat(handle, cccd, enable, sizeof(enable), subscribed, arg))
            fail("Unable to subscribe to reader.");
    } else if (error->status) fail("Descriptor discovery failed.");
    return 0;
}
int characteristic(uint16_t handle, const ble_gatt_error* error, const ble_gatt_chr* chr, void* arg) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!valid(handle, arg)) return 0;
    if (!error->status && chr) {
        if (notify_handle && !notify_end && chr->def_handle > notify_handle) notify_end = chr->def_handle - 1;
        if (!ble_uuid_cmp(&chr->uuid.u, &rx.u)) {
            if (write_handle || !(chr->properties & BLE_GATT_CHR_PROP_WRITE)) {
                fail("Reader RX must support acknowledged writes."); return 0;
            }
            write_handle = chr->val_handle;
        }
        if (!ble_uuid_cmp(&chr->uuid.u, &tx.u)) {
            if (notify_handle || !(chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
                fail("Reader TX notifications unavailable."); return 0;
            }
            notify_handle = chr->val_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (!write_handle || !notify_handle) { fail("Incomplete Nordic UART service."); return 0; }
        if (!notify_end) notify_end = last;
        if (notify_handle >= notify_end ||
            ble_gattc_disc_all_dscs(handle, notify_handle, notify_end, descriptor, arg))
            fail("Unable to discover notification descriptor.");
    } else if (error->status) fail("Characteristic discovery failed.");
    return 0;
}
int discovered(uint16_t handle, const ble_gatt_error* error, const ble_gatt_svc* svc, void* arg) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!valid(handle, arg)) return 0;
    if (!error->status && svc) {
        if (first) { fail("Ambiguous Nordic UART services."); return 0; }
        first = svc->start_handle; last = svc->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!first || ble_gattc_disc_all_chrs(handle, first, last, characteristic, arg))
            fail("Nordic UART service unavailable.");
    } else if (error->status) fail("Service discovery failed.");
    return 0;
}
int gap(ble_gap_event* event, void* arg) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!lifecycle.current(epoch_of(arg))) {
        if (event->type == BLE_GAP_EVENT_CONNECT && !event->connect.status)
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (event->type == BLE_GAP_EVENT_DISCONNECT && event->disconnect.conn.conn_handle == connection &&
            epoch_of(arg) == connection_epoch)
            connection = BLE_HS_CONN_HANDLE_NONE;
        return 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (lifecycle.view.phase != UltraPhase::Scanning) break;
        ble_hs_adv_fields fields{};
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data)) break;
        bool candidate = false;
        for (unsigned i = 0; i < fields.num_uuids128; ++i)
            if (!ble_uuid_cmp(&fields.uuids128[i].u, &service.u)) candidate = true;
        if (!candidate) break;
        UltraPeer peer; peer.type = event->disc.addr.type;
        std::copy_n(event->disc.addr.val, 6, peer.address.begin());
        std::snprintf(peer.label, sizeof(peer.label), "NUS %02X:%02X:%02X:%02X:%02X:%02X",
            peer.address[5], peer.address[4], peer.address[3], peer.address[2], peer.address[1], peer.address[0]);
        lifecycle.found(epoch_of(arg), peer); break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        lifecycle.view.phase = UltraPhase::Selecting;
        lifecycle.view.expires = esp_timer_get_time() + 30000000;
        message("Select a specific reader. Candidates are not trusted."); break;
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status) { fail("Connection failed. Select again explicitly."); break; }
        connection = event->connect.conn_handle; connection_epoch = epoch_of(arg);
        ble_gap_conn_desc desc{};
        if (ble_gap_conn_find(connection, &desc) || !desc.role ||
            desc.peer_ota_addr.type != lifecycle.view.selected.type ||
            std::memcmp(desc.peer_ota_addr.val, lifecycle.view.selected.address.data(), 6) ||
            !ble_role_allowed(desc, true) || ble_gap_security_initiate(connection))
            fail("Selected peer/security unavailable; existing bonds preserved.");
        break;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (!valid(event->passkey.conn_handle, arg)) break;
        if (event->passkey.params.action != BLE_SM_IOACT_INPUT) {
            fail("Reader requires unsupported pairing interaction; no downgrade."); break;
        }
        lifecycle.view.phase = UltraPhase::Pairing;
        lifecycle.view.challenge = ++challenge_serial;
        lifecycle.view.expires = esp_timer_get_time() + 60000000;
        message("Enter the six-digit reader passkey locally."); break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (!valid(event->enc_change.conn_handle, arg)) break;
        ble_gap_conn_desc desc{};
        if (event->enc_change.status || ble_gap_conn_find(connection, &desc) ||
            !authenticated_link(desc.sec_state.encrypted, desc.sec_state.authenticated,
                                desc.sec_state.bonded, desc.sec_state.key_size) || !ble_role_allowed(desc, true, true)) {
            fail("Secure authenticated bonded 128-bit link required."); break;
        }
        lifecycle.view.challenge = 0;
        lifecycle.view.phase = UltraPhase::Discovering;
        lifecycle.view.expires = esp_timer_get_time() + 15000000;
        if (ble_gattc_disc_svc_by_uuid(connection, &service.u, discovered, arg)) fail("Service discovery failed.");
        break;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (!valid(event->notify_rx.conn_handle, arg) || event->notify_rx.attr_handle != notify_handle) break;
        Fragment fragment{}; fragment.epoch = lifecycle.view.epoch;
        fragment.size = OS_MBUF_PKTLEN(event->notify_rx.om); uint16_t copied = 0;
        if (!receiving || !fragment.size || fragment.size > sizeof(fragment.data) ||
            ble_hs_mbuf_to_flat(event->notify_rx.om, fragment.data, sizeof(fragment.data), &copied) ||
            copied != fragment.size || xQueueSend(fragments, &fragment, 0) != pdTRUE)
            fail("Unexpected reader data or receive overflow; reconnect explicitly.");
        std::memset(fragment.data, 0, sizeof(fragment.data)); break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE;
        if (lifecycle.view.phase != UltraPhase::Failed) { lifecycle.reset(); message("Reader disconnected. No automatic reconnect."); }
        receiving = false; write_pending = false; break;
    default: break;
    }
    return 0;
}
void execute(ble_npl_event*) {
    Command command{};
    while (xQueueReceive(commands, &command, 0) == pdTRUE) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!lifecycle.current(command.epoch)) { std::memset(&command, 0, sizeof(command)); continue; }
        switch (command.action) {
        case Action::Scan: {
            if (connection != BLE_HS_CONN_HANDLE_NONE || ble_hs_id_infer_auto(0, &own_address_type)) { fail("Disconnect before scanning."); break; }
            ble_gap_disc_params params{}; params.passive = 1; params.filter_duplicates = 1;
            if (ble_gap_disc(own_address_type, 8000, &params, gap, token(command.epoch))) fail("Bluetooth scan unavailable.");
            break;
        }
        case Action::Select: {
            first = last = write_handle = notify_handle = notify_end = cccd = 0;
            ble_addr_t peer{}; peer.type = lifecycle.view.selected.type;
            std::copy_n(lifecycle.view.selected.address.begin(), 6, peer.val);
            if (ble_gap_connect(own_address_type, &peer, 15000, nullptr, gap, token(command.epoch)))
                fail("Unable to connect. No connection retry.");
            break;
        }
        case Action::Cancel:
            ble_gap_disc_cancel(); ble_gap_conn_cancel();
            if (connection != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
            cancel_pending = false;
            break;
        case Action::Input: {
            if (lifecycle.view.challenge != command.challenge || lifecycle.view.phase != UltraPhase::Pairing ||
                esp_timer_get_time() > lifecycle.view.expires) break;
            ble_sm_io io{}; io.action = BLE_SM_IOACT_INPUT; io.passkey = command.passkey;
            lifecycle.view.challenge = 0;
            lifecycle.view.phase = UltraPhase::Connecting;
            lifecycle.view.expires = esp_timer_get_time() + 15000000;
            if (ble_sm_inject_io(connection, &io)) fail("Passkey rejected; reconnect explicitly.");
            io.passkey = 0; break;
        }
        case Action::Write:
            if (connection != command.handle || !write_pending ||
                ble_gattc_write_flat(connection, write_handle, command.data, command.size,
                                     write_complete, token(command.epoch))) fail("Reader write failed; no replay.");
            break;
        }
        std::memset(&command, 0, sizeof(command));
    }
}
}
void nus_init() {
    fragments = xQueueCreate(12, sizeof(Fragment)); commands = xQueueCreate(4, sizeof(Command));
    assert(fragments && commands);
    ble_npl_event_init(&command_event, execute, nullptr);
}
void nus_snapshot(UltraView& view) {
    std::lock_guard<std::mutex> lock(mutex); view = lifecycle.view;
}
bool nus_scan() {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancel_pending || connection != BLE_HS_CONN_HANDLE_NONE || lifecycle.view.phase == UltraPhase::Scanning ||
        lifecycle.view.phase == UltraPhase::Connecting) return false;
    lifecycle.scan(esp_timer_get_time()); message("Scanning for eight seconds; no automatic selection.");
    if (enqueue({Action::Scan, lifecycle.view.epoch})) return true;
    fail("Scan request queue overflow."); return false;
}
bool nus_select(uint64_t scan, size_t index) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!lifecycle.select(scan, index, esp_timer_get_time())) return false;
    message("Connecting to selected reader.");
    if (enqueue({Action::Select, lifecycle.view.epoch})) return true;
    fail("Selection request queue overflow."); return false;
}
void nus_cancel() {
    std::lock_guard<std::mutex> lock(mutex);
    lifecycle.reset(); receiving = false; write_pending = false; message("Reader connection cancelled.");
    cancel_pending = true;
    if (!enqueue({Action::Cancel, lifecycle.view.epoch})) fail("Cancellation queue overflow.");
}
bool nus_idle() {
    std::lock_guard<std::mutex> lock(mutex);
    return !cancel_pending && connection == BLE_HS_CONN_HANDLE_NONE &&
        !ble_gap_conn_active() && !ble_gap_disc_active();
}
bool nus_input(uint64_t epoch, uint64_t challenge, const char* digits) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!lifecycle.input(epoch, challenge, digits, esp_timer_get_time())) return false;
    Command command{Action::Input, epoch}; command.challenge = challenge;
    for (unsigned i = 0; i < 6; ++i) command.passkey = command.passkey * 10 + unsigned(digits[i] - '0');
    bool ok = enqueue(command); command.passkey = 0;
    if (!ok) fail("Pairing request queue overflow; reconnect explicitly.");
    return ok;
}
bool nus_subscribed() {
    std::lock_guard<std::mutex> lock(mutex);
    if (lifecycle.view.expires && esp_timer_get_time() > lifecycle.view.expires &&
        lifecycle.view.phase != UltraPhase::Ready && lifecycle.view.phase != UltraPhase::Selecting &&
        lifecycle.view.phase != UltraPhase::Disconnected && lifecycle.view.phase != UltraPhase::Failed)
        fail("Reader connection/pairing expired; reconnect explicitly.");
    return lifecycle.view.phase == UltraPhase::Validating || lifecycle.view.phase == UltraPhase::Ready;
}
void nus_validated(bool ready) {
    std::lock_guard<std::mutex> lock(mutex);
    if (ready && lifecycle.view.phase == UltraPhase::Validating) {
        lifecycle.view.phase = UltraPhase::Ready; lifecycle.view.expires = 0; message("Validated Ultra ready over Bluetooth.");
    } else if (!ready) fail("Unsupported reader or protocol validation failed.");
}
Status nus_transaction(const Bytes& request, Bytes& response) {
    uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if ((lifecycle.view.phase != UltraPhase::Validating && lifecycle.view.phase != UltraPhase::Ready) || receiving)
            return UNAVAILABLE;
        epoch = lifecycle.view.epoch; receiving = true; xQueueReset(fragments);
    }
    UltraReply reply; reply.begin(unsigned(request[2]) << 8 | request[3]);
    const auto deadline = esp_timer_get_time() + int64_t(usb_timeout_ms(unsigned(request[2]) << 8 | request[3])) * 1000;
    Status result = TIMEOUT;
    for (size_t offset = 0; offset < request.size();) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!lifecycle.current(epoch) || connection == BLE_HS_CONN_HANDLE_NONE) { result = IO_ERROR; break; }
            Command command{Action::Write, epoch}; command.handle = connection;
            command.size = std::min(size_t(20), request.size() - offset);
            std::copy_n(request.data() + offset, command.size, command.data);
            write_pending = true; write_status = 0;
            if (!enqueue(command)) { fail("Transmit queue overflow; no replay."); result = IO_ERROR; break; }
            offset += command.size; std::memset(command.data, 0, sizeof(command.data));
        }
        for (;;) {
            bool pending;
            { std::lock_guard<std::mutex> lock(mutex); pending = write_pending && lifecycle.current(epoch); }
            if (!pending || esp_timer_get_time() >= deadline) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!lifecycle.current(epoch) || write_status || write_pending) { result = IO_ERROR; break; }
        }
        if (offset == request.size()) result = BUSY;
    }
    while (result == BUSY && esp_timer_get_time() < deadline) {
        { std::lock_guard<std::mutex> lock(mutex); if (!lifecycle.current(epoch)) { result = IO_ERROR; break; } }
        Fragment fragment{};
        if (xQueueReceive(fragments, &fragment, pdMS_TO_TICKS(10)) != pdTRUE) continue;
        if (fragment.epoch != epoch || !reply.feed(fragment.data, fragment.size)) result = IO_ERROR;
        else result = reply.take(response);
        std::memset(fragment.data, 0, sizeof(fragment.data));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        receiving = false;
        if (result == BUSY) result = TIMEOUT;
        if (result != OK || !lifecycle.current(epoch) || uxQueueMessagesWaiting(fragments)) {
            response.clear(); fail("Reader reply interrupted or malformed; no replay.");
            if (result == OK) result = IO_ERROR;
        }
    }
    return result;
}
}
