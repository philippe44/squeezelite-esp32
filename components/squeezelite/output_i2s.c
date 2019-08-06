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
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "perf_trace.h"
#include <signal.h>
#include "time.h"
#include "led.h"

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

typedef enum { DAC_ON = 0, DAC_OFF, DAC_POWERDOWN, DAC_VOLUME } dac_cmd_e;

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
static void dac_cmd(dac_cmd_e cmd, ...);

#ifdef CONFIG_SQUEEZEAMP

#define TAS575x

#undef	CONFIG_I2S_BCK_IO 
#define CONFIG_I2S_BCK_IO 	33
#undef 	CONFIG_I2S_WS_IO	
#define CONFIG_I2S_WS_IO	25
#undef 	CONFIG_I2S_DO_IO
#define CONFIG_I2S_DO_IO	32
#undef 	CONFIG_I2S_NUM
#define CONFIG_I2S_NUM		0

#define I2C_PORT	0
#define I2C_ADDR	0x4c
#define VOLUME_GPIO	33

struct tas575x_cmd_s {
	u8_t reg;
	u8_t value;
};
	
static const struct tas575x_cmd_s tas575x_init_sequence[] = {
    { 0x00, 0x00 },		// select page 0
    { 0x02, 0x10 },		// standby
    { 0x0d, 0x10 },		// use SCK for PLL
    { 0x25, 0x08 },		// ignore SCK halt 
	{ 0x02, 0x00 },		// restart
	{ 0xff, 0xff }		// end of table
};

static const i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = 27,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = 26,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
};

static const struct tas575x_cmd_s tas575x_cmd[] = {
	{ 0x02, 0x00 },	// DAC_ON
	{ 0x02, 0x10 },	// DAC_OFF
	{ 0x02, 0x01 },	// DAC_POWERDOWN
};
#endif

/****************************************************************************************
 * Initialize the DAC output
 */
void output_init_i2s(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;
	
#ifdef TAS575x
	gpio_pad_select_gpio(39);
	gpio_set_direction(39, GPIO_MODE_INPUT);
	
	adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0,ADC_ATTEN_DB_0);
    			
	// init volume & mute
	gpio_pad_select_gpio(VOLUME_GPIO);
	gpio_set_direction(VOLUME_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(VOLUME_GPIO, 0);
	
	// configure i2c
	i2c_param_config(I2C_PORT, &i2c_config);
	i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, false, false, false);
	i2c_cmd_handle_t i2c_cmd = i2c_cmd_link_create();
	
	for (int i = 0; tas575x_init_sequence[i].reg != 0xff; i++) {
		i2c_master_start(i2c_cmd);
		i2c_master_write_byte(i2c_cmd, I2C_ADDR << 1 | I2C_MASTER_WRITE, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas575x_init_sequence[i].reg, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas575x_init_sequence[i].value, I2C_MASTER_NACK);

		LOG_DEBUG("i2c write %x at %u", tas575x_init_sequence[i].reg, tas575x_init_sequence[i].value);
	}

	i2c_master_stop(i2c_cmd);	
	esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, i2c_cmd, 500 / portTICK_RATE_MS);
    i2c_cmd_link_delete(i2c_cmd);
	
	if (ret != ESP_OK) {
		LOG_ERROR("could not intialize TAS575x %d", ret);
	}
#endif	
	
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
	
#ifdef TAS575x	
	i2c_driver_delete(I2C_PORT);
#endif	
}

/****************************************************************************************
 * change volume
 */
bool output_volume_i2s(unsigned left, unsigned right) {
#ifdef TAS575x	
	gpio_set_level(VOLUME_GPIO, left || right);
#endif	
 return false;	
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
	frames_t iframes = FRAME_BLOCK, oframes;
	size_t bytes;
	uint32_t timer_start = 0;
	int discard = 0;
	uint32_t fullness = gettime_ms();
	bool synced;
#ifdef TAS575x					
	output_state state = OUTPUT_OFF;
#endif	
		
	while (running) {
			
		TIME_MEASUREMENT_START(timer_start);
		
		LOCK;
		
#ifdef TAS575x						
		// manage led display
		if (state != output.state) {
			if (output.state == OUTPUT_OFF) led_blink(LED_GREEN, 100, 2500);
			else if (output.state == OUTPUT_STOPPED) led_blink_wait(LED_GREEN, 200, 1000);
			else if (output.state == OUTPUT_RUNNING) led_on(LED_GREEN);
		}
		state = output.state;
#endif				
		
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			LOG_INFO("Output state is off.");
			if (isI2SStarted) {
				isI2SStarted = false;
				i2s_stop(CONFIG_I2S_NUM);
				dac_cmd(DAC_OFF);
			}
			LOG_ERROR("Jack %d Voltage %.2fV", !gpio_get_level(39), adc1_get_raw(ADC1_CHANNEL_0) / 4095. * (10+169)/10. * 1.1);
			usleep(200000);
			continue;
		} else if (output.state == OUTPUT_STOPPED) {
			synced = false;
		}
		
		output.updated = gettime_ms();
		output.frames_played_dmp = output.frames_played;
		// try to estimate how much we have consumed from the DMA buffer
		output.device_frames = DMA_BUF_COUNT * DMA_BUF_LEN - ((output.updated - fullness) * output.current_sample_rate) / 1000;
				
		oframes = _output_frames( iframes ); 
		
		SET_MIN_MAX_SIZED(oframes,rec,iframes);
		SET_MIN_MAX_SIZED(_buf_used(outputbuf),o,outputbuf->size);
		SET_MIN_MAX_SIZED(_buf_used(streambuf),s,streambuf->size);
		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),buffering);
		
		/* must skip first whatever is in the pipe (but not when resuming). 
		This test is incorrect when we pause a track that has just started, 
		but this is higly unlikely and I don't have a better one for now */
		if (output.state == OUTPUT_START_AT) {
			discard = output.frames_played_dmp ? 0 : output.device_frames;
			synced = true;
		} else if (discard) {
			discard -= oframes;
			iframes = discard ? min(FRAME_BLOCK, discard) : FRAME_BLOCK;
			UNLOCK;
			continue;
		}
		
		UNLOCK;
		
		// now send all the data
		TIME_MEASUREMENT_START(timer_start);
		
		if (!isI2SStarted ) {
			isI2SStarted = true;
			LOG_INFO("Restarting I2S.");
			i2s_zero_dma_buffer(CONFIG_I2S_NUM);
			i2s_start(CONFIG_I2S_NUM);
			dac_cmd(DAC_ON);	
		} 
		
		// this does not work well as set_sample_rates resets the fifos (and it's too early)
		if (i2s_config.sample_rate != output.current_sample_rate) {
			LOG_INFO("changing sampling rate %u to %u", i2s_config.sample_rate, output.current_sample_rate);
			/* 
			if (synced)
				//  can sleep for a buffer_queue - 1 and then eat a buffer (discard) if we are synced
				usleep(((DMA_BUF_COUNT - 1) * DMA_BUF_LEN * bytes_per_frame * 1000) / 44100 * 1000);
				discard = DMA_BUF_COUNT * DMA_BUF_LEN * bytes_per_frame;
			}	
			*/
			i2s_config.sample_rate = output.current_sample_rate;
			i2s_set_sample_rates(CONFIG_I2S_NUM, i2s_config.sample_rate);
			i2s_zero_dma_buffer(CONFIG_I2S_NUM);
			//return;
		}
		
		// we assume that here we have been able to entirely fill the DMA buffers
		i2s_write(CONFIG_I2S_NUM, obuf, oframes * bytes_per_frame, &bytes, portMAX_DELAY);
		fullness = gettime_ms();
			
		if (bytes != oframes * bytes_per_frame) {
			LOG_WARN("I2S DMA Overflow! available bytes: %d, I2S wrote %d bytes", oframes * bytes_per_frame, bytes);
		}
			
		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),i2s_time);
		
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

/****************************************************************************************
 * DAC specific commands
 */
void dac_cmd(dac_cmd_e cmd, ...) {
	va_list args;
	esp_err_t ret = ESP_OK;
	
	va_start(args, cmd);
#ifdef TAS575x	
	i2c_cmd_handle_t i2c_cmd = i2c_cmd_link_create();

	switch(cmd) {
	case DAC_VOLUME:
		LOG_ERROR("volume not handled yet");
		break;
	default:
		i2c_master_start(i2c_cmd);
		i2c_master_write_byte(i2c_cmd, I2C_ADDR << 1 | I2C_MASTER_WRITE, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas575x_cmd[cmd].reg, I2C_MASTER_NACK);
		i2c_master_write_byte(i2c_cmd, tas575x_cmd[cmd].value, I2C_MASTER_NACK);
		i2c_master_stop(i2c_cmd);	
		ret	= i2c_master_cmd_begin(I2C_PORT, i2c_cmd, 50 / portTICK_RATE_MS);
	}
	
    i2c_cmd_link_delete(i2c_cmd);
	
	if (ret != ESP_OK) {
		LOG_ERROR("could not intialize TAS575x %d", ret);
	}
#endif	
	va_end(args);
}




