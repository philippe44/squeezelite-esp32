/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef __BT_APP_SINK_H__
#define __BT_APP_SINK_H__

#include <stdint.h>

typedef enum { 	BT_SINK_CONNECTED, BT_SINK_DISCONNECTED, BT_SINK_PLAY, BT_SINK_STOP, BT_SINK_PAUSE, 
				BT_SINK_RATE, BT_SINK_VOLUME,  } bt_sink_cmd_t;
				
typedef void (*bt_cmd_cb_t)(bt_sink_cmd_t cmd, ...);
typedef void (*bt_data_cb_t)(const uint8_t *data, uint32_t len);

/**
 * @brief     init sink mode (need to be provided)
 */
void bt_sink_init(bt_cmd_cb_t cmd_cb, bt_data_cb_t data_cb);

/**
 * @brief     deinit sink mode (need to be provided)
 */
void bt_sink_deinit(void);

/**
 * @brief     local command mode (stop, play, volume ...)
 */
void bt_sink_cmd(bt_sink_cmd_t event, ...);

#endif /* __BT_APP_SINK_H__*/
