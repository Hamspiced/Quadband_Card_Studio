#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { BLE_SM_IOACT_INPUT = 1, BLE_SM_IOACT_DISP = 2, BLE_SM_IOACT_NUMCMP = 3 };
struct ble_sm_io { uint8_t action; uint32_t passkey; bool numcmp_accept; };
enum { BLE_HS_IO_KEYBOARD_DISPLAY=4, BLE_SM_PAIR_KEY_DIST_ENC=1, BLE_SM_PAIR_KEY_DIST_ID=2 };
int ble_sm_inject_io(uint16_t, const struct ble_sm_io*);
#ifdef __cplusplus
}
#endif
