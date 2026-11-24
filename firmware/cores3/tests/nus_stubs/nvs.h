#pragma once
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
using nvs_handle_t = unsigned;
using esp_err_t = int;
enum { ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, NVS_READWRITE = 1 };
#define ESP_ERROR_CHECK(call) assert((call)==ESP_OK)
int nvs_open(const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_erase_key(nvs_handle_t,const char*);
int nvs_get_u8(nvs_handle_t,const char*,uint8_t*);
int nvs_set_u8(nvs_handle_t,const char*,uint8_t);
int nvs_commit(nvs_handle_t);
