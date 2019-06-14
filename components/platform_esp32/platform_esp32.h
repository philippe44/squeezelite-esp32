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
#define ESP_LOG_DEBUG_EVENT(tag,e) ESP_LOGD(tag,"evt: " QUOTE(e))
typedef struct {
	char * optName;
	char * cmdLinePrefix;
	char * description;
	char * defaultValue;
	char * relatedcommand;
} optListStruct;
optListStruct * getOptionByName(char * option);
//static optListStruct optList[] = {
//	{
//		.optName= "log_slimproto",
//		.cmdLinePrefix="-d slimproto=",
//		.description="Slimproto Logging Level info|debug|sdebug",
//		.defaultValue=(CONFIG_LOGGING_SLIMPROTO),
//		.relatedcommand="squeezelite"
//	},
//	{
//		.optName="log_stream",
//		.cmdLinePrefix="-d stream=",
//		.description="Stream Logging Level info|debug|sdebug",
//		.defaultValue=(CONFIG_LOGGING_STREAM),
//		.relatedcommand="squeezelite"
//	},
//	{
//		.optName="log_decode",
//		.cmdLinePrefix="-d decode=",
//		.description="Decode Logging Level info|debug|sdebug",
//		.defaultValue=(CONFIG_LOGGING_DECODE),
//		.relatedcommand="squeezelite"
//	},
//	{
//		.optName="log_output",
//		.cmdLinePrefix="-d output=",
//		.description="Output Logging Level info|debug|sdebug",
//		.defaultValue=(CONFIG_LOGGING_OUTPUT),
//		.relatedcommand="squeezelite"
//	},
//	{
//		.optName="output_rates",
//		.cmdLinePrefix="-r ",
//		.description="Supported rates",
//		.defaultValue=(CONFIG_OUTPUT_RATES),
//		.relatedcommand="squeezelite"
//	},
//	{
//		.optName="output_dev",
//		.cmdLinePrefix="-O",
//		.description="Output device to use. BT for Bluetooth, DAC for i2s DAC.",
//		.defaultValue=(CONFIG_A2DP_SINK_NAME),
//		.relatedcommand=""
//	},
//	{
//		.optName="a2dp_sink_name",
//		.cmdLinePrefix="",
//		.description="Bluetooth sink name to connect to.",
//		.defaultValue=(CONFIG_A2DP_SINK_NAME),
//		.relatedcommand=""
//	},
//	{
//		.optName="a2dp_dev_name",
//		.cmdLinePrefix="",
//		.description="A2DP Device name to use when connecting to audio sink.",
//		.defaultValue=(CONFIG_A2DP_DEV_NAME),
//		.relatedcommand=""
//	},
//	{
//		.optName="a2dp_cntrldelay",
//		.cmdLinePrefix="",
//		.description="Delay (ms) for each pass of the A2DP control loop.",
//		.defaultValue=STR(CONFIG_A2DP_CONTROL_DELAY_MS),
//		.relatedcommand=""
//	},
//	{
//		.optName="a2dp_timeout",
//		.cmdLinePrefix="",
//		.description="Delay (ms) for A2DP timeout on connect.",
//		.defaultValue=STR(CONFIG_A2DP_CONNECT_TIMEOUT_MS),
//		.relatedcommand=""
//	},
//	{
//		.optName="wifi_ssid",
//		.cmdLinePrefix="",
//		.description="WiFi access point name to connect to.",
//		.defaultValue=	(CONFIG_WIFI_SSID),
//		.relatedcommand=""
//	},
//	{
//		.optName="wifi_password",
//		.cmdLinePrefix= "",
//		.description="WiFi access point password.",
//		.defaultValue=(CONFIG_WIFI_PASSWORD),
//		.relatedcommand=""
//	},
//	{}
//};

#ifdef __cplusplus
}
#endif
