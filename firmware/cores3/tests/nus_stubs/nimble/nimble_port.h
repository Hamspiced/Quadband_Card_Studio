#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct ble_npl_event { void (*callback)(struct ble_npl_event*); };
void ble_npl_event_init(struct ble_npl_event*, void (*)(struct ble_npl_event*), void*);
int nimble_port_init(void);
void nimble_port_run(void);
void* nimble_port_get_dflt_eventq(void);
void ble_npl_eventq_put(void*, struct ble_npl_event*);
#ifdef __cplusplus
}
#endif
