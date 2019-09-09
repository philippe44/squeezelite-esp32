#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "mdns.h"
#include "nvs.h"
#include "tcpip_adapter.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "freertos/timers.h"

#include "raop.h"

#include "log_util.h"

#include "trace.h"

#ifndef CONFIG_AIRPLAY_NAME
#define CONFIG_AIRPLAY_NAME		"ESP32-AirPlay"
#endif

static const char * TAG = "platform";
extern char current_namespace[];

log_level	raop_loglevel = lINFO;
log_level	util_loglevel;

static log_level *loglevel = &raop_loglevel;
static struct raop_ctx_s *raop;

/****************************************************************************************
 * Airplay sink de-initialization
 */
void raop_sink_deinit(void) {
	raop_delete(raop);
	mdns_free();
}	

/****************************************************************************************
 * Airplay sink initialization
 */
void raop_sink_init(raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb) {
    const char *hostname;
	char sink_name[64-6] = CONFIG_AIRPLAY_NAME;
	nvs_handle nvs;
	tcpip_adapter_ip_info_t ipInfo; 
	struct in_addr host;
   	
	// get various IP info
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
	tcpip_adapter_get_hostname(TCPIP_ADAPTER_IF_STA, &hostname);
	host.s_addr = ipInfo.ip.addr;

    // initialize mDNS
    ESP_ERROR_CHECK( mdns_init() );
    ESP_ERROR_CHECK( mdns_hostname_set(hostname) );
        
    if (nvs_open(current_namespace, NVS_READONLY, &nvs) == ESP_OK) {
		size_t len = sizeof(sink_name) - 1;
		nvs_get_str(nvs, "airplay_name", sink_name, &len);
		nvs_close(nvs);
	}	
	
	ESP_LOGI(TAG, "mdns hostname set to: [%s] with servicename %s", hostname, sink_name);

    // create RAOP instance, latency is set by controller
	uint8_t mac[6];	
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
	raop = raop_create(host, sink_name, mac, 0, cmd_cb, data_cb);
}

/****************************************************************************************
 * Airplay local command (stop, start, volume ...)
 */
void raop_sink_cmd(raop_event_t event, void *param) {
	raop_cmd(raop, event, param);
}
