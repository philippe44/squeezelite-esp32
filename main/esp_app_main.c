/* 
 *  Squeezelite for esp32
 *
 *  (c) Sebastien 2019
 *      Philippe G. 2019, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "platform_esp32.h"
#include "led.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/netdb.h"

#include "http_server.h"
#include "wifi_manager.h"
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
#define JOIN_TIMEOUT_MS (10000)

static const char TAG[] = "esp_app_main";


#ifdef CONFIG_SQUEEZEAMP
#define LED_GREEN_GPIO 	12
#define LED_RED_GPIO	13
#else
#define LED_GREEN_GPIO 	0
#define LED_RED_GPIO	0
#endif

/* brief this is an exemple of a callback that you can setup in your own app to get notified of wifi manager event */
void cb_connection_got_ip(void *pvParameter){
	ESP_LOGI(TAG, "I have a connection!");
	xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
	led_unpush(LED_GREEN);
}
void cb_connection_sta_disconnected(void *pvParameter){
	led_blink_pushed(LED_GREEN, 250, 250);
	xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
}
bool wait_for_wifi(){
	bool connected=(xEventGroupGetBits(wifi_event_group) & CONNECTED_BIT)!=0;
	if(!connected){
		ESP_LOGD(TAG,"Waiting for WiFi...");
	    connected = (xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
	                                   pdFALSE, pdTRUE, JOIN_TIMEOUT_MS / portTICK_PERIOD_MS)& CONNECTED_BIT)!=0;
	    if(!connected){
	    	ESP_LOGW(TAG,"wifi timeout.");
	    }
	    else
	    {
	    	ESP_LOGI(TAG,"WiFi Connected!");
	    }
	}


    return connected;

}

void app_main()
{
	led_config(LED_GREEN, LED_GREEN_GPIO, 0);
	led_config(LED_RED, LED_RED_GPIO, 0);
	wifi_event_group = xEventGroupCreate();
	
	/* start the wifi manager */
	led_blink(LED_GREEN, 250, 250);
	wifi_manager_start();
	wifi_manager_set_callback(EVENT_STA_GOT_IP, &cb_connection_got_ip);
	wifi_manager_set_callback(WIFI_EVENT_STA_DISCONNECTED, &cb_connection_sta_disconnected);


	console_start();
}
