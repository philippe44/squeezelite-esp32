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
#include "driver/i2s.h"
#include "perf_trace.h"
#include <signal.h>
#include "time.h"


#define DECLARE_ALL_MIN_MAX \
	DECLARE_MIN_MAX(o); \
	DECLARE_MIN_MAX(s); \
	DECLARE_MIN_MAX(loci2sbuf); \
	DECLARE_MIN_MAX(req); \
	DECLARE_MIN_MAX(rec); \
	DECLARE_MIN_MAX(over); \
	DECLARE_MIN_MAX(i2savailable);\
	DECLARE_MIN_MAX(i2s_time); \
	DECLARE_MIN_MAX(buffering);


#define RESET_ALL_MIN_MAX \
	RESET_MIN_MAX(o); \
	RESET_MIN_MAX(s); \
	RESET_MIN_MAX(loci2sbuf); \
	RESET_MIN_MAX(req);  \
	RESET_MIN_MAX(rec);  \
	RESET_MIN_MAX(over);  \
	RESET_MIN_MAX(i2savailable);\
	RESET_MIN_MAX(i2s_time);\
	RESET_MIN_MAX(buffering);
#define STATS_PERIOD_MS 5000

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

#if REPACK && BYTES_PER_FRAMES == 4
#error "REPACK is not compatible with BYTES_PER_FRAME=4"
#endif

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES

extern struct outputstate output;
extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern u8_t *silencebuf;

static log_level loglevel;
static size_t i2s_buffer_size = 0;
static bool running = true;
static bool isI2SStarted=false;
static struct buffer _i2s_buffer_structure;
static struct buffer *i2sbuffer=&_i2s_buffer_structure;
static i2s_config_t i2s_config;
static int bytes_per_frame;
static thread_type thread;
static pthread_t stats_thread;

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
	
	running=true;

	// todo: move this to a hardware abstraction layer
	//hal_dac_init(device);
		
	i2s_config.mode = I2S_MODE_MASTER | I2S_MODE_TX;                    // Only TX
	i2s_config.sample_rate = output.current_sample_rate;
	i2s_config.bits_per_sample = bytes_per_frame * 8 / 2;
	i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;           //2-channels
	i2s_config.communication_format = I2S_COMM_FORMAT_I2S| I2S_COMM_FORMAT_I2S_MSB;
	// todo: tune this parameter. Expressed in number of samples. Byte size depends on bit depth.
	i2s_config.dma_buf_count = 10;
	// From the I2S driver source, the DMA buffer size is 4092 bytes.
	// so buf_len * 2 channels * 2 bytes/sample should be < 4092 or else it will be resized.
	i2s_config.dma_buf_len =  FRAME_BLOCK/2;
	i2s_config.use_apll = false;
	i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1; //Interrupt level 1

	i2s_pin_config_t pin_config = { .bck_io_num = CONFIG_I2S_BCK_IO, .ws_io_num =
			CONFIG_I2S_WS_IO, .data_out_num = CONFIG_I2S_DO_IO, .data_in_num = -1 //Not used
			};
	LOG_INFO("Initializing I2S with rate: %d, bits per sample: %d, buffer len: %d, number of buffers: %d ",
			i2s_config.sample_rate, i2s_config.bits_per_sample, i2s_config.dma_buf_len, i2s_config.dma_buf_count);

	i2s_driver_install(CONFIG_I2S_NUM, &i2s_config, 0, NULL);
	i2s_set_pin(CONFIG_I2S_NUM, &pin_config);
	i2s_set_clk(CONFIG_I2S_NUM, output.current_sample_rate, i2s_config.bits_per_sample, 2);
	isI2SStarted=false;
	i2s_stop(CONFIG_I2S_NUM);

	i2s_buffer_size = 5*FRAME_BLOCK*bytes_per_frame;
	LOG_INFO("Allocating local DAC transfer buffer of %u bytes.",i2s_buffer_size);

	buf_init(i2sbuffer,i2s_buffer_size);
	if (!i2sbuffer->buf) {
		LOG_ERROR("unable to malloc i2s buffer");
		exit(0);
	}
	
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
	pthread_create(&thread, &attr, output_thread_i2s, NULL);
	pthread_attr_destroy(&attr);
#if HAS_PTHREAD_SETNAME_NP
	pthread_setname_np(thread, "output_i2s");
#endif

	// leave stack size to default 
	pthread_create(&stats_thread, NULL, output_thread_i2s_stats, NULL);
#if HAS_PTHREAD_SETNAME_NP
	pthread_setname_np(stats_thread, "output_i2s_sts");
#endif
}


/****************************************************************************************
 * Terminate DAC output
 */
void output_close_i2s(void) {
	i2s_driver_uninstall(CONFIG_I2S_NUM);
	buf_destroy(i2sbuffer);
}

/****************************************************************************************
 * Write frames to the output buffer
 */
static int _i2s_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {

	size_t bytes = out_frames * bytes_per_frame;
	assert(bytes > 0);
	
	if (!silence) {
		
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}
		
#if !REPACK
		if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
			_apply_gain(outputbuf, out_frames, gainL, gainR);
		}
			
		memcpy(i2sbuffer->writep, outputbuf->readp, bytes);
#else
		obuf = outputbuf->readp;	
#endif		

	} else {
		
#if !REPACK
		memcpy(i2sbuffer->writep, silencebuf,  bytes);
#endif		

	}

#if REPACK
	_scale_and_pack_frames(optr, (s32_t *)(void *)obuf, out_frames, gainL, gainR, output.format);
#endif	
	_buf_inc_writep(i2sbuffer, bytes);
	
	return bytes / bytes_per_frame;
}


/****************************************************************************************
 * Main output thread
 */
static void *output_thread_i2s() {
	frames_t frames=0;
	frames_t available_frames_space=0;
	size_t bytes_to_send_i2s=0, // Contiguous buffer which can be addressed
		 i2s_bytes_written = 0,
		 i2s_total_bytes_written=0; //actual size that the i2s port was able to write
	uint32_t timer_start=0;
	static int count = 0;
	output_state state;

	while (running) {
		i2s_bytes_written=0;
		frames=0;
		available_frames_space=0;
		bytes_to_send_i2s=0, // Contiguous buffer which can be addressed
		i2s_bytes_written = 0; //actual size that the i2s port was able to write
		TIME_MEASUREMENT_START(timer_start);
		LOCK;
		state =output.state;
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			LOG_INFO("Output state is off.");
			LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(i2sbuffer),_buf_cont_read(i2sbuffer));
			if(isI2SStarted) {
				isI2SStarted=false;
				i2s_stop(CONFIG_I2S_NUM);
			}
			usleep(200000);
			continue;
		}
		LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(i2sbuffer),_buf_cont_read(i2sbuffer));

		output.device_frames =0;
		output.updated = gettime_ms();
		output.frames_played_dmp = output.frames_played;

		do{
			// fill our buffer
			available_frames_space = min(_buf_space(i2sbuffer), _buf_cont_write(i2sbuffer)) / bytes_per_frame;
			if(available_frames_space)
			{
				frames = _output_frames( available_frames_space ); // Keep the transfer buffer full
				SET_MIN_MAX( available_frames_space,req);
				SET_MIN_MAX(frames,rec);
			}
		}while(available_frames_space>0 && frames>0);

		SET_MIN_MAX_SIZED(_buf_used(outputbuf),o,outputbuf->size);
		SET_MIN_MAX_SIZED(_buf_used(streambuf),s,streambuf->size);
		UNLOCK;
		LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(i2sbuffer),_buf_cont_read(i2sbuffer));

		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),buffering);

		SET_MIN_MAX_SIZED(_buf_used(i2sbuffer),loci2sbuf,i2sbuffer->size);
		bytes_to_send_i2s = _buf_cont_read(i2sbuffer);
		SET_MIN_MAX(bytes_to_send_i2s,i2savailable);
		i2s_total_bytes_written=0;

		while (bytes_to_send_i2s>0 )
		{
			// now send all the data
			TIME_MEASUREMENT_START(timer_start);
			if(!isI2SStarted)
			{
				isI2SStarted=true;
				LOG_INFO("Restarting I2S.");
				i2s_start(CONFIG_I2S_NUM);
			} 
			if (i2s_config.sample_rate != output.current_sample_rate) {
				LOG_INFO("changing sampling rate %u to %u", i2s_config.sample_rate, output.current_sample_rate);
				i2s_config.sample_rate = output.current_sample_rate;
				i2s_set_sample_rates(CONFIG_I2S_NUM, i2s_config.sample_rate);
			}
			count++;
			LOG_SDEBUG("Outputting to I2S");
			LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(i2sbuffer),_buf_cont_read(i2sbuffer));

			i2s_write(CONFIG_I2S_NUM, i2sbuffer->readp,bytes_to_send_i2s, &i2s_bytes_written, portMAX_DELAY);
			_buf_inc_readp(i2sbuffer,i2s_bytes_written);
			if(i2s_bytes_written!=bytes_to_send_i2s)
			{
				LOG_WARN("I2S DMA Overflow! available bytes: %d, I2S wrote %d bytes", bytes_to_send_i2s,i2s_bytes_written);

			}
			LOG_SDEBUG("DONE Outputting to I2S. Wrote: %d bytes out of %d", i2s_bytes_written,bytes_to_send_i2s);
			LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(i2sbuffer),_buf_cont_read(i2sbuffer));
			i2s_total_bytes_written+=i2s_bytes_written;
			SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),i2s_time);
			if(bytes_to_send_i2s>0) {
				SET_MIN_MAX(bytes_to_send_i2s-i2s_bytes_written,over);
			}
			bytes_to_send_i2s = _buf_cont_read(i2sbuffer);
			SET_MIN_MAX(bytes_to_send_i2s,i2savailable);

		}

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
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("dac buf used",loci2sbuf));
			LOG_INFO(LINE_MIN_MAX_FORMAT_FOOTER);
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("i2swrite",i2savailable));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("requested",req));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("received",rec));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("overflow",over));
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




