#pragma once
#include "esp_err.h"
#include "nvs.h"
#ifdef __cplusplus
extern "C" {
#endif
bool isNameValid(char * key);
esp_err_t store_nvs_value_len(nvs_type_t type, const char *key, void * data, size_t data_len);
esp_err_t store_nvs_value(nvs_type_t type, const char *key, void * data);
esp_err_t get_nvs_value(nvs_type_t type, const char *key, void*value, const uint8_t buf_size);
void * get_nvs_value_alloc(nvs_type_t type, const char *key);
#ifdef __cplusplus
}
#endif
