#pragma once
#include "bridge.hpp"
#include "workbench.hpp"
#include <atomic>
#include <mutex>

namespace qb {
extern std::mutex state_mutex;
extern std::array<Slot, 3> slots;
extern Session session;
extern std::atomic<bool> stop_operation;
extern std::atomic<uint32_t> pairing_code;
extern std::atomic<uint64_t> pairing_challenge;
extern std::atomic<int64_t> pairing_until;
extern std::atomic<bool> link_authorized;
extern std::atomic<bool> pairing_pending;
extern std::atomic<bool> usb_host_approved;
extern std::atomic<UsbHostState> usb_host_state;
void hardware_init();
void hardware_poll();
Status hardware_release();
Status hardware_release_channel(uint8_t channel);
bool hardware_command_supported(uint8_t channel, uint16_t command);
void hardware_local_uhf_handler(std::function<void(const Bytes&)> handler);
bool local_operation_allowed(uint64_t epoch);
bool remote_event_allowed(uint64_t nonce, uint32_t generation);
Status operate(const Frame&, Bytes&);
void ble_init();
void ble_disconnect();
void ble_send(const Frame&);
void ble_pair_approve(bool, uint64_t expected_challenge = 0);
void ble_clear_bonds();
void receive(const uint8_t*, size_t);
void disconnected();
void peripheral_invalidate(unsigned);
void peripheral_changed(unsigned, bool, const std::string&, const std::vector<std::string>&);
std::string capabilities();
std::string hex(const uint8_t*, size_t);
}
