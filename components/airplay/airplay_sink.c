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
#include "airplay_sink.h"

#include "trace.h"

static const char * TAG = "platform";
extern char current_namespace[];

void airplay_sink_init(void) {
    const char *hostname;
	char *airplay_name, sink_name[32] = CONFIG_AIRPLAY_NAME;
	nvs_handle nvs;
				
	tcpip_adapter_get_hostname(TCPIP_ADAPTER_IF_STA, &hostname);

    //initialize mDNS
    ESP_ERROR_CHECK( mdns_init() );
    ESP_ERROR_CHECK( mdns_hostname_set(hostname) );
        
    //structure with TXT records
    mdns_txt_item_t serviceTxtData[] = {
		{"am", "esp32"},
		{"tp", "UDP"},
		{"sm","false"}, 
		{"sv","false"}, 
		{"ek","1"},
		{"et","0,1"},
		{"md","0,1,2"},
		{"cn","0,1"},
		{"ch","2"},
		{"ss","16"},
		{"sr","44100"},
		{"vn","3"},
		{"txtvers","1"},
	};
	
	if (nvs_open(current_namespace, NVS_READONLY, &nvs) == ESP_OK) {
		size_t len = 31;
		nvs_get_str(nvs, "airplay_sink_name", sink_name, &len);
		nvs_close(nvs);
	}	
	
	// AirPlay wants mDNS name to be MAC@name
	uint8_t mac[6];	
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    asprintf(&airplay_name, "%02X%02X%02X%02X%02X%02X@%s",  mac[3], mac[4], mac[5], mac[3], mac[4], mac[5], sink_name);
	
	ESP_LOGI(TAG, "mdns hostname set to: [%s] with servicename %s", hostname, sink_name);

    //initialize service
    ESP_ERROR_CHECK( mdns_service_add(airplay_name, "_raop", "_tcp", 6000, serviceTxtData, sizeof(serviceTxtData) / sizeof(mdns_txt_item_t)) );
	free(airplay_name);
}
