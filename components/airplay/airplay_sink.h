/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef __AIRPLAY_SINK_H__
#define __AIRPLAY_SINK_H__

#include <stdint.h>

//typedef enum { 	BT_SINK_CONNECTED, BT_SINK_DISCONNECTED, BT_SINK_PLAY, BT_SINK_STOP, BT_SINK_PAUSE, 
				//BT_SINK_RATE, BT_SINK_VOLUME,  } bt_sink_cmd_t;

/**
 * @brief     init sink mode (need to be provided)
 */
void airplay_sink_init(void);

#endif /* __AIRPLAY_SINK_H__*/