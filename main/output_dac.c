
#include "squeezelite.h"
#include "driver/i2s.h"
#include <signal.h>
#define I2S_NUM         (0)
#define I2S_BCK_IO      (GPIO_NUM_26)
#define I2S_WS_IO       (GPIO_NUM_25)
#define I2S_DO_IO       (GPIO_NUM_22)
#define I2S_DI_IO       (-1)

// buffer length is expressed in number of samples
#define I2S_BUF_LEN		60
static log_level loglevel;

static bool running = true;

extern struct outputstate output;
extern struct buffer *outputbuf;

#if REPACK && BYTES_PER_FRAMES == 4
#error "REPACK is not compatible with BYTES_PER_FRAME=4"
#endif

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES

extern u8_t *silencebuf;

static u8_t *optr;
static int bytes_per_frame;
static thread_type thread;

static int _dac_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
static void *output_thread();

void set_volume(unsigned left, unsigned right) {
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	LOCK;
	output.gainL = left;
	output.gainR = right;
	UNLOCK;
}

void output_init_dac(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;
	optr = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
	if (!optr) {
		LOG_ERROR("unable to malloc buf");
		return;
	}
	LOG_INFO("init output DAC");
	
	memset(&output, 0, sizeof(output));

#if BYTES_PER_FRAME == 4
	output.format = S16_LE;
#else 
	output.format = S32_LE;
#endif	
	output.start_frames = FRAME_BLOCK * 2;
	output.write_cb = &_dac_write_frames;
	output.rate_delay = rate_delay;

	if (params) {
		if (!strcmp(params, "32"))	output.format = S32_LE;
		if (!strcmp(params, "24")) output.format = S24_3LE;
		if (!strcmp(params, "16")) output.format = S16_LE;
	}
	
	// ensure output rate is specified to avoid test open
	if (!rates[0]) {
		rates[0] = 44100;
	}

	output_init_common(level, device, output_buf_size, rates, idle);
	
	i2s_config_t i2s_config = {
			.mode = I2S_MODE_MASTER | I2S_MODE_TX,                    // Only TX
			.sample_rate = output.current_sample_rate,
			.bits_per_sample = BYTES_PER_FRAME * 8,
			.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,           //2-channels
			.communication_format = I2S_COMM_FORMAT_I2S
					| I2S_COMM_FORMAT_I2S_MSB,
			.dma_buf_count = 6, //todo: tune this parameter. Expressed in numbrer of buffers
			.dma_buf_len = I2S_BUF_LEN, // todo: tune this parameter. Expressed in number of samples. Byte size depends on bit depth
			.use_apll = false,
			.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1 //Interrupt level 1
			};
	i2s_pin_config_t pin_config = { .bck_io_num = I2S_BCK_IO, .ws_io_num =
			I2S_WS_IO, .data_out_num = I2S_DO_IO, .data_in_num = I2S_DI_IO //Not used
			};
	i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
	i2s_set_pin(I2S_NUM, &pin_config);
	i2s_set_clk(I2S_NUM, output.current_sample_rate, i2s_config.bits_per_sample, 2);

#if LINUX || OSX || FREEBSD || POSIX
	pthread_attr_t attr;
	pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
#endif
	pthread_create(&thread, &attr, output_thread, NULL);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&output_thread, NULL, 0, NULL);
#endif
}

void output_close_dac(void) {
	LOG_INFO("close output");

	LOCK;
	running = false;
	UNLOCK;
	free(optr);
	output_close_common();
}

static int _dac_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {

	u8_t *obuf;
	
	if (!silence) {
		
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}
		
#if !REPACK
		if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
			_apply_gain(outputbuf, out_frames, gainL, gainR);
		}
			
		IF_DSD(
		if (output.outfmt == DOP) {
				update_dop((u32_t *) outputbuf->readp, out_frames, output.invert);
			} else if (output.outfmt != PCM && output.invert)
				dsd_invert((u32_t *) outputbuf->readp, out_frames);
		)
		
		memcpy(optr, outputbuf->readp, out_frames * BYTES_PER_FRAME);
#else
		obuf = outputbuf->readp;	
#endif		

	} else {
	
		obuf = silencebuf;
#if !REPACK
		IF_DSD(
			if (output.outfmt != PCM) {
				obuf = silencebuf_dsd;
				update_dop((u32_t *) obuf, out_frames, false); // don't invert silence
			}
		)

		memcpy(optr, obuf, out_frames * BYTES_PER_FRAME);
#endif		
	}

#if REPACK
	_scale_and_pack_frames(optr, (s32_t *)(void *)obuf, out_frames, gainL, gainR, output.format);
#endif	

	return (int)out_frames;
}

static void *output_thread() {
	// buffer to hold output data so we can block on writing outside of output lock, allocated on init
	u8_t *obuf = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
	int frames = 0;
	size_t i2s_bytes_write = 0;
#if REPACK
	LOCK;

	switch (output.format) {
	case S32_LE:
		bytes_per_frame = 4 * 2; break;
	case S24_3LE:
		bytes_per_frame = 3 * 2; break;
	case S16_LE:
		bytes_per_frame = 2 * 2; break;
	default:
		bytes_per_frame = 4 * 2; break;
		break;
	}

	UNLOCK;
#else	
	bytes_per_frame = BYTES_PER_FRAME;
#endif

	while (running) {

		LOCK;
		
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			usleep(500000);
			continue;
		}		
		// todo: call i2s_set_clock here if rate is changed

		output.device_frames = 0;
		output.updated = gettime_ms();
		output.frames_played_dmp = output.frames_played;

		optr = obuf + frames * bytes_per_frame;
		frames += _output_frames(FRAME_BLOCK);
		
		UNLOCK;

		if (frames) {
			 i2s_write(I2S_NUM, optr,frames*BYTES_PER_FRAME, &i2s_bytes_write, 100);
			if(i2s_bytes_write!=frames*BYTES_PER_FRAME){
				LOG_WARN("Bytes available: %d, I2S wrote %d", frames*BYTES_PER_FRAME,i2s_bytes_write);
			}
			 usleep((frames * 1000 * 1000) / output.current_sample_rate);
			frames = 0;
		} else {
			usleep((FRAME_BLOCK * 1000 * 1000) / output.current_sample_rate);
		}	
		
	}

	return 0;
}

bool test_open(const char *device, unsigned rates[], bool userdef_rates) {
	unsigned _rates[] = { 96000, 88200, 48000, 44100, 32000, 0 };	
	memcpy(rates, _rates, sizeof(_rates));
	return true;
}


