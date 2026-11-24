#pragma once
using SemaphoreHandle_t = void*;
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
