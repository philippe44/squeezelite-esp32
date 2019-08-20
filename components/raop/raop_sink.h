/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef RAOP_SINK_H
#define RAOP_SINK_H

#include <stdint.h>

#define RAOP_SAMPLE_RATE	44100

typedef enum { RAOP_SETUP, RAOP_STREAM, RAOP_PLAY, RAOP_FLUSH, RAOP_PAUSE, RAOP_STOP, RAOP_VOLUME, RAOP_TIMING } raop_event_t ;

typedef void (*raop_cmd_cb_t)(raop_event_t event, void *param);
typedef void (*raop_data_cb_t)(const u8_t *data, size_t len);

/**
 * @brief     init sink mode (need to be provided)
 */

void raop_sink_init(raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb);

/**
 * @brief     init sink mode (need to be provided)
 */

void raop_sink_cmd(raop_event_t event, void *param);

#endif /* RAOP_SINK_H*/