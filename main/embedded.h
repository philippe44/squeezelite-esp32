#pragma once
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_pthread.h"
#define PTHREAD_SET_NAME(n) 	{ esp_pthread_cfg_t cfg = esp_pthread_get_default_config(); cfg.thread_name= n; cfg.inherit_cfg = true; esp_pthread_set_cfg(&cfg); }
#endif
