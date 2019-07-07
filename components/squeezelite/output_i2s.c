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
 
/* 
Synchronisation is a bit of a hack with i2s. The esp32 driver is always
full when it starts, so there is a delay of the total length of buffers.
In other words, i2w_write blocks at first call, until at least one buffer
has been written (it uses a queue with produce / consume).

The first hack is to consume that length at the beginning of tracks when
synchronization is active. It's about ~180ms @ 44.1kHz

The second hack is that we never know exactly the number of frames in the 
DMA buffers when we update the output.frames_played_dmp. We assume that
after i2s_write, these buffers are always full so by measuring the gap
between time after i2s_write and update of frames_played_dmp, we have a
good idea of the error. 

The third hack is when sample rate changes, buffers are reset and we also
do the change too early, but can't do that exaclty at the right time. So 
there might be a pop and a de-sync when sampling rate change happens. Not
sure that using rate_delay would fix that
*/

#include "squeezelite.h"
#include "driver/i2s.h"
#include "perf_trace.h"
#include <signal.h>
#include "time.h"

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES

// Prevent compile errors if dac output is
// included in the build and not actually activated in menuconfig
#ifndef CONFIG_I2S_BCK_IO
#define CONFIG_I2S_BCK_IO -1
#endif
#ifndef CONFIG_I2S_WS_IO
#define CONFIG_I2S_WS_IO -1
#endif
#ifndef CONFIG_I2S_DO_IO
#define CONFIG_I2S_DO_IO -1
#endif
#ifndef CONFIG_I2S_NUM
#define CONFIG_I2S_NUM -1
#endif

// must have an integer ratio with FRAME_BLOCK
#define DMA_BUF_LEN		512	
#define DMA_BUF_COUNT	16

#define DECLARE_ALL_MIN_MAX 	\
	DECLARE_MIN_MAX(o); 		\
	DECLARE_MIN_MAX(s); 		\
	DECLARE_MIN_MAX(rec); 		\
	DECLARE_MIN_MAX(i2s_time); 	\
	DECLARE_MIN_MAX(buffering);

#define RESET_ALL_MIN_MAX 		\
	RESET_MIN_MAX(o); 			\
	RESET_MIN_MAX(s); 			\
	RESET_MIN_MAX(rec);	\
	RESET_MIN_MAX(i2s_time);	\
	RESET_MIN_MAX(buffering);
	
#define STATS_PERIOD_MS 5000

extern struct outputstate output;
extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern u8_t *silencebuf;

static log_level loglevel;
static bool running, isI2SStarted;
static i2s_config_t i2s_config;
static int bytes_per_frame;
static thread_type thread, stats_thread;
static u8_t *obuf;

DECLARE_ALL_MIN_MAX;

static int _i2s_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
static void *output_thread_i2s();
static void *output_thread_i2s_stats();

/****************************************************************************************
 * Initialize the DAC output
 */
void output_init_i2s(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;
	
#ifdef CONFIG_I2S_BITS_PER_CHANNEL
	switch (CONFIG_I2S_BITS_PER_CHANNEL) {
		case 24:
			output.format = S24_BE;
			bytes_per_frame = 2*3;
			break;
		case 16:
			output.format = S16_BE;
			bytes_per_frame = 2*2;
			break;
		case 8:
			output.format = S8_BE;
			bytes_per_frame = 2*4;
			break;
		default:
			LOG_ERROR("Unsupported bit depth %d",CONFIG_I2S_BITS_PER_CHANNEL);
			break;
	}
#else
	output.format = S16_LE;
	bytes_per_frame = 2*2;
#endif

	output.write_cb = &_i2s_write_frames;
	obuf = malloc(FRAME_BLOCK * bytes_per_frame);
	if (!obuf) {
		LOG_ERROR("Cannot allocate i2s buffer");
		return;
	}
	
	running=true;

	i2s_config.mode = I2S_MODE_MASTER | I2S_MODE_TX;
	i2s_config.sample_rate = output.current_sample_rate;
	i2s_config.bits_per_sample = bytes_per_frame * 8 / 2;
	i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
	i2s_config.communication_format = I2S_COMM_FORMAT_I2S| I2S_COMM_FORMAT_I2S_MSB;
	// in case of overflow, do not replay old buffer
	i2s_config.tx_desc_auto_clear = true;		
	i2s_config.dma_buf_count = DMA_BUF_COUNT;
	// Counted in frames (but i2s allocates a buffer <= 4092 bytes)
	i2s_config.dma_buf_len = DMA_BUF_LEN;
	i2s_config.use_apll = true;
	i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1; //Interrupt level 1

	i2s_pin_config_t pin_config = { .bck_io_num = CONFIG_I2S_BCK_IO, .ws_io_num =
			CONFIG_I2S_WS_IO, .data_out_num = CONFIG_I2S_DO_IO, .data_in_num = -1 //Not used
			};
	LOG_INFO("Initializing I2S with rate: %d, bits per sample: %d, buffer frames: %d, number of buffers: %d ",
			i2s_config.sample_rate, i2s_config.bits_per_sample, i2s_config.dma_buf_len, i2s_config.dma_buf_count);

	i2s_driver_install(CONFIG_I2S_NUM, &i2s_config, 0, NULL);
	i2s_set_pin(CONFIG_I2S_NUM, &pin_config);
	i2s_stop(CONFIG_I2S_NUM);
	i2s_zero_dma_buffer(CONFIG_I2S_NUM);
	isI2SStarted=false;
	
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
	pthread_create_name(&thread, &attr, output_thread_i2s, NULL, "output_i2s");
	pthread_attr_destroy(&attr);

	// leave stack size to default 
	pthread_create_name(&stats_thread, NULL, output_thread_i2s_stats, NULL, "output_i2s_sts");
}


/****************************************************************************************
 * Terminate DAC output
 */
void output_close_i2s(void) {
	LOCK;
	running = false;
	UNLOCK;
	pthread_join(thread, NULL);
	pthread_join(stats_thread, NULL);
	i2s_driver_uninstall(CONFIG_I2S_NUM);
	free(obuf);
}

/****************************************************************************************
 * Write frames to the output buffer
 */
static int _i2s_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {
#if BYTES_PER_FRAME == 8									
	s32_t *optr;
#endif	
	
	if (!silence) {
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}
		
#if BYTES_PER_FRAME == 4
		if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
			_apply_gain(outputbuf, out_frames, gainL, gainR);
		}
			
		memcpy(obuf, outputbuf->readp, out_frames * bytes_per_frame);
#else
		optr = (s32_t*) outputbuf->readp;	
#endif		
	} else {
#if BYTES_PER_FRAME == 4		
		memcpy(obuf, silencebuf, out_frames * bytes_per_frame);
#else		
		optr = (s32_t*) silencebuf;
#endif	
	}

#if BYTES_PER_FRAME == 8
	IF_DSD(
	if (output.outfmt == DOP) {
			update_dop((u32_t *) optr, out_frames, output.invert);
		} else if (output.outfmt != PCM && output.invert)
			dsd_invert((u32_t *) optr, out_frames);
	)

	_scale_and_pack_frames(obuf, optr, out_frames, gainL, gainR, output.format);
#endif	

	return out_frames;
}


/****************************************************************************************
 * Main output thread
 */
static void *output_thread_i2s() {
	frames_t frames = 0;
	size_t bytes;
	uint32_t timer_start = 0;
	int discard = DMA_BUF_COUNT * DMA_BUF_LEN;
	uint32_t jitter = gettime_ms();
		
	while (running) {
			
		TIME_MEASUREMENT_START(timer_start);
		
		LOCK;
		
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			LOG_INFO("Output state is off.");
			if (isI2SStarted) {
				isI2SStarted=false;
				i2s_stop(CONFIG_I2S_NUM);
			}
			usleep(200000);
			continue;
		}
		
		output.updated = gettime_ms();
		// try to estimate how much we have consumed from the DMA buffer
		output.frames_played_dmp = output.frames_played - ((output.updated - jitter) * output.current_sample_rate) / 1000;
		output.device_frames = DMA_BUF_COUNT * DMA_BUF_LEN;
		
		frames = _output_frames( FRAME_BLOCK ); 
		
		SET_MIN_MAX_SIZED(frames,rec,FRAME_BLOCK);
		SET_MIN_MAX_SIZED(_buf_used(outputbuf),o,outputbuf->size);
		SET_MIN_MAX_SIZED(_buf_used(streambuf),s,streambuf->size);
		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),buffering);
		
		// must skip first frames as buffer already filled with silence
		// if (output.state < OUTPUT_RUNNING || output.state == OUTPUT_START_AT) {
		if (output.state == OUTPUT_START_AT) {
			discard = DMA_BUF_COUNT * DMA_BUF_LEN;
		} else if (discard) {
			discard -= output.frames_played + frames;
			if (discard < 0) {
				frames += discard;
				discard = 0;
			} else {
				UNLOCK;
				continue;
			}
		}	
		
		UNLOCK;
		
		// now send all the data
		TIME_MEASUREMENT_START(timer_start);
		
		if (!isI2SStarted ) {
			isI2SStarted = true;
			LOG_INFO("Restarting I2S.");
			i2s_zero_dma_buffer(CONFIG_I2S_NUM);
			i2s_start(CONFIG_I2S_NUM);
		} 
		
		// this does not work well as set_sample_rates resets the fifos (and it's too early)
		if (i2s_config.sample_rate != output.current_sample_rate) {
			LOG_INFO("changing sampling rate %u to %u", i2s_config.sample_rate, output.current_sample_rate);
			i2s_config.sample_rate = output.current_sample_rate;
			i2s_set_sample_rates(CONFIG_I2S_NUM, i2s_config.sample_rate);
			i2s_zero_dma_buffer(CONFIG_I2S_NUM);
		}
		
		// we assume that here we have been able to entirely fill the DMA buffers
		i2s_write(CONFIG_I2S_NUM, obuf, frames * bytes_per_frame, &bytes, portMAX_DELAY);
		jitter = gettime_ms();
			
		if (bytes != frames * bytes_per_frame) {
			LOG_WARN("I2S DMA Overflow! available bytes: %d, I2S wrote %d bytes", frames * bytes_per_frame, bytes);
		}
			
		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),i2s_time);
			
		frames = 0;
		
	}
	
	return 0;
}

/****************************************************************************************
 * Stats output thread
 */
static void *output_thread_i2s_stats() {
	while (running) {
		LOCK;
		output_state state = output.state;
		UNLOCK;
		if(state>OUTPUT_STOPPED){
			LOG_INFO( "Output State: %d, current sample rate: %d, bytes per frame: %d",state,output.current_sample_rate, bytes_per_frame);
			LOG_INFO( LINE_MIN_MAX_FORMAT_HEAD1);
			LOG_INFO( LINE_MIN_MAX_FORMAT_HEAD2);
			LOG_INFO( LINE_MIN_MAX_FORMAT_HEAD3);
			LOG_INFO( LINE_MIN_MAX_FORMAT_HEAD4);
			LOG_INFO(LINE_MIN_MAX_FORMAT_STREAM, LINE_MIN_MAX_STREAM("stream",s));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("output",o));
			LOG_INFO(LINE_MIN_MAX_FORMAT_FOOTER);
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("received",rec));
			LOG_INFO(LINE_MIN_MAX_FORMAT_FOOTER);
			LOG_INFO("");
			LOG_INFO("              ----------+----------+-----------+-----------+  ");
			LOG_INFO("              max (us)  | min (us) |   avg(us) |  count    |  ");
			LOG_INFO("              ----------+----------+-----------+-----------+  ");
			LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Buffering(us)",buffering));
			LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("i2s tfr(us)",i2s_time));
			LOG_INFO("              ----------+----------+-----------+-----------+");
			RESET_ALL_MIN_MAX;
		}
		usleep(STATS_PERIOD_MS *1000);
	}
	return NULL;
}




