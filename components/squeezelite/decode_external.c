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
#include "bt_app_sink.h"
#include "raop_sink.h"

#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)
#define LOCK_D   mutex_lock(decode.mutex);
#define UNLOCK_D mutex_unlock(decode.mutex);

extern struct outputstate output;
extern struct decodestate decode;
extern struct buffer *outputbuf;
// this is the only system-wide loglevel variable
extern log_level loglevel;

static raop_event_t	raop_state;
static bool raop_expect_stop = false;

/****************************************************************************************
 * Common sink data handler
 */
static void sink_data_handler(const uint8_t *data, uint32_t len)
{
    size_t bytes, space;
		
	// would be better to lock decoder, but really, it does not matter
	if (decode.state != DECODE_STOPPED) {
		LOG_SDEBUG("Cannot use external sink while LMS is controlling player");
		return;
	} 
	
	// there will always be room at some point
	while (len) {
		LOCK_O;
		
		bytes = min(len, _buf_cont_write(outputbuf));
#if BYTES_PER_FRAME == 4
		memcpy(outputbuf->writep, data, bytes);
#else
		{
			s16_t *iptr = (s16_t*) data;
			ISAMPLE_T *optr = (ISAMPLE_T*) outputbuf->writep;
			size_t n = bytes / BYTES_PER_FRAME * 2;
			while (n--) *optr++ = *iptr++ << 16;
		}
#endif	
		_buf_inc_writep(outputbuf, bytes);
		space = _buf_space(outputbuf);
		
		len -= bytes;
		data += bytes;
				
		UNLOCK_O;
		
		// allow i2s to empty the buffer if needed
		if (len && !space) usleep(50000);
	}	
}

/****************************************************************************************
 * BT sink command handler
 */
static void bt_sink_cmd_handler(bt_sink_cmd_t cmd, ...) 
{
	va_list args;
	
	LOCK_D;
	
	if (decode.state != DECODE_STOPPED) {
		LOG_WARN("Cannot use BT sink while LMS is controlling player");
		UNLOCK_D;
		return;
	} 	
	
	va_start(args, cmd);
	
	if (cmd != BT_SINK_VOLUME) LOCK_O;
		
	switch(cmd) {
	case BT_SINK_CONNECTED:
		output.external = true;
		output.state = OUTPUT_STOPPED;
		LOG_INFO("BT sink started");
		break;
	case BT_SINK_DISCONNECTED:	
		output.external = false;
		output.state = OUTPUT_OFF;
		LOG_INFO("BT sink stopped");
		break;
	case BT_SINK_PLAY:
		output.state = OUTPUT_RUNNING;
		LOG_INFO("BT sink playing");
		break;
	case BT_SINK_PAUSE:		
	case BT_SINK_STOP:
		output.state = OUTPUT_STOPPED;
		LOG_INFO("BT sink stopped");
		break;
	case BT_SINK_RATE:
		output.current_sample_rate = va_arg(args, u32_t);
		LOG_INFO("Setting BT sample rate %u", output.current_sample_rate);
		break;
	case BT_SINK_VOLUME: {
		u16_t volume = (u16_t) va_arg(args, u32_t);
		volume *= 65536 / 128;
		set_volume(volume, volume);
		break;
	}
	}
	
	if (cmd != BT_SINK_VOLUME) UNLOCK_O;
	UNLOCK_D;

	va_end(args);
}

/****************************************************************************************
 * AirPlay sink command handler
 */
void raop_sink_cmd_handler(raop_event_t event, void *param)
{
	LOCK_D;
	
	if (decode.state != DECODE_STOPPED) {
		LOG_WARN("Cannot use Airplay sink while LMS is controlling player");
		UNLOCK_D;
		return;
	} 	
	
	if (event != RAOP_VOLUME) LOCK_O;
	
	// this is async, so player might have been deleted
	switch (event) {
		case RAOP_STREAM:
			// a PLAY will come later, so we'll do the load at that time
			LOG_INFO("Stream", NULL);
			raop_state = event;
			output.external = true;
			output.current_sample_rate = 44100;
			output.state = OUTPUT_BUFFER;
			output.threshold = 5;
			break;
		case RAOP_STOP:
			LOG_INFO("Stop", NULL);
			output.external = false;
			output.state = OUTPUT_OFF;
			raop_state = event;
			break;
		case RAOP_FLUSH:
			LOG_INFO("Flush", NULL);
			raop_expect_stop = true;
			raop_state = event;
			output.state = OUTPUT_STOPPED;
			break;
		case RAOP_PLAY: {
			LOG_INFO("Play", NULL);
			// this where we need the OUTPUT_START_AT
			if (raop_state != RAOP_PLAY) {
				output.external = true;
				output.state = OUTPUT_RUNNING;
			}
			raop_state = event;
			break;
		}
		case RAOP_VOLUME: {
			float volume = *((float*) param);
			LOG_INFO("Volume[0..1] %0.4f", volume);
			volume *= 65536;
			set_volume((u16_t) volume, (u16_t) volume);
			break;
		}
		default:
			break;
	}
	
	if (event != RAOP_VOLUME) UNLOCK_O;
	
	UNLOCK_D;
}

/****************************************************************************************
 * We provide the generic codec register option
 */
void register_other(void) {
#ifdef CONFIG_BT_SINK	
	if (!strcasestr(output.device, "BT ")) {
		bt_sink_init(bt_sink_cmd_handler, sink_data_handler);
		LOG_INFO("Initializing BT sink");
	} else {
		LOG_WARN("Cannot be a BT sink and source");
	}	
#endif	
#ifdef CONFIG_AIRPLAY_SINK
	if (!strcasestr(output.device, "BT ")) {
		raop_sink_init(raop_sink_cmd_handler, sink_data_handler);
		LOG_INFO("Initializing AirPlay sink");		
	} else {
		LOG_WARN("Cannot be an AirPlay sink and BT source");
	}	
#endif
}
