#pragma once
#include "ble_hs.h"
struct ble_store_status_event { int event_code; };
int ble_store_util_bonded_peers(ble_addr_t*,int*,int);
int ble_store_util_delete_peer(const ble_addr_t*);
