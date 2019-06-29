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
#include "squeezelite.h"

extern struct outputstate output;
extern struct buffer *outputbuf;

#define FRAME_BLOCK MAX_SILENCE_FRAMES

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

extern void set_volume_i2s(unsigned left, unsigned right);
extern void output_init_bt(log_level level, char *device, unsigned output_buf_size, char *params, 
						  unsigned rates[], unsigned rate_delay, unsigned idle);
extern void output_init_i2s(log_level level, char *device, unsigned output_buf_size, char *params, 
						  unsigned rates[], unsigned rate_delay, unsigned idle);							  

static log_level loglevel;

static void (*volume_cb)(unsigned left, unsigned right);
static void (*close_cb)(void);

void output_init_embedded(log_level level, char *device, unsigned output_buf_size, char *params, 
						  unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;						
	LOG_INFO("init device: %s", device);
	
	memset(&output, 0, sizeof(output));
	output_init_common(level, device, output_buf_size, rates, idle);
	output.start_frames =  FRAME_BLOCK;
	output.rate_delay = rate_delay;
	
	if (strstr(device, "BT ")) {
		LOG_INFO("init Bluetooth");
		output_init_bt(level, device, output_buf_size, params, rates, rate_delay, idle);
	} else {
		LOG_INFO("init I2S");
		//volume_cb = set_volume_i2s;
		//close_cb = output_close_i2s;
		//output_init_i2s(level, device, output_buf_size, params, rates, rate_delay, idle);
	}	
	
	LOG_INFO("init completed.");
}	

void output_close_embedded(void) {
	LOG_INFO("close output");
	output_close_common();
	if (close_cb) (*close_cb)();		
}

void set_volume(unsigned left, unsigned right) { 
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	if (!volume_cb) {
		LOCK;
		output.gainL = left;
		output.gainR = right;
		UNLOCK;
	} else (*volume_cb)(left, right); 	
}

bool test_open(const char *device, unsigned rates[], bool userdef_rates) {
	memset(rates, 0, MAX_SUPPORTED_SAMPLERATES * sizeof(unsigned));
	if (!strcmp(device, "BT")) {
		rates[0] = 44100;	
	} else {
		unsigned _rates[] = { 96000, 88200, 48000, 44100, 32000, 0 };	
		memcpy(rates, _rates, sizeof(_rates));
	}
	return true;
}

char* output_state_str(void){
	output_state state;
	LOCK;
	state = output.state;
	UNLOCK;
	switch (state) {
	case OUTPUT_OFF: 			return STR(OUTPUT_OFF);
	case OUTPUT_STOPPED:		return STR(OUTPUT_STOPPED);
	case OUTPUT_BUFFER:			return STR(OUTPUT_BUFFER);
	case OUTPUT_RUNNING:		return STR(OUTPUT_RUNNING);
	case OUTPUT_PAUSE_FRAMES: 	return STR(OUTPUT_PAUSE_FRAMES);
	case OUTPUT_SKIP_FRAMES:	return STR(OUTPUT_SKIP_FRAMES);
	case OUTPUT_START_AT:		return STR(OUTPUT_START_AT);
	default:					return "OUTPUT_UNKNOWN_STATE";
	}
}

bool output_stopped(void) {
	output_state state;
	LOCK;
	state = output.state;
	UNLOCK;
	return state <= OUTPUT_STOPPED;
}	
	



