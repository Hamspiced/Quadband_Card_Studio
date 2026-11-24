#pragma once
#include <stddef.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
using QueueHandle_t = void*;
QueueHandle_t xQueueCreate(size_t, size_t);
int xQueueSend(QueueHandle_t, const void*, unsigned);
int xQueueReceive(QueueHandle_t, void*, unsigned);
void xQueueReset(QueueHandle_t);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
void vTaskDelay(unsigned);

#define portMAX_DELAY 0xffffffffu
#define pdPASS 1
int xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, void*);
