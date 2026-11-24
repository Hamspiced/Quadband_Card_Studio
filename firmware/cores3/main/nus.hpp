#pragma once
#include "ultra_link.hpp"
extern "C" {
struct ble_gap_conn_desc;
}
namespace qb {
void nus_init();
void nus_snapshot(UltraView&);
bool nus_scan();
bool nus_select(uint64_t scan_epoch, size_t index);
void nus_cancel();
bool nus_idle();
bool nus_input(uint64_t epoch, uint64_t challenge, const char* digits);
bool nus_subscribed();
void nus_validated(bool);
Status nus_transaction(const Bytes&, Bytes&);
// Called only by the peripheral worker, never from a radio callback.
UltraTransport ultra_transport();
Status hardware_select_transport(UltraTransport);
bool ble_role_allowed(const ::ble_gap_conn_desc&, bool central, bool persist = false);
}
