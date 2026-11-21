#include "runtime.hpp"
#include "local_runtime.hpp"
#include <M5Unified.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <cstring>
#include <new>

namespace qb {
std::mutex state_mutex;
std::array<Slot, 3> slots;
Session session;
std::atomic<bool> stop_operation{false}, link_authorized{false}, pairing_pending{false};
std::atomic<bool> usb_host_approved{false};
std::atomic<UsbHostState> usb_host_state{UsbHostState::Off};
std::atomic<uint32_t> pairing_code{0};
std::atomic<uint64_t> pairing_challenge{0};
std::atomic<int64_t> pairing_until{0};
static Decoder decoder;
static QueueHandle_t work_queue;
static std::atomic<bool> release_requested{false};
static std::atomic<bool> worker_running{false};
static std::atomic<unsigned> last_status{0};
static ModeBroker modes;
static std::atomic<bool> refresh_requested{false};
static std::atomic<uint64_t> local_epoch{1};
static std::atomic<unsigned> local_pending{0};
static std::atomic<bool> local_stop_requested{false}, local_exit_requested{false};
static std::atomic<bool> confirm_mode{false}, confirm_usb{false}, confirm_bonds{false};
static WorkMode confirm_target = WorkMode::Home;
static bool light_theme = false;
static std::atomic<bool> release_unverified{false};
static std::string ui_message = "Choose Connected or Standalone.";
static std::string client_nonce(16, '0'), device_id;
struct Work {
    Frame frame;
    uint64_t epoch = 0;
    bool local_job = false;
    local::Intent intent;
};
bool local_operation_allowed(uint64_t epoch) {
    std::lock_guard<std::mutex> guard(state_mutex);
    return modes.local_allowed() && !release_unverified && local_epoch == epoch;
}
bool remote_event_allowed(uint64_t nonce, uint32_t generation) {
    std::lock_guard<std::mutex> guard(state_mutex);
    return modes.remote_allowed() && nonce && session.nonce == nonce && slots[1].generation == generation;
}
std::string hex(const uint8_t* p, size_t n) {
    const char* digits = "0123456789abcdef"; std::string s;
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}
std::string capabilities() {
    std::string s = "{\"protocol\":1,\"deviceId\":\"" + device_id +
        "\",\"name\":\"Quadband CoreS3\",\"firmware\":\"0.4.0\",\"clientNonce\":\"" + client_nonce +
        "\",\"mode\":\"" + mode_name(modes.mode()) + "\",\"owner\":\"" + mode_owner(modes.mode()) +
        "\",\"modePending\":" + (modes.pending() ? "true" : "false") + ",\"peripherals\":[";
    for (unsigned i = 0; i < 3; ++i) {
        const auto& p = slots[i];
        if (i) s += ",";
        s += "{\"channel\":" + std::to_string(i + 1) + ",\"generation\":" + std::to_string(p.generation) +
            ",\"present\":" + (p.present ? "true" : "false") + ",\"kind\":\"" + p.kind + "\",\"capabilities\":[";
        for (size_t j = 0; j < p.capabilities.size(); ++j) { if (j) s += ","; s += "\"" + p.capabilities[j] + "\""; }
        s += "]}";
    }
    return s + "]}";
}
static void publish_capabilities_locked() {
    if (!session.nonce) return;
    Frame event; event.kind = 2; event.session = session.nonce; event.op = 128;
    auto j = capabilities(); event.payload.assign(j.begin(), j.end()); ble_send(event);
}
void peripheral_changed(unsigned ch, bool present, const std::string& kind, const std::vector<std::string>& caps) {
    std::lock_guard<std::mutex> guard(state_mutex);
    auto& s = slots[ch - 1];
    if (s.present == present && s.kind == kind && s.capabilities == caps) return;
    ++s.generation; if (!s.generation) ++s.generation;
    s.present = present; s.kind = kind; s.capabilities = caps;
    publish_capabilities_locked();
}
void disconnected() {
    std::lock_guard<std::mutex> guard(state_mutex);
    if (modes.mode() != WorkMode::Standalone) { stop_operation = true; release_requested = true; }
    session.clear(); decoder.reset();
    Work* work;
    while (xQueueReceive(work_queue, &work, 0) == pdTRUE) {
        if (work->local_job) { xQueueSendToFront(work_queue, &work, 0); break; }
        std::fill(work->frame.payload.begin(), work->frame.payload.end(), 0); delete work;
    }
}
static void request(const Frame& f) {
    std::lock_guard<std::mutex> guard(state_mutex);
    Frame response = f; response.kind = 1; response.payload.clear();
    response.status = modes.admit(session, f, link_authorized, slots);
    if (response.status == BUSY && f.channel && !modes.remote_allowed()) {
        const char* text = "Select Connected mode on the CoreS3 to use readers.";
        response.payload.assign(text, text + std::strlen(text));
    }
    if (response.status == OK && (worker_running || release_requested) && (!f.channel ? f.op == 1 : true)) {
        if (session.pending == f.id) session.pending = 0;
        response.status = BUSY;
    }
    if (response.status == OK && !f.channel) {
        switch (f.op) {
        case 1:
            session.nonce = uint64_t(esp_random()) << 32 | esp_random();
            if (!session.nonce) session.nonce = 1;
            ++session.epoch; client_nonce = hex(f.payload.data(), 8);
            response.session = session.nonce;
            [[fallthrough]];
        case 2: {
            if (f.op == 2 && !f.payload.empty()) { response.status = INVALID_ARGUMENT; break; }
            auto j = capabilities(); response.payload.assign(j.begin(), j.end()); break;
        }
        case 3:
            if (f.payload.size() > 64) response.status = INVALID_ARGUMENT;
            else response.payload = f.payload;
            break;
        case 4:
            if (f.payload.size() != 4) response.status = INVALID_ARGUMENT;
            else if (!le32(f.payload.data())) response.status = INVALID_ARGUMENT;
            else if (modes.cancel_remote(session, le32(f.payload.data()))) stop_operation = true;
            break;
        default: response.status = UNSUPPORTED; break;
        }
    } else if (response.status == OK) {
        auto* work = new Work{f, session.epoch}; stop_operation = false;
        if (xQueueSend(work_queue, &work, 0) == pdTRUE) return;
        std::fill(work->frame.payload.begin(), work->frame.payload.end(), 0); delete work;
        session.pending = 0; response.status = BUSY;
    }
    ble_send(response);
}
void receive(const uint8_t* p, size_t n) {
    if (!decoder.feed(p, n, request)) ble_disconnect();
}
static void request_mode(WorkMode target, bool confirmed = false) {
    std::lock_guard<std::mutex> guard(state_mutex);
    if (release_unverified) return;
    auto result = modes.request(target, worker_running || session.pending || local_pending || local_runtime_active(),
                                link_authorized, confirmed);
    if (result == ModeRequest::Confirm) {
        confirm_mode = true; confirm_target = target;
    } else if (result == ModeRequest::Queued) {
        confirm_mode = false; stop_operation = true;
        ++local_epoch;
        ui_message = "Releasing readers before switching modes.";
        publish_capabilities_locked();
    } else if (result == ModeRequest::Busy) ui_message = "Wait for the mode change to finish.";
}
static void finish_idle_transition() {
    bool release = release_requested.exchange(false);
    {
        std::lock_guard<std::mutex> guard(state_mutex);
        release = release || (modes.pending() && !session.pending && !local_pending);
    }
    if (!release) return;
    const auto epoch = local_epoch.load();
    Status released = local_runtime_cancel(epoch, true);
    std::lock_guard<std::mutex> guard(state_mutex);
    if (released != OK) {
        release_unverified = true;
        confirm_mode = false; confirm_usb = false; confirm_bonds = false;
        modes.release_failed();
        for (auto& slot : slots) { ++slot.generation; if (!slot.generation) ++slot.generation; }
        last_status = released;
        ui_message = "Reader release unverified. Check connections and restart Core.";
        publish_capabilities_locked();
        return;
    }
    if (modes.complete(!worker_running && !session.pending && !local_pending)) {
        for (auto& slot : slots) { ++slot.generation; if (!slot.generation) ++slot.generation; }
        stop_operation = false;
        ui_message = last_status == WRITE_UNVERIFIED ? "Write outcome unverified. Read target before retrying." :
            modes.mode() == WorkMode::Connected ? "Desktop reader control enabled." :
            modes.mode() == WorkMode::Standalone ? "Local reader tools ready." :
            "Readers released. Choose an operating mode.";
        publish_capabilities_locked();
    }
}
static void save_theme() {
    nvs_handle_t prefs;
    if (nvs_open("qbui", NVS_READWRITE, &prefs) == ESP_OK) {
        nvs_set_u8(prefs, "light", light_theme ? 1 : 0); nvs_commit(prefs); nvs_close(prefs);
    }
}
static void latch_release_failure(Status status) {
    std::lock_guard<std::mutex> guard(state_mutex);
    release_unverified = true; modes.release_failed(); ++local_epoch;
    confirm_mode = false; confirm_usb = false; confirm_bonds = false;
    last_status = status;
    for (auto& slot : slots) { ++slot.generation; if (!slot.generation) ++slot.generation; }
    ui_message = "Reader release unverified. Check connections and restart Core.";
    publish_capabilities_locked();
}
static void submit_local(const local::Intent& intent) {
    std::lock_guard<std::mutex> guard(state_mutex);
    if (!modes.local_allowed() || release_unverified) return;
    if (intent.command == local::Command::Stop || intent.command == local::Command::Exit) {
        stop_operation = true; local_stop_requested = true; ++local_epoch;
        if (intent.command == local::Command::Exit) local_exit_requested = true;
        return;
    }
    if (intent.command == local::Command::None || local_pending || worker_running || local_runtime_inventory() ||
        local_stop_requested || local_exit_requested) {
        ui_message = "Finish or stop the active local operation.";
        return;
    }
    auto* work = new (std::nothrow) Work{};
    if (!work) { ui_message = "Insufficient memory for a local request."; return; }
    work->local_job = true; work->epoch = local_epoch; work->intent = intent;
    work->intent.text.back() = '\0';
    ++local_pending;
    if (xQueueSend(work_queue, &work, 0) != pdTRUE) {
        --local_pending; delete work; ui_message = "Local request queue is busy.";
    }
}
static void local_action(UiAction action, const UiSnapshot& shown) {
    if (pairing_pending && action != UiAction::ApprovePairing && action != UiAction::RejectPairing) return;
    if (release_unverified && (action == UiAction::ChooseConnected || action == UiAction::ChooseStandalone ||
        action == UiAction::ChooseHome || action == UiAction::ConfirmMode || action == UiAction::Stop)) return;
    switch (action) {
    case UiAction::LocalRequest: submit_local(ui::take_local_intent()); break;
    case UiAction::ChooseConnected: request_mode(WorkMode::Connected); break;
    case UiAction::ChooseStandalone: request_mode(WorkMode::Standalone); break;
    case UiAction::ChooseHome: request_mode(WorkMode::Home); break;
    case UiAction::ConfirmMode: if (confirm_mode) request_mode(confirm_target, true); break;
    case UiAction::CancelMode: confirm_mode = false; break;
    case UiAction::OpenPairing:
        if (pairing_pending) break;
        pairing_until = esp_timer_get_time() + 60000000;
        break;
    case UiAction::ApprovePairing: ble_pair_approve(true, shown.pairing_challenge); break;
    case UiAction::RejectPairing: ble_pair_approve(false, shown.pairing_challenge); break;
    case UiAction::RequestUsbHost: if (!usb_host_approved) confirm_usb = true; break;
    case UiAction::ConfirmUsbHost: {
        if (!confirm_usb) break;
        confirm_usb = false;
        std::lock_guard<std::mutex> guard(state_mutex);
        if (worker_running || session.pending || local_pending || local_runtime_active() || modes.pending()) {
            ui_message = "Finish the reader operation before enabling USB host power.";
            break;
        }
        usb_host_state = UsbHostState::Requested; usb_host_approved = true; refresh_requested = true;
        break;
    }
    case UiAction::CancelUsbHost: confirm_usb = false; break;
    case UiAction::RequestClearBonds: confirm_bonds = true; break;
    case UiAction::ConfirmClearBonds:
        if (!confirm_bonds) break;
        confirm_bonds = false; pairing_until = 0; ble_clear_bonds(); break;
    case UiAction::CancelClearBonds: confirm_bonds = false; break;
    case UiAction::Stop: {
        WorkMode current;
        { std::lock_guard<std::mutex> guard(state_mutex); current = modes.mode(); }
        if (current == WorkMode::Connected) request_mode(WorkMode::Home, true);
        else {
            local::Intent stop; stop.command = local::Command::Stop; submit_local(stop);
        }
        break;
    }
    case UiAction::ToggleTheme: light_theme = !light_theme; save_theme(); break;
    case UiAction::Refresh: refresh_requested = true; break;
    default: break;
    }
}
static void worker(void*) {
    int64_t next_poll = 0, next_inventory = 0;
    local_runtime_initialize();
    for (;;) {
        Work* work = nullptr;
        if (xQueueReceive(work_queue, &work, pdMS_TO_TICKS(30)) == pdTRUE) {
            worker_running = true;
            if (work->local_job) {
                if (local_operation_allowed(work->epoch) && !local_stop_requested) {
                    stop_operation = false;
                    last_status = local_runtime_process(work->intent, work->epoch);
                    if (local_runtime_release_status() != OK) latch_release_failure(local_runtime_release_status());
                }
                --local_pending;
            } else {
                Frame result = work->frame; result.kind = 1; result.payload.clear();
                {
                    std::lock_guard<std::mutex> guard(state_mutex);
                    if (work->epoch != session.epoch || !session.nonce || !modes.remote_allowed()) result.status = CANCELLED;
                    else if (slots[result.channel - 1].generation != result.generation) result.status = STALE_GENERATION;
                }
                if (result.status == OK) {
                    try { result.status = operate(work->frame, result.payload); }
                    catch (const std::exception&) { result.status = WRITE_UNVERIFIED; }
                }
                last_status = result.status;
                {
                    std::lock_guard<std::mutex> guard(state_mutex);
                    if (work->epoch == session.epoch && session.nonce == result.session) {
                        if (slots[result.channel - 1].generation != result.generation && result.status == OK)
                            result.status = result.channel == 1 && (result.op == 18 || result.op == 20 || result.op == 22 || result.op == 23) ?
                                WRITE_UNVERIFIED : STALE_GENERATION;
                        session.pending = 0;
                        if (result.status != OK) result.payload.clear();
                        ble_send(result);
                    }
                }
            }
            delete work;
            worker_running = false;
        }
        if (local_stop_requested.exchange(false)) {
            worker_running = true;
            const bool exit = local_exit_requested.exchange(false);
            const auto epoch = local_epoch.load();
            const auto status = local_runtime_cancel(epoch, exit);
            if (status != OK) latch_release_failure(status);
            {
                std::lock_guard<std::mutex> guard(state_mutex);
                if (!modes.pending() && !release_unverified && local_epoch == epoch) stop_operation = false;
            }
            local_runtime_refresh(epoch);
            worker_running = false;
        }
        finish_idle_transition();
        const auto epoch = local_epoch.load();
        if (!local_pending && local_operation_allowed(epoch) && local_runtime_inventory() &&
            esp_timer_get_time() >= next_inventory) {
            worker_running = true;
            last_status = local_runtime_tick(epoch);
            if (local_runtime_release_status() != OK) latch_release_failure(local_runtime_release_status());
            worker_running = false; next_inventory = esp_timer_get_time() + 100000;
        }
        if (!worker_running && !local_pending && !local_runtime_inventory() &&
            (refresh_requested.exchange(false) || esp_timer_get_time() > next_poll)) {
            hardware_poll(); next_poll = esp_timer_get_time() + 3000000;
            if (local_operation_allowed(epoch)) local_runtime_refresh(epoch);
        }
    }
}
}
extern "C" void app_main() {
    using namespace qb;
    ESP_LOGI("quadband", "Starting CoreS3 hub; internal heap=%u bytes, external heap=%u bytes",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_handle_t prefs;
    if (nvs_open("qbui", NVS_READONLY, &prefs) == ESP_OK) {
        uint8_t value = 0; nvs_get_u8(prefs, "light", &value); light_theme = value == 1; nvs_close(prefs);
    }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT); device_id = hex(mac, 6);
    slots[1].kind = "jrd4035"; slots[2].kind = "chameleon";
    work_queue = xQueueCreate(1, sizeof(Work*));
    hardware_init(); ble_init();
    ui::initialize();
    ESP_LOGI("quadband", "Board initialized; BLE service ready; USB host output off");
    xTaskCreate(worker, "peripherals", 24576, nullptr, 3, nullptr);
    int64_t next_metrics = 0;
    int battery = -1; uint32_t internal = 0, external = 0;
    TouchGate touches;
    for (;;) {
        M5.update();
        auto touch = M5.Touch.getDetail();
        if (pairing_pending && esp_timer_get_time() > pairing_until) ble_pair_approve(false);
        if (esp_timer_get_time() > next_metrics) {
            battery = M5.Power.getBatteryLevel();
            internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            external = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            next_metrics = esp_timer_get_time() + 1000000;
        }
        UiSnapshot state;
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            state.mode = modes.mode(); state.transition_target = modes.target(); state.transition_pending = modes.pending();
            state.peripherals = slots; state.request_id = session.pending; state.message = ui_message;
            state.release_unverified = release_unverified;
        }
        state.confirm_mode = confirm_mode; state.confirm_target = confirm_target;
        state.confirm_usb = confirm_usb; state.confirm_bonds = confirm_bonds;
        state.paired = link_authorized; state.pairing_pending = pairing_pending;
        state.pairing_open = esp_timer_get_time() < pairing_until;
        state.pairing_challenge = pairing_challenge.load();
        state.pairing_code = uint32_t(state.pairing_challenge & 0xfffff);
        state.busy = worker_running || state.request_id != 0 || local_pending || local_runtime_inventory();
        state.last_status = last_status;
        local_runtime_snapshot(state.local);
        state.local.light_theme = light_theme; state.local.battery = battery;
        state.battery_percent = battery; state.internal_bytes = internal; state.external_bytes = external;
        state.usb_host = usb_host_state; state.light_theme = light_theme;
        ui::update(state);
        if (touches.permit(state, touch.wasPressed(), touch.isPressed()))
            local_action(ui::touch(touch.x, touch.y), state);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
