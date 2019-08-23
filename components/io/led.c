/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "led.h"

#define MAX_LED	8
#define BLOCKTIME	10	// up to portMAX_DELAY

static struct led_s {
	gpio_num_t gpio;
	bool on;
	int onstate;
	int ontime, offtime;
	int pushedon, pushedoff;
	bool pushed;
	TimerHandle_t timer;
} leds[MAX_LED];

static void vCallbackFunction( TimerHandle_t xTimer ) {
	struct led_s *led = (struct led_s*) pvTimerGetTimerID (xTimer);
	
	if (!led->timer) return;
	
	led->on = !led->on;
	gpio_set_level(led->gpio, led->on ? led->onstate : !led->onstate);
	
	// was just on for a while
	if (!led->on && led->offtime == -1) return;
	
	// regular blinking
	xTimerChangePeriod(xTimer, (led->on ? led->ontime : led->offtime) / portTICK_RATE_MS, BLOCKTIME);
}

bool led_blink_core(int idx, int ontime, int offtime, bool pushed) {
	if (!leds[idx].gpio) return false;
	
	if (leds[idx].timer) {
		// normal requests waits if a pop is pending
		if (!pushed && leds[idx].pushed) {
			leds[idx].pushedon = ontime; 
			leds[idx].pushedoff = offtime; 
			return true;
		}
		xTimerStop(leds[idx].timer, BLOCKTIME);
	}
	
	// save current state if not already pushed
	if (!leds[idx].pushed) {
		leds[idx].pushedon = leds[idx].ontime;
		leds[idx].pushedoff = leds[idx].offtime;	
		leds[idx].pushed = pushed;
	}	
	
	// then set new one
	leds[idx].ontime = ontime;
	leds[idx].offtime = offtime;	
			
	if (ontime == 0) {
		gpio_set_level(leds[idx].gpio, !leds[idx].onstate);
	} else if (offtime == 0) {
		gpio_set_level(leds[idx].gpio, leds[idx].onstate);
	} else {
		if (!leds[idx].timer) leds[idx].timer = xTimerCreate("ledTimer", ontime / portTICK_RATE_MS, pdFALSE, (void *)&leds[idx], vCallbackFunction);
        leds[idx].on = true;
		gpio_set_level(leds[idx].gpio, leds[idx].onstate);
		if (xTimerStart(leds[idx].timer, BLOCKTIME) == pdFAIL) return false;
	}
	
	return true;
} 

bool led_unpush(int idx) {
	if (!leds[idx].gpio) return false;
	
	led_blink_core(idx, leds[idx].pushedon, leds[idx].pushedoff, true);
	leds[idx].pushed = false;
	
	return true;
}	

bool led_config(int idx, gpio_num_t gpio, int onstate) {
	if (idx >= MAX_LED) return false;
	leds[idx].gpio = gpio;
	leds[idx].onstate = onstate;
	
	gpio_pad_select_gpio(gpio);
	gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
	gpio_set_level(gpio, !onstate);
	
	return true;
}

bool led_unconfig(int idx) {
	if (idx >= MAX_LED) return false;	
	
	if (leds[idx].timer) xTimerDelete(leds[idx].timer, BLOCKTIME);
	leds[idx].timer = NULL;
	
	return true;
}
