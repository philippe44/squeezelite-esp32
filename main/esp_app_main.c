/* Scan Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
    This example shows how to use the All Channel Scan or Fast Scan to connect
    to a Wi-Fi network.

    In the Fast Scan mode, the scan will stop as soon as the first network matching
    the SSID is found. In this mode, an application can set threshold for the
    authentication mode and the Signal strength. Networks that do not meet the
    threshold requirements will be ignored.

    In the All Channel Scan mode, the scan will end only after all the channels
    are scanned, and connection will start with the best network. The networks
    can be sorted based on Authentication Mode or Signal Strength. The priority
    for the Authentication mode is:  WPA2 > WPA > WEP > Open
*/
#include "squeezelite.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "sys/socket.h"
#include "string.h"
thread_cond_type wifi_connect_suspend_cond;
mutex_type wifi_connect_suspend_mutex;
char * art_wifi[]={
		"\n",
		"o          `O ooOoOOo OOooOoO ooOoOOo\n",
		"O           o    O    o          O   \n",
		"o           O    o    O          o   \n",
		"O           O    O    oOooO      O   \n",
		"o     o     o    o    O          o   \n",
		"O     O     O    O    o          O   \n",
		"`o   O o   O'    O    o          O   \n",
		" `OoO' `OoO'  ooOOoOo O'      ooOOoOo\n",
		"\n",
		""
};
char * art_wifi_connecting[]={
		" .oOOOo.",
		".O     o                                        o               \n",
		"o                                           O                   \n",
		"o                                          oOo                  \n",
		"o         .oOo. 'OoOo. 'OoOo. .oOo. .oOo    o   O  'OoOo. .oOoO \n",
		"O         O   o  o   O  o   O OooO' O       O   o   o   O o   O \n",
		"`o     .o o   O  O   o  O   o O     o       o   O   O   o O   o \n",
		" `OoooO'  `OoO'  o   O  o   O `OoO' `OoO'   `oO o'  o   O `OoOo \n",
		"                                                              O \n",
		"                                                           OoO' \n",
		"\n",
		""
};
char * art_wifi_connected[]={
		" .oOOOo.                                                   o       oO\n",
		".O     o                                                  O        OO\n",
		"o                                           O             o        oO\n",
		"o                                          oOo            o        Oo\n",
		"o         .oOo. 'OoOo. 'OoOo. .oOo. .oOo    o   .oOo. .oOoO        oO\n",
		"O         O   o  o   O  o   O OooO' O       O   OooO' o   O          \n",
		"`o     .o o   O  O   o  O   o O     o       o   O     O   o        Oo\n",
		" `OoooO'  `OoO'  o   O  o   O `OoO' `OoO'   `oO `OoO' `OoO'o       oO\n",
		"\n",
		""
};

/*Set the SSID and Password via "make menuconfig"*/
#define DEFAULT_SSID CONFIG_WIFI_SSID
#define DEFAULT_PWD CONFIG_WIFI_PASSWORD

#if CONFIG_WIFI_ALL_CHANNEL_SCAN
#define DEFAULT_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#elif CONFIG_WIFI_FAST_SCAN
#define DEFAULT_SCAN_METHOD WIFI_FAST_SCAN
#else
#define DEFAULT_SCAN_METHOD WIFI_FAST_SCAN
#endif /*CONFIG_SCAN_METHOD*/

#if CONFIG_WIFI_CONNECT_AP_BY_SIGNAL
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#elif CONFIG_WIFI_CONNECT_AP_BY_SECURITY
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SECURITY
#else
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#endif /*CONFIG_SORT_METHOD*/

#if CONFIG_FAST_SCAN_THRESHOLD
#define DEFAULT_RSSI CONFIG_FAST_SCAN_MINIMUM_SIGNAL
#if CONFIG_EXAMPLE_OPEN
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#elif CONFIG_EXAMPLE_WEP
#define DEFAULT_AUTHMODE WIFI_AUTH_WEP
#elif CONFIG_EXAMPLE_WPA
#define DEFAULT_AUTHMODE WIFI_AUTH_WPA_PSK
#elif CONFIG_EXAMPLE_WPA2
#define DEFAULT_AUTHMODE WIFI_AUTH_WPA2_PSK
#else
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#endif
#else
#define DEFAULT_RSSI -127
#define DEFAULT_AUTHMODE WIFI_AUTH_OPEN
#endif /*CONFIG_FAST_SCAN_THRESHOLD*/

static const char *TAG = "scan";

static void event_handler(void* arg, esp_event_base_t event_base, 
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip: %s.", ip4addr_ntoa(&event->ip_info.ip));
        logprint("Signaling wifi connected. Locking.\n");
    	mutex_lock(wifi_connect_suspend_mutex);
    	logprint("Signaling wifi connected. Broadcasting.\n");
		mutex_broadcast_cond(wifi_connect_suspend_cond);
    	logprint("Signaling wifi connected. Unlocking.\n");
		mutex_unlock(wifi_connect_suspend_mutex);
    }
}


/* Initialize Wi-Fi as sta and set scan method */
static void wifi_scan(void)
{
	for(uint8_t l=0;art_wifi[l][0]!='\0';l++){
		logprint("%s",art_wifi[l]);
	}
	for(uint8_t l=0;art_wifi_connecting[l][0]!='\0';l++){
		logprint("%s",art_wifi_connecting[l]);
		}
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DEFAULT_SSID,
            .password = DEFAULT_PWD,
            .scan_method = DEFAULT_SCAN_METHOD,
            .sort_method = DEFAULT_SORT_METHOD,
            .threshold.rssi = DEFAULT_RSSI,
            .threshold.authmode = DEFAULT_AUTHMODE,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

int main(int argc, char**argv);


#define DO_EXPAND(VAL)  VAL ## 1
#define EXPAND(VAL)     DO_EXPAND(VAL)
void app_main()
{
	int i; 
	char **argv, *_argv[] = {
		"squeezelite-esp32",
		"-C",
		"1",
		"-o",
		CONFIG_OUTPUT_NAME,
		"-n",
		"ESP32",
		"-r",
		"OUTPUT_RATES",
		"-d",
		"slimproto=" CONFIG_LOGGING_SLIMPROTO,
		"-d",
		"stream=" CONFIG_LOGGING_STREAM,
		"-d",
		"decode=" CONFIG_LOGGING_DECODE,
		"-d",
		"output=" CONFIG_LOGGING_OUTPUT,
		"-b",
		"500:2000"

	};


	// can't do strtok on FLASH strings
	argv = malloc(sizeof(_argv));
	for (i = 0; i < sizeof(_argv)/sizeof(char*); i++) {
		argv[i] = strdup(_argv[i]);
	}


    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

	mutex_create_p(wifi_connect_suspend_mutex);
	mutex_cond_init(wifi_connect_suspend_cond);

	logprint("Starting WiFi.\n");
	wifi_scan();
	logprint("Waiting for WiFi to connect. Locking Mutex.\n");
	mutex_lock(wifi_connect_suspend_mutex);
	logprint("Waiting for WiFi to connect. cond_wait.\n");
	pthread_cond_wait(&wifi_connect_suspend_cond,&wifi_connect_suspend_mutex);
	logprint("Waiting for WiFi to connect. Unlocking Mutex.\n");
	mutex_unlock(wifi_connect_suspend_mutex);
	for(uint8_t l=0;art_wifi[l][0]!='\0';l++){
		logprint("%s",art_wifi[l]);
	}
	for(uint8_t l=0;art_wifi_connected[l][0]!='\0';l++){
		logprint("%s",art_wifi_connected[l]);
		}
	logprint("%s %s:%d Calling main with parameters: " , logtime(), __FUNCTION__, __LINE__);

	for (i = 0; i < sizeof(_argv)/sizeof(char*); i++) {
		logprint("%s " , _argv[i]);
	}
	logprint("\n");

	main(sizeof(_argv)/sizeof(char*), argv);
}
