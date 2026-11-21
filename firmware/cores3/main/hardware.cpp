#include "runtime.hpp"
#include "standalone/format.hpp"
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedRFID.h>
#include <wiring/m5_unit_unified_wiring.hpp>
#include <nfc/layer/a/nfc_layer_a.hpp>
#include <driver/uart.h>
#include <usb/usb_host.h>
#include <usb/cdc_acm_host.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <cstring>
#include <memory>
#include <set>

namespace qb {
using m5::nfc::a::PICC;
using m5::nfc::a::Type;
static m5::unit::UnitUnified units, units2;
static m5::unit::UnitMFRC522 rfid;
static m5::unit::UnitWS1850S rfid2;
static m5::unit::UnitMFRC522* hf = nullptr;
static std::unique_ptr<m5::nfc::NFCLayerA> layer;
static bool hf_wired = false, hf2_wired = false;
static uint8_t fingerprint = 0;
static uart_port_t uart = UART_NUM_1;
static cdc_acm_dev_hdl_t usb = nullptr;
static bool usb_quarantined = false;
static std::atomic<bool> usb_gone{false};
static std::mutex usb_mutex;
static Bytes usb_rx;
static int original_mode = -1;
static UhfGuard uhf_guard;
struct UsbRead { bool valid = false; std::array<uint8_t, 8> credential{}; std::array<uint8_t, 16> data{}; };
static std::array<UsbRead, 256> usb_reads;
static Bytes usb_identity;
static unsigned usb_blocks = 0;
static std::set<unsigned> usb_commands;
static uint64_t operation_session = 0;
enum class EventOrigin { None, Remote, Local };
static EventOrigin event_origin = EventOrigin::None;
static std::function<void(const Bytes&)> local_uhf_events;
static bool uhf_inventory_used = false;
struct OriginScope {
    EventOrigin previous;
    explicit OriginScope(const Frame& frame) : previous(event_origin) {
        event_origin = frame.session ? EventOrigin::Remote : EventOrigin::Local;
    }
    ~OriginScope() { event_origin = previous; }
};
static void clear_usb_reads() {
    for (auto& entry : usb_reads) { entry.valid = false; entry.credential.fill(0); entry.data.fill(0); }
    std::fill(usb_identity.begin(), usb_identity.end(), 0); usb_identity.clear(); usb_blocks = 0;
}
static Bytes uhf_frame(uint8_t command, const Bytes& p = {}) {
    Bytes b{0xbb, 0, command, uint8_t(p.size() >> 8), uint8_t(p.size())};
    b.insert(b.end(), p.begin(), p.end());
    uint8_t sum = 0; for (size_t i = 1; i < b.size(); ++i) sum += b[i];
    b.push_back(sum); b.push_back(0x7e); return b;
}
static Bytes chameleon_frame(unsigned command, const Bytes& p = {}) {
    Bytes b{0x11, 0xef, uint8_t(command >> 8), uint8_t(command), 0, 0,
            uint8_t(p.size() >> 8), uint8_t(p.size()), 0};
    uint8_t s = 0; for (auto v : b) s += v; b[8] = uint8_t(-s);
    b.insert(b.end(), p.begin(), p.end()); s = 0; for (auto v : p) s += v;
    b.push_back(uint8_t(-s)); return b;
}
static bool usb_data(const uint8_t* p, size_t n, void*) {
    std::lock_guard<std::mutex> guard(usb_mutex);
    if (usb_rx.size() + n > max_frame) { usb_gone = true; return true; }
    usb_rx.insert(usb_rx.end(), p, p + n); return true;
}
static void usb_event(const cdc_acm_host_dev_event_data_t* event, void*) {
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED || event->type == CDC_ACM_HOST_ERROR) usb_gone = true;
}
static Status usb_transaction(const Bytes& request, Bytes& response) {
    if (!usb || usb_gone) return UNAVAILABLE;
    { std::lock_guard<std::mutex> guard(usb_mutex); usb_rx.clear(); }
    unsigned command = unsigned(request[2]) << 8 | request[3];
    int64_t deadline = esp_timer_get_time() + int64_t(usb_timeout_ms(command)) * 1000;
    if (cdc_acm_host_data_tx_blocking(usb, request.data(), request.size(), 1000) != ESP_OK) return IO_ERROR;
    while (esp_timer_get_time() < deadline && !usb_gone) {
        {
            std::lock_guard<std::mutex> guard(usb_mutex);
            if (usb_rx.size() >= 9) {
                auto* p = usb_rx.data(); size_t total = (unsigned(p[6]) << 8 | p[7]) + 10;
                if (total > max_payload || p[0] != 0x11 || uint8_t(p[0] + p[1]) != 0) break;
                if (usb_rx.size() >= total) {
                    uint8_t sum = 0, h = 0;
                    for (size_t i = 0; i < 9; ++i) h += p[i];
                    for (size_t i = 9; i < total; ++i) sum += p[i];
                    if (h || sum || p[2] != request[2] || p[3] != request[3] || usb_rx.size() != total) break;
                    response = usb_rx; usb_rx.clear(); return OK;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    usb_gone = true;
    return TIMEOUT;
}
static Status uart_transaction(const Bytes& request, Bytes& response, int64_t timeout_us = 2000000) {
    uint64_t origin;
    uint32_t generation;
    {
        std::lock_guard<std::mutex> guard(state_mutex);
        origin = session.nonce; generation = slots[1].generation;
    }
    const auto route = event_origin;
    auto notify = [origin, generation, route](const Bytes& bytes) {
        if (route == EventOrigin::Local) {
            if (local_uhf_events) local_uhf_events(bytes);
            return;
        }
        if (route != EventOrigin::Remote || !remote_event_allowed(origin, generation)) return;
        Frame e; e.kind = 2; e.session = origin; e.channel = 2; e.op = 32;
        e.generation = generation; e.payload = bytes; ble_send(e);
    };
    uart_flush_input(uart);
    if (uart_write_bytes(uart, request.data(), request.size()) != int(request.size())) return IO_ERROR;
    int64_t deadline = esp_timer_get_time() + timeout_us;
    response.clear();
    Bytes buffer;
    bool matched = false;
    while (esp_timer_get_time() < deadline) {
        uint8_t b[128]; int n = uart_read_bytes(uart, b, sizeof(b), pdMS_TO_TICKS(20));
        if (n > 0) buffer.insert(buffer.end(), b, b + n);
        for (;;) {
            Bytes inner; auto status = take_uhf_frame(buffer, inner);
            if (status == BUSY) break;
            if (status != OK) return status;
            bool inventory = inner[1] == 2 && inner[2] == 0x22;
            if (!matched && (inner[2] == request[2] || (inner[1] == 1 && inner[2] == 0xff))) {
                response = inner; matched = true;
            } else if (inventory) notify(inner);
            else return IO_ERROR;
        }
        if (matched && buffer.empty()) {
            if (request[2] != 0x22 || n == 0) return OK;
            size_t waiting = 0;
            uart_get_buffered_data_len(uart, &waiting);
            if (!waiting) {
                vTaskDelay(pdMS_TO_TICKS(10));
                uart_get_buffered_data_len(uart, &waiting);
                if (!waiting) return OK;
            }
        }
    }
    uart_wait_tx_done(uart, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(250));
    uart_flush_input(uart);
    return TIMEOUT;
}
static bool usb_success(const Bytes& b, unsigned status = 0) {
    return b.size() >= 10 && b[4] == uint8_t(status >> 8) && b[5] == uint8_t(status);
}
static Status usb_scan(Bytes& identity, unsigned& geometry) {
    Bytes response;
    auto status = usb_transaction(chameleon_frame(2000), response);
    if (status != OK) return status;
    if (!usb_success(response)) return response.size() >= 10 && response[5] == 1 ? NO_CARD : IO_ERROR;
    if (response.size() < 19) return IO_ERROR;
    const uint8_t* p = response.data() + 9;
    unsigned uid = p[0];
    if ((uid != 4 && uid != 7 && uid != 10) || response.size() < uid + 15 ||
        response.size() != uid + 15 + p[uid + 4]) return IO_ERROR;
    identity.assign(p, p + uid + 4);
    geometry = classic_geometry(p[uid + 3], p + uid + 1, uid);
    return OK;
}
Status hardware_release() {
    operation_session = 0;
    Status status = hardware_release_channel(1);
    const auto uhf = hardware_release_channel(2), usb_status = hardware_release_channel(3);
    if (status == OK) status = uhf;
    if (status == OK) status = usb_status;
    return status;
}
static Status usb_guarded_transaction(const Frame& f, Bytes& out) {
    const auto& b = f.payload;
    unsigned cmd = unsigned(b[2]) << 8 | b[3];
    if (!usb_commands.count(cmd)) return UNSUPPORTED;
    if (cmd != 2008 && cmd != 2009) {
        if (cmd == 1001 || cmd >= 3000) clear_usb_reads();
        return usb_transaction(b, out);
    }
    const auto* p = b.data() + 9;
    unsigned block = p[1];
    Bytes identity; unsigned geometry = 0;
    auto status = usb_scan(identity, geometry);
    if (status != OK) { clear_usb_reads(); return status; }
    if (!geometry || block >= geometry) return UNSUPPORTED;
    if (cmd == 2008) {
        if (identity != usb_identity) clear_usb_reads();
        usb_identity = identity; usb_blocks = geometry;
        status = usb_transaction(b, out);
        if (status == OK && usb_success(out) && out.size() == 26) {
            auto& entry = usb_reads[block];
            std::copy(p, p + 8, entry.credential.begin());
            std::copy(out.begin() + 9, out.begin() + 25, entry.data.begin()); entry.valid = true;
        }
        return status;
    }
    auto& entry = usb_reads[block];
    if (!classic_data(geometry, block) || !entry.valid || usb_identity != identity || usb_blocks != geometry ||
        memcmp(entry.credential.data(), p, 8)) return PERMISSION_DENIED;
    Bytes read_payload(p, p + 8), response;
    read_payload[1] = trailer(block);
    status = usb_transaction(chameleon_frame(2008, read_payload), response);
    if (status != OK) return status;
    if (!usb_success(response) || response.size() != 26) return AUTH_FAILED;
    if (!classic_write_allowed(block, p[0], response.data() + 9)) return PERMISSION_DENIED;
    read_payload[1] = block;
    status = usb_transaction(chameleon_frame(2008, read_payload), response);
    if (status != OK) return status;
    if (!usb_success(response) || response.size() != 26) return AUTH_FAILED;
    if (memcmp(response.data() + 9, entry.data.data(), 16)) return STALE_DATA;
    if (stop_operation) return CANCELLED;
    entry.valid = false;
    status = usb_transaction(b, out);
    if (status != OK) return WRITE_UNVERIFIED;
    if (!usb_success(out)) return OK;
    if (out.size() != 10) return WRITE_UNVERIFIED;
    status = usb_transaction(chameleon_frame(2008, read_payload), response);
    std::fill(read_payload.begin(), read_payload.end(), 0);
    if (status != OK || !usb_success(response) || response.size() != 26 || memcmp(response.data() + 9, p + 8, 16))
        return WRITE_UNVERIFIED;
    return OK;
}
static Status select_card(PICC& card) {
    if (!hf || !layer) return UNAVAILABLE;
    hf->mifareClassicStopCrypto1(); hf->turnOffAntenna();
    vTaskDelay(pdMS_TO_TICKS(8)); hf->turnOnAntenna();
    uint16_t atqa = 0;
    if (!hf->wakeup(atqa)) {
        uint8_t v = 0;
        return hf->readRegister8(uint8_t(0x37), v, 0) && v == fingerprint ? NO_CARD : IO_ERROR;
    }
    if (!layer->select(card)) return IO_ERROR;
    card.atqa = atqa;
    if (!layer->identify(card) || !layer->reactivate(card)) return IO_ERROR;
    return OK;
}
static unsigned blocks(const PICC& p) {
    return p.type == Type::MIFARE_Classic_1K ? 64 : p.type == Type::MIFARE_Classic_4K ? 256 : 0;
}
static unsigned pages(const PICC& p) {
    return p.type == Type::NTAG_213 ? 45 : p.type == Type::NTAG_215 ? 135 : p.type == Type::NTAG_216 ? 231 : 0;
}
static bool transceive(const uint8_t* tx, size_t n, uint8_t* rx, uint16_t& len) {
    return hf->nfcaTransceive(rx, len, tx, n, 50);
}
static Status auth_ntag(const Bytes& p, size_t& offset, bool& authenticated) {
    if (offset >= p.size()) return INVALID_ARGUMENT;
    unsigned mode = p[offset++]; authenticated = false;
    if (mode == 0) return OK;
    if (mode > 2 || p.size() - offset < (mode == 1 ? 4u : 6u)) return INVALID_ARGUMENT;
    uint8_t tx[5]{0x1b}, pack[2]{};
    memcpy(tx + 1, p.data() + offset, 4);
    uint16_t len = 2; bool good = transceive(tx, sizeof(tx), pack, len);
    memset(tx, 0, sizeof(tx));
    if (!good || len != 2 || (mode == 2 && memcmp(pack, p.data() + offset + 4, 2))) return AUTH_FAILED;
    offset += mode == 1 ? 4 : 6; authenticated = true; return OK;
}
static Status hf_operation(const Frame& f, Bytes& out) {
    const auto& p = f.payload;
    auto shape = validate_hf(f);
    if (shape != OK) return shape;
    if (f.op == 21) {
        if (!p.empty()) return INVALID_ARGUMENT;
        hf->mifareClassicStopCrypto1(); hf->turnOffAntenna(); return OK;
    }
    if (f.op < 16 || f.op > 23) return UNSUPPORTED;
    if (f.op == 16 && !p.empty()) return INVALID_ARGUMENT;
    if (f.op != 16 && (p.empty() || (p[0] != 4 && p[0] != 7 && p[0] != 10) || p.size() < size_t(p[0]) + 3)) return INVALID_ARGUMENT;
    PICC card; Status s = select_card(card);
    if (s != OK) return s;
    if (f.op != 16 && (card.size != p[0] || memcmp(card.uid, p.data() + 1, card.size))) return STALE_DATA;
    if (stop_operation) return CANCELLED;
    unsigned bc = blocks(card), pc = pages(card);
    if (f.op == 22 || f.op == 23) {
        if ((f.op == 22 && bc != 64) || (f.op == 23 && !pc)) return UNSUPPORTED;
        local::HfIdentity expected;
        expected.family = bc == 64 ? local::HfFamily::Classic1K : pc == 45 ? local::HfFamily::Ntag213 :
            pc == 135 ? local::HfFamily::Ntag215 : local::HfFamily::Ntag216;
        expected.uid_size = card.size; std::copy_n(card.uid,card.size,expected.uid.begin());
        expected.sak = card.sak; expected.atqa = {{uint8_t(card.atqa),uint8_t(card.atqa>>8)}};
        if (pc) {
            uint8_t cmd = 0x60; uint16_t n = 8;
            if (!transceive(&cmd,1,expected.version.data(),n) || n != 8) return IO_ERROR;
        }
        local::FormatIO io;
        io.guard = [&]() -> Status {
            if (stop_operation) return CANCELLED;
            { std::lock_guard<std::mutex> lock(state_mutex);
              if (slots[0].generation != f.generation || !slots[0].present) return STALE_GENERATION; }
            PICC actual;
            auto status = select_card(actual); if (status != OK) return status;
            if (actual.size != expected.uid_size || memcmp(actual.uid,expected.uid.data(),actual.size) ||
                actual.sak != expected.sak || actual.atqa != card.atqa ||
                blocks(actual) != bc || pages(actual) != pc) return STALE_DATA;
            if (pc) {
                uint8_t cmd = 0x60, version[8]; uint16_t n = 8;
                if (!transceive(&cmd,1,version,n) || n != 8) return IO_ERROR;
                if (memcmp(version,expected.version.data(),8)) return STALE_DATA;
            }
            return stop_operation ? CANCELLED : OK;
        };
        auto authenticate = [&](unsigned b, const local::ClassicKey& credential) {
            if (!bc) return true;
            m5::nfc::a::mifare::classic::Key key;
            std::copy(credential.bytes.begin(),credential.bytes.end(),key.data());
            bool good = layer->mifareClassicAuthenticateA(b,key);
            memset(key.data(),0,6); return good;
        };
        io.read = [&](unsigned b, const local::ClassicKey& key, Bytes& data) -> Status {
            auto status = io.guard(); if (status != OK) return status;
            if (!authenticate(b,key)) return AUTH_FAILED;
            data.resize(bc ? 16 : 4);
            return (bc ? layer->read16(data.data(),b) : layer->read4(data.data(),b)) ? OK : IO_ERROR;
        };
        io.write = [&](unsigned b, const local::ClassicKey& key, const Bytes& data) -> Status {
            if (stop_operation) return CANCELLED;
            if (!authenticate(b,key)) return AUTH_FAILED;
            return (bc ? layer->write16(b,data.data(),16) : layer->write4(b,data.data(),4)) ? OK : WRITE_UNVERIFIED;
        };
        const size_t offset = 1+p[0];
        return local::canonical_format(expected,p[offset],Bytes(p.begin()+offset+1,p.end()),io);
    }
    if (f.op == 16) {
        uint8_t version[8]{}; uint16_t n = 8; uint8_t cmd = 0x60;
        std::string v;
        if (pc && transceive(&cmd, 1, version, n) && n == 8) v = hex(version, 8);
        else if (pc) return IO_ERROR;
        std::string family = bc == 64 ? "classic1k" : bc == 256 ? "classic4k" :
            pc == 45 ? "ntag213" : pc == 135 ? "ntag215" : pc == 231 ? "ntag216" : "unknown";
        uint8_t atqa[]{uint8_t(card.atqa), uint8_t(card.atqa >> 8)};
        std::string j = "{\"uid\":\"" + hex(card.uid, card.size) + "\",\"atqa\":\"" + hex(atqa, 2) +
            "\",\"sak\":" + std::to_string(card.sak) + ",\"family\":\"" + family +
            "\",\"blockCount\":" + std::to_string(bc) + ",\"pageCount\":" + std::to_string(pc) +
            ",\"version\":\"" + v + "\"}";
        out.assign(j.begin(), j.end()); return OK;
    }
    size_t offset = 1 + p[0];
    unsigned address = le16(p.data() + offset); offset += 2;
    if (f.op == 17 || f.op == 18) {
        if (!bc || address >= bc) return INVALID_ARGUMENT;
        if (p.size() != offset + (f.op == 18 ? 39 : 7)) return INVALID_ARGUMENT;
        uint8_t key_type = p[offset++];
        if (key_type != 0x60 && key_type != 0x61) return INVALID_ARGUMENT;
        m5::nfc::a::mifare::classic::Key key;
        memcpy(key.data(), p.data() + offset, 6); offset += 6;
        bool auth = key_type == 0x60 ? layer->mifareClassicAuthenticateA(address, key) :
            layer->mifareClassicAuthenticateB(address, key);
        memset(key.data(), 0, 6);
        if (!auth) return AUTH_FAILED;
        uint8_t previous[16]{};
        if (f.op == 18) {
            if (!classic_data(bc, address)) return PERMISSION_DENIED;
            uint8_t t[16]{};
            if (!layer->read16(t, trailer(address))) return IO_ERROR;
            if (!classic_write_allowed(address, key_type, t)) return PERMISSION_DENIED;
        }
        if (!layer->read16(previous, address)) return IO_ERROR;
        if (f.op == 17) { out.assign(previous, previous + 16); return OK; }
        if (memcmp(previous, p.data() + offset, 16)) return STALE_DATA;
        if (stop_operation) return CANCELLED;
        if (!layer->write16(address, p.data() + offset + 16, 16)) return WRITE_UNVERIFIED;
        if (!layer->read16(previous, address) || memcmp(previous, p.data() + offset + 16, 16)) return WRITE_UNVERIFIED;
        out.assign(previous, previous + 16); return OK;
    }
    if (!pc) return UNSUPPORTED;
    unsigned count = 1;
    if (f.op == 19) { if (offset >= p.size()) return INVALID_ARGUMENT; count = p[offset++]; }
    if (address + count > pc) return INVALID_ARGUMENT;
    bool authenticated = false;
    s = auth_ntag(p, offset, authenticated); if (s != OK) return s;
    if (p.size() != offset + (f.op == 20 ? 8 : 0) || !count || count > 16 || address + count > pc) return INVALID_ARGUMENT;
    if (f.op == 19) {
        out.resize(count * 4);
        for (unsigned i = 0; i < count; i += 4) {
            if (stop_operation) return CANCELLED;
            uint8_t chunk[16];
            if (!layer->read16(chunk,address+i)) return IO_ERROR;
            std::copy_n(chunk,std::min(4u,count-i)*4,out.begin()+i*4);
        }
        PICC actual;
        s = select_card(actual); if (s != OK) return s;
        if (actual.size != card.size || memcmp(actual.uid,card.uid,card.size) ||
            actual.sak != card.sak || actual.atqa != card.atqa || pages(actual) != pc) return STALE_DATA;
        return OK;
    }
    uint8_t initial[16]{}, dyn[4]{}, cfg[8]{}, previous[4]{};
    auto l = ntag(pc);
    if (!layer->read16(initial, 0) || !layer->read4(dyn, l.dynamic) ||
        !layer->read4(cfg, l.dynamic + 1) || !layer->read4(cfg + 4, l.dynamic + 2)) return PERMISSION_DENIED;
    if (!ntag_write_allowed(pc, address, initial, dyn, cfg, authenticated)) return PERMISSION_DENIED;
    if (!layer->read4(previous, address)) return IO_ERROR;
    if (memcmp(previous, p.data() + offset, 4)) return STALE_DATA;
    if (stop_operation) return CANCELLED;
    if (!layer->write4(address, p.data() + offset + 4, 4)) return WRITE_UNVERIFIED;
    if (!layer->read4(previous, address) || memcmp(previous, p.data() + offset + 4, 4)) return WRITE_UNVERIFIED;
    out.assign(previous, previous + 4); return OK;
}
void hardware_init() {
    auto config = M5.config();
    config.internal_imu = false; config.internal_rtc = false;
    config.internal_mic = false; config.internal_spk = false;
    config.external_display_value = 0;
    M5.begin(config);
    M5.Display.setTextSize(2); M5.Display.setRotation(1);
    ESP_LOGI("quadband", "Detected board=%d; Port A SDA=%d SCL=%d",
             int(M5.getBoard()), int(M5.getPin(m5::pin_name_t::port_a_sda)),
             int(M5.getPin(m5::pin_name_t::port_a_scl)));
    if (M5.getBoard() != m5::board_t::board_M5StackCoreS3 ||
        M5.getPin(m5::pin_name_t::port_a_sda) != 2 || M5.getPin(m5::pin_name_t::port_a_scl) != 1) abort();
    M5.Ex_I2C.begin();
    auto pins = m5::unit::wiring::uartPins();
    if (pins.tx != 17 || pins.rx != 18) abort();
    uart_config_t uc{};
    uc.baud_rate = 115200; uc.data_bits = UART_DATA_8_BITS; uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1; uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE; uc.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(uart, 4096, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(uart, &uc));
    ESP_ERROR_CHECK(uart_set_pin(uart, pins.tx, pins.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    M5.Power.setUsbOutput(false);
    usb_host_state = UsbHostState::Off;
}
void hardware_poll() {
    static bool usb_powered = false;
    static bool usb_installed = false;
    static bool usb_setup_failed = false;
    if (usb_host_approved && !usb_powered) {
        if (usb_setup_failed) {
            usb_host_approved = false; usb_host_state = UsbHostState::Failed; return;
        }
        int voltage = M5.Power.getVBUSVoltage();
        if (voltage < 0 || voltage > 1000) {
            usb_host_approved = false; usb_host_state = UsbHostState::ExternalPowerBlocked; return;
        }
        if (!usb_installed) {
            usb_host_config_t host{}; host.intr_flags = ESP_INTR_FLAG_LEVEL1;
            esp_err_t error = usb_host_install(&host);
            const bool host_ready = error == ESP_OK;
            if (host_ready) error = cdc_acm_host_install(nullptr);
            const bool cdc_ready = host_ready && error == ESP_OK;
            if (cdc_ready && xTaskCreate(
                    [](void*) { for (;;) { uint32_t flags; usb_host_lib_handle_events(portMAX_DELAY, &flags); } },
                    "usb-events", 4096, nullptr, 5, nullptr) != pdPASS) error = ESP_ERR_NO_MEM;
            if (error != ESP_OK) {
                ESP_LOGE("quadband", "Optional USB setup failed: %s; restart required", esp_err_to_name(error));
                if (cdc_ready) {
                    esp_err_t cleanup = cdc_acm_host_uninstall();
                    if (cleanup != ESP_OK) ESP_LOGE("quadband", "USB CDC cleanup failed: %s", esp_err_to_name(cleanup));
                }
                if (host_ready) {
                    esp_err_t cleanup = usb_host_uninstall();
                    if (cleanup != ESP_OK) ESP_LOGE("quadband", "USB host cleanup failed: %s", esp_err_to_name(cleanup));
                }
                M5.Power.setUsbOutput(false);
                usb_setup_failed = true; usb_host_approved = false; usb_host_state = UsbHostState::Failed;
                return;
            }
            usb_installed = true;
        }
        voltage = M5.Power.getVBUSVoltage();
        if (voltage < 0 || voltage > 1000) {
            usb_host_approved = false; usb_host_state = UsbHostState::ExternalPowerBlocked; return;
        }
        M5.Power.setUsbOutput(true);
        usb_powered = M5.Power.getUsbOutput();
        if (!usb_powered) { usb_host_approved = false; usb_host_state = UsbHostState::Failed; return; }
        usb_host_state = UsbHostState::Enabled;
    }
    uint8_t v = 0;
    bool detected = M5.Ex_I2C.readRegister(0x28, 0x37, &v, 1, 400000);
    if (!detected) v = 0;
    if (v != fingerprint) {
        layer.reset(); hf = nullptr; fingerprint = v;
        bool known = v == 0x15 || v == 0x91 || v == 0x92;
        if (known) {
            hf = v == 0x15 ? static_cast<m5::unit::UnitMFRC522*>(&rfid2) : &rfid;
            bool& wired = v == 0x15 ? hf2_wired : hf_wired;
            if (!wired) wired = (v == 0x15 ? units2 : units).add(*hf, M5.Ex_I2C);
            if (!wired || !hf->begin()) hf = nullptr;
            else { hf->turnOffAntenna(); layer.reset(new m5::nfc::NFCLayerA(*hf)); }
        }
        peripheral_changed(1, hf != nullptr || (detected && !known), v == 0x15 ? "rfid2" : known ? "rfid" : "unknown",
            hf ? std::vector<std::string>{"identify", "classic1k", "classic4k", "ntag21x", "ntag_auth", "classic1k_format", "ntag_initialize"} : std::vector<std::string>{});
    }
    Bytes response;
    Status u = uart_transaction({0xbb, 0, 3, 0, 1, 0, 4, 0x7e}, response, 300000);
    bool uhf_present = u == OK && response.size() >= 9 && response[1] == 1 && response[2] == 3 && response[5] == 0;
    std::string hardware = uhf_present ? std::string(response.begin() + 6, response.end() - 2) : "";
    for (char& c : hardware) if (c >= 'a' && c <= 'z') c -= 32;
    uhf_present = uhf_present && (hardware.find("M100") != std::string::npos ||
        hardware.find("JRD") != std::string::npos || hardware.find("YRM") != std::string::npos);
    if (!uhf_present) uhf_guard.clear();
    peripheral_changed(2, uhf_present, "jrd4035",
        uhf_present ? std::vector<std::string>{"m100"} : std::vector<std::string>{});
    if (usb_gone && usb) {
        clear_usb_reads();
        usb_commands.clear();
        cdc_acm_host_close(usb); usb = nullptr; original_mode = -1;
        { std::lock_guard<std::mutex> guard(usb_mutex); std::fill(usb_rx.begin(), usb_rx.end(), 0); usb_rx.clear(); }
        peripheral_changed(3, false, "chameleon", {});
    }
    if (usb_powered && !usb && !usb_quarantined) {
        usb_gone = false;
        cdc_acm_host_device_config_t cfg{};
        cfg.connection_timeout_ms = 10; cfg.out_buffer_size = 2048; cfg.in_buffer_size = 2048;
        cfg.event_cb = usb_event; cfg.data_cb = usb_data;
        if (cdc_acm_host_open(0x6868, 0x8686, 0, &cfg, &usb) == ESP_OK) {
            cdc_acm_line_coding_t coding{115200, 0, 0, 8};
            bool ready = cdc_acm_host_line_coding_set(usb, &coding) == ESP_OK &&
                cdc_acm_host_set_control_line_state(usb, true, true) == ESP_OK &&
                usb_transaction(chameleon_frame(1000), response) == OK &&
                usb_success(response, 0x68) && response.size() == 12 && response[9] == 2;
            ready = ready && usb_transaction(chameleon_frame(1033), response) == OK &&
                usb_success(response, 0x68) && response.size() == 11 && response[9] == 0;
            ready = ready && usb_transaction(chameleon_frame(1035), response) == OK &&
                usb_success(response, 0x68) && response.size() > 10 && (response.size() - 10) % 2 == 0;
            usb_commands.clear();
            if (ready) for (size_t i = 9; i + 1 < response.size() - 1; i += 2)
                usb_commands.insert(unsigned(response[i]) << 8 | response[i + 1]);
            ready = ready && usb_commands.count(1001) && usb_commands.count(1002);
            if (ready && usb_transaction(chameleon_frame(1002), response) == OK &&
                usb_success(response, 0x68) && response.size() == 11 && response[9] <= 1)
                original_mode = response[9];
            else ready = false;
            if (ready) {
                std::vector<std::string> caps{"chameleon"};
                if (usb_commands.count(2000) && usb_commands.count(2008)) {
                    caps.push_back("classic1k"); caps.push_back("classic4k");
                }
                if (usb_commands.count(3016)) caps.push_back("t5577_password_free");
                peripheral_changed(3, true, "chameleon", caps);
            }
            else usb_gone = true;
        }
    }
}
void hardware_local_uhf_handler(std::function<void(const Bytes&)> handler) {
    local_uhf_events = std::move(handler);
}
bool hardware_command_supported(uint8_t channel, uint16_t command) {
    if (channel == 1) return hf && command >= 16 && command <= 23;
    if (channel == 3) return usb && !usb_gone && !usb_quarantined && usb_commands.count(command);
    if (channel != 2) return false;
    switch (command) {
    case 3: case 8: case 11: case 12: case 18: case 34: case 40:
    case 57: case 73: case 182: case 183: return true;
    default: return false;
    }
}
Status hardware_release_channel(uint8_t channel) {
    if (channel == 1) {
        if (hf) { hf->mifareClassicStopCrypto1(); hf->turnOffAntenna(); }
        return OK;
    }
    if (channel == 2) {
        uhf_guard.clear();
        if (!uhf_inventory_used) return OK;
        uhf_inventory_used = false;
        Bytes response;
        const auto status = uart_transaction(uhf_frame(0x28), response);
        if (status != OK || response.size() != 8 || response[1] != 1 || response[2] != 0x28 || response[5] != 0) {
            peripheral_changed(2, false, "jrd4035", {});
            return IO_ERROR;
        }
        return OK;
    }
    if (channel != 3) return INVALID_ARGUMENT;
    clear_usb_reads();
    if (usb_quarantined) return IO_ERROR;
    if (usb && original_mode >= 0) {
        Bytes acknowledgement, reply;
        bool restored = usb_transaction(chameleon_frame(1001, {uint8_t(original_mode)}), acknowledgement) == OK;
        restored = restored && usb_transaction(chameleon_frame(1002), reply) == OK &&
            release_mode_verified(acknowledgement, reply, uint8_t(original_mode));
        if (!restored) {
            usb_quarantined = true; usb_commands.clear();
            peripheral_changed(3, false, "chameleon", {});
            return IO_ERROR;
        }
    }
    return OK;
}
Status operate(const Frame& f, Bytes& out) {
    OriginScope routing(f);
    if (operation_session != f.session) {
        uhf_guard.clear(); clear_usb_reads(); operation_session = f.session;
    }
    if (stop_operation) return CANCELLED;
    if (f.channel == 1) {
        if (!hf) return UNAVAILABLE;
        Status result = hf_operation(f, out);
        hf->mifareClassicStopCrypto1(); hf->turnOffAntenna();
        if (result == IO_ERROR) { fingerprint = 0; peripheral_changed(1, false, "unknown", {}); }
        return result;
    }
    if (f.op != 32) return UNSUPPORTED;
    auto status = f.channel == 2 ? validate_uhf(f.payload) : validate_chameleon(f.payload);
    if (status != OK) return status;
    if (f.channel == 2 && f.payload[2] == 0x49) {
        if (!uhf_guard.writable()) return PERMISSION_DENIED;
        Bytes check, query = uhf_frame(0x0b);
        status = uart_transaction(query, check);
        if (status != OK) { uhf_guard.clear(); return status; }
        uhf_guard.observe(query, check);
        if (!uhf_guard.writable()) return PERMISSION_DENIED;
        Bytes read(f.payload.begin() + 5, f.payload.begin() + 9);
        read.insert(read.end(), {1, 0, 1, 0, 1});
        status = uart_transaction(uhf_frame(0x39, read), check);
        std::fill(read.begin(), read.end(), 0);
        if (status != OK) return status;
        const auto& selected = uhf_guard.identity();
        size_t id_size = selected.size() - 7;
        if (check.size() != id_size + 10 || check[2] != 0x39 || check[5] != id_size ||
            memcmp(check.data() + 6, selected.data() + 7, id_size) ||
            memcmp(check.data() + 6 + id_size, selected.data() + 7, 2)) return STALE_DATA;
        if (stop_operation) return CANCELLED;
    }
    if (f.channel == 2 && f.payload[2] == 0x22) uhf_inventory_used = true;
    status = f.channel == 2 ? uart_transaction(f.payload, out) : usb_guarded_transaction(f, out);
    if (f.channel == 2 && f.payload[2] == 0x28 && status == OK && out.size() == 8 && out[5] == 0)
        uhf_inventory_used = false;
    if (f.channel == 2) {
        if (status == OK) uhf_guard.observe(f.payload, out);
        else uhf_guard.clear();
    }
    if (status == TIMEOUT || status == IO_ERROR) {
        peripheral_changed(f.channel, false, f.channel == 2 ? "jrd4035" : "chameleon", {});
        if ((f.channel == 2 && f.payload[2] == 0x49) ||
            (f.channel == 3 && f.payload[2] == (3016 >> 8) && f.payload[3] == (3016 & 255))) return WRITE_UNVERIFIED;
    }
    return status;
}
}
