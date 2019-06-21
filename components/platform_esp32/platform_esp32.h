#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "bt_app_core.h"
#include "perf_trace.h"
#include "esp_pthread.h"
#ifndef QUOTE
#define QUOTE(name) #name
#define STR(macro)  QUOTE(macro)
#endif
extern void run_command(char * line);
extern bool wait_for_wifi();

//typedef struct {
//	char opt_slimproto_logging[11];
//	char opt_stream_logging[11];
//	char opt_decode_logging[11];
//	char opt_output_logging[11];
//	char opt_player_name[11];
//	char opt_output_rates[21];
//	char opt_buffer[11];
//} str_squeezelite_options ;
extern void console_start();
extern pthread_cond_t wifi_connect_suspend_cond;
extern pthread_t wifi_connect_suspend_mutex;
//static const char * art_wifi[]={
//		"\n",
//		"o          `O ooOoOOo OOooOoO ooOoOOo\n",
//		"O           o    O    o          O   \n",
//		"o           O    o    O          o   \n",
//		"O           O    O    oOooO      O   \n",
//		"o     o     o    o    O          o   \n",
//		"O     O     O    O    o          O   \n",
//		"`o   O o   O'    O    o          O   \n",
//		" `OoO' `OoO'  ooOOoOo O'      ooOOoOo\n",
//		"\n",
//		""
//};
//static const char * art_wifi_connecting[]={
//		" .oOOOo.",
//		".O     o                                        o               \n",
//		"o                                           O                   \n",
//		"o                                          oOo                  \n",
//		"o         .oOo. 'OoOo. 'OoOo. .oOo. .oOo    o   O  'OoOo. .oOoO \n",
//		"O         O   o  o   O  o   O OooO' O       O   o   o   O o   O \n",
//		"`o     .o o   O  O   o  O   o O     o       o   O   O   o O   o \n",
//		" `OoooO'  `OoO'  o   O  o   O `OoO' `OoO'   `oO o'  o   O `OoOo \n",
//		"                                                              O \n",
//		"                                                           OoO' \n",
//		"\n",
//		""
//};
//static const char * art_wifi_connected[]={
//		" .oOOOo.                                                   o       oO\n",
//		".O     o                                                  O        OO\n",
//		"o                                           O             o        oO\n",
//		"o                                          oOo            o        Oo\n",
//		"o         .oOo. 'OoOo. 'OoOo. .oOo. .oOo    o   .oOo. .oOoO        oO\n",
//		"O         O   o  o   O  o   O OooO' O       O   OooO' o   O          \n",
//		"`o     .o o   O  O   o  O   o O     o       o   O     O   o        Oo\n",
//		" `OoooO'  `OoO'  o   O  o   O `OoO' `OoO'   `oO `OoO' `OoO'o       oO\n",
//		"\n",
//		""
//};
#define ESP_LOG_DEBUG_EVENT(tag,e) ESP_LOGD(tag,"evt: " e)
const char *loc_logtime(void);
//#define MY_ESP_LOG
#ifdef MY_ESP_LOG
#ifdef ESP_LOGI
#undef ESP_LOGI
#define ESP_LOGI(tag, format, ... ) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO,    tag, "%s %d " format,loc_logtime(),  __LINE__, ##__VA_ARGS__)
#endif
#ifdef ESP_LOGE
#undef ESP_LOGE
#define ESP_LOGE(tag, format, ... ) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR,    tag, "%s %d " format,loc_logtime(),  __LINE__, ##__VA_ARGS__)
#endif
#ifdef ESP_LOGW
#undef ESP_LOGW
#define ESP_LOGW(tag, format, ... ) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN,    tag, "%s %d " format,loc_logtime(),  __LINE__, ##__VA_ARGS__)
#endif
#ifdef ESP_LOGD
#undef ESP_LOGD
#define ESP_LOGD(tag, format, ... ) ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG,    tag, "%s %d " format,loc_logtime(),  __LINE__, ##__VA_ARGS__)
#endif
#ifdef ESP_LOGV
#undef ESP_LOGV
#define ESP_LOGV(tag, format, ... ) ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, tag, "%s %d " format,loc_logtime(),  __LINE__, ##__VA_ARGS__)
#endif
#endif
#ifdef __cplusplus
}
#endif
