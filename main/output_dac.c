
#include "squeezelite.h"
#include "driver/i2s.h"

#include <signal.h>

#define I2S_NUM         (0)
#define I2S_BCK_IO      (GPIO_NUM_26)
#define I2S_WS_IO       (GPIO_NUM_25)
#define I2S_DO_IO       (GPIO_NUM_22)
#define I2S_DI_IO       (-1)

#define TIMED_SECTION_START_MS_FORCE(x,force) { static time_t __aa_time_start = 0; if(hasTimeElapsed(&__aa_time_start,x,force)) {
#define TIMED_SECTION_START_MS(x) 		{ static time_t __aa_time_start = 0; if(hasTimeElapsed(&__aa_time_start,x,false)){
#define TIMED_SECTION_START_FORCE(x,force) 			TIMED_SECTION_START_MS(x * 1000UL,force)
#define TIMED_SECTION_START(x) 			TIMED_SECTION_START_MS(x * 1000UL)
#define TIMED_SECTION_END				}}

static log_level loglevel;

static bool running = true;
static bool isI2SStarted=false;
extern struct outputstate output;
extern struct buffer *streambuf;
extern struct buffer *outputbuf;
static i2s_config_t i2s_config;
#if REPACK && BYTES_PER_FRAMES == 4
#error "REPACK is not compatible with BYTES_PER_FRAME=4"
#endif

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES
#define DAC_OUTPUT_BUFFER_FRAMES FRAME_BLOCK
#define DAC_OUTPUT_BUFFER_RESERVE FRAME_BLOCK/2
#define I2S_FRAME_SIZE 256
#define FRAME_TO_BYTES(f) f*BYTES_PER_FRAME
#define BYTES_TO_FRAME(b) b/BYTES_PER_FRAME
#define FRAMES_TO_MS(f) 1000*f/output.current_sample_rate
#define BYTES_TO_MS(b) FRAMES_TO_MS(BYTES_TO_FRAME(b))

#define SET_MIN_MAX(val,var) var=val; if(var<min_##var) min_##var=var; if(var>max_##var) max_##var=var
#define RESET_MIN_MAX(var,mv) min_##var=mv##_MAX; max_##var=mv##_MIN
#define DECLARE_MIN_MAX(var,t,mv) static t min_##var = mv##_MAX, max_##var = mv##_MIN; t var=0
#define DECLARE_ALL_MIN_MAX DECLARE_MIN_MAX(req, long,LONG); DECLARE_MIN_MAX(o, long,LONG); DECLARE_MIN_MAX(s, long,LONG); DECLARE_MIN_MAX(d, long,LONG); DECLARE_MIN_MAX(duration, long,LONG);DECLARE_MIN_MAX(buffering, long,LONG);DECLARE_MIN_MAX(totalprocess, long,LONG);
#define RESET_ALL_MIN_MAX RESET_MIN_MAX(d,LONG); RESET_MIN_MAX(o,LONG); RESET_MIN_MAX(s,LONG); RESET_MIN_MAX(req,LONG);  RESET_MIN_MAX(duration,LONG);RESET_MIN_MAX(buffering,LONG);RESET_MIN_MAX(totalprocess,LONG);
extern u8_t *silencebuf;

static u8_t *optr;
static int bytes_per_frame;
static thread_type thread;

static int _dac_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
static void *output_thread();
bool hasTimeElapsed(time_t * lastTime, time_t delayMS, bool bforce)
{
	if (*lastTime <= gettime_ms() ||bforce)
	{
		*lastTime = gettime_ms() + delayMS;
		return true;
	}
	else
		return false;
}
void set_volume(unsigned left, unsigned right) {
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	LOCK;
	output.gainL = left;
	output.gainR = right;
	UNLOCK;
}


void output_init_dac(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;
	optr = malloc(FRAME_TO_BYTES(DAC_OUTPUT_BUFFER_FRAMES));
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
	output.start_frames = DAC_OUTPUT_BUFFER_FRAMES*2;
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
	

	i2s_config.mode = I2S_MODE_MASTER | I2S_MODE_TX;                    // Only TX
	i2s_config.sample_rate = output.current_sample_rate;
	i2s_config.bits_per_sample = BYTES_PER_FRAME * 8/2;
	i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;           //2-channels
	i2s_config.communication_format = I2S_COMM_FORMAT_I2S
					| (output.format==S16_LE||output.format==S32_LE||output.format==S24_3LE)?I2S_COMM_FORMAT_I2S_LSB:I2S_COMM_FORMAT_I2S_MSB;
	i2s_config.dma_buf_count = 6; //todo: tune this parameter. Expressed in numbrer of buffers.
	i2s_config.dma_buf_len = I2S_FRAME_SIZE; // todo: tune this parameter. Expressed in number of samples. Byte size depends on bit depth
	i2s_config.use_apll = false;
	i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1; //Interrupt level 1

	i2s_pin_config_t pin_config = { .bck_io_num = I2S_BCK_IO, .ws_io_num =
			I2S_WS_IO, .data_out_num = I2S_DO_IO, .data_in_num = I2S_DI_IO //Not used
			};
	LOG_INFO("Initializing I2S with rate: %d, bits per sample: %d, buffer len: %d, number of buffers: %d ",
			i2s_config.sample_rate, i2s_config.bits_per_sample, i2s_config.dma_buf_len, i2s_config.dma_buf_count);
	i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
	i2s_set_pin(I2S_NUM, &pin_config);
	i2s_set_clk(I2S_NUM, output.current_sample_rate, i2s_config.bits_per_sample, 2);
	isI2SStarted=false;
	i2s_stop(I2S_NUM);

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
//	TIMED_SECTION_START_MS(500);
//	LOG_INFO("Done moving data to out buffer");
//	TIMED_SECTION_END;
	return (int)out_frames;
}

void wait_for_frames(size_t frames)
{
	usleep((1000* frames/output.current_sample_rate) );
}

static void *output_thread() {
//	// buffer to hold output data so we can block on writing outside of output lock, allocated on init
//	u8_t *obuf = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
	u8_t *opos=optr;
	frames_t frames=0, requested_frames = 0;
	size_t used_buffer=0;
	static int count = 0, count2=0;
	uint32_t start_writing=0, start_i2s=0;
	DECLARE_ALL_MIN_MAX;

	size_t i2s_bytes_write, i2s_bytes_to_write = 0;
#if REPACK
	LOCK;

	switch (output.format) {
	case S32_BE:
	case S32_LE:
		bytes_per_frame = 4 * 2; break;
	case S24_3LE:
	case S24_3BE:
		bytes_per_frame = 3 * 2; break;
	case S16_LE:
	case S16_BE:
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
		start_writing=esp_timer_get_time();
		LOCK;
		
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			LOG_INFO("Output state is off.");
			isI2SStarted=false;
			i2s_stop(I2S_NUM);
			usleep(500000);
			continue;
		}		
		requested_frames = 0;
		frames=0;
		if(used_buffer==0)
		{
			// replenish buffer when it's empty
			opos=optr;
			requested_frames =DAC_OUTPUT_BUFFER_FRAMES;

			frames = _output_frames( requested_frames ); // Keep the dma buffer full
			used_buffer+=FRAME_TO_BYTES(frames);

		}
		UNLOCK;
		if(frames>0) SET_MIN_MAX((esp_timer_get_time()-start_writing)/1000,buffering);
		// todo: call i2s_set_clock here if rate is changed


		if (used_buffer  )
		{
			start_i2s=esp_timer_get_time();
			if(!isI2SStarted)
			{
				isI2SStarted=true;
				i2s_start(I2S_NUM);
			}
			i2s_write(I2S_NUM, opos,used_buffer, &i2s_bytes_write, portMAX_DELAY);
			if(i2s_bytes_write!=used_buffer)
			{
				LOG_WARN("I2S DMA Overflow! available bytes: %d, I2S wrote %d bytes", used_buffer,i2s_bytes_write);
			}
			used_buffer -= i2s_bytes_write;
			opos+=i2s_bytes_write;
			output.device_frames =BYTES_TO_FRAME(used_buffer);
			output.updated = gettime_ms();
			output.frames_played_dmp = output.frames_played-output.device_frames;
			SET_MIN_MAX((esp_timer_get_time()-start_i2s)/1000,duration);
		}
		SET_MIN_MAX(duration+frames>0?buffering:0,totalprocess);
		SET_MIN_MAX(_buf_used(outputbuf),o);
		SET_MIN_MAX(_buf_used(streambuf),s);
		SET_MIN_MAX(used_buffer,d);
		SET_MIN_MAX(requested_frames,req);
		if (!(count++ & 0x1ff)) {
			LOG_INFO( "count:%d"
					"\n              ----------+----------+-----------+  +----------+----------+----------------+"
					"\n                    max |      min |    current|  |      max |      min |        current |"
					"\n                   (ms) |     (ms) |       (ms)|  | (frames) | (frames) |        (frames)|"
					"\n              ----------+----------+-----------+  +----------+----------+----------------+"
					"\nout           %10d|%10d|%11d|"                 "  |%10d|%10d|%16d|"
					"\nstream        %10d|%10d|%11d|"                 "  |%10d|%10d|%16d|"
					"\nDMA overflow  %10d|%10d|%11d|"                 "  |%10d|%10d|%16d|"
					"\nrequested     %10d|%10d|%11d|"                 "  |%10d|%10d|%16d|"
					"\n              ----------+----------+-----------+  +----------+----------+----------------+"
					"\n"
					"\n              max (us)  | min (us) | total(us) |  "
					"\n              ----------+----------+-----------+  "
					"\ni2s time (us):%10d|%10d|%11d|"
					"\nbuffering(us):%10d|%10d|%11d|"
					"\ntotal(us)    :%10d|%10d|%11d|"
					"\n              ----------+----------+-----------+  ",
					count,
					BYTES_TO_MS(max_o), BYTES_TO_MS(min_o),BYTES_TO_MS(o),max_o,min_o,o,
					BYTES_TO_MS(max_s), BYTES_TO_MS(min_s),BYTES_TO_MS(s),max_s,min_s,s,
					BYTES_TO_MS(max_d),BYTES_TO_MS(min_d),BYTES_TO_MS(d),max_d,min_d,d,
					FRAMES_TO_MS(max_req),FRAMES_TO_MS(min_req),FRAMES_TO_MS(req), max_req, min_req,req,
					max_duration, min_duration, duration,
					max_buffering, min_buffering, buffering,
					max_totalprocess,min_totalprocess,totalprocess
					);
			RESET_ALL_MIN_MAX;
		}
	}

	return 0;
}

bool test_open(const char *device, unsigned rates[], bool userdef_rates) {
	unsigned _rates[] = { 96000, 88200, 48000, 44100, 32000, 0 };	
	memcpy(rates, _rates, sizeof(_rates));
	return true;
}


