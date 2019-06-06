
#include "squeezelite.h"
#include "driver/i2s.h"
#include "perf_trace.h"
#include <signal.h>


#define DECLARE_ALL_MIN_MAX \
	DECLARE_MIN_MAX(req, long,LONG); \
	DECLARE_MIN_MAX(rec, long,LONG); \
	DECLARE_MIN_MAX(over, long,LONG); \
	DECLARE_MIN_MAX(o, long,LONG); \
	DECLARE_MIN_MAX(s, long,LONG); \
	DECLARE_MIN_MAX(d, long,LONG); \
	DECLARE_MIN_MAX(loci2sbuf, long,LONG); \
	DECLARE_MIN_MAX(buffering, long,LONG);\
	DECLARE_MIN_MAX(i2s_time, long,LONG); \
	DECLARE_MIN_MAX(i2savailable, long,LONG);
#define RESET_ALL_MIN_MAX \
	RESET_MIN_MAX(d,LONG); \
	RESET_MIN_MAX(o,LONG); \
	RESET_MIN_MAX(s,LONG); \
	RESET_MIN_MAX(loci2sbuf, LONG); \
	RESET_MIN_MAX(req,LONG);  \
	RESET_MIN_MAX(rec,LONG);  \
	RESET_MIN_MAX(over,LONG);  \
	RESET_MIN_MAX(over,LONG);  \
	RESET_MIN_MAX(i2savailable,LONG);\
	RESET_MIN_MAX(i2s_time,LONG);

static log_level loglevel;
size_t dac_buffer_size =0;
static bool running = true;
static bool isI2SStarted=false;
extern struct outputstate output;
extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern u8_t *silencebuf;
static struct buffer _dac_buffer_structure;
struct buffer *dacbuffer=&_dac_buffer_structure;

static i2s_config_t i2s_config;
#if REPACK && BYTES_PER_FRAMES == 4
#error "REPACK is not compatible with BYTES_PER_FRAME=4"
#endif

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES

#define FRAME_TO_BYTES(f) f*out_bytes_per_frame
#define BYTES_TO_FRAME(b) b/out_bytes_per_frame


static int out_bytes_per_frame;
static thread_type thread;

static int _dac_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
static void *output_thread();


/****************************************************************************************
 * set output volume
 */
void set_volume(unsigned left, unsigned right) {
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	LOCK;
	output.gainL = left;
	output.gainR = right;
	UNLOCK;
}

/****************************************************************************************
 * Initialize the DAC output
 */
void output_init_dac(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;

	LOG_INFO("Init output DAC.");

	LOG_DEBUG("Setting output parameters.");

	memset(&output, 0, sizeof(output));

	switch (CONFIG_I2S_BITS_PER_CHANNEL) {
		case 24:
			output.format = S24_BE;
			break;
		case 16:
			output.format = S16_BE;
			break;
		case 8:
			output.format = S8_BE;
			break;
		default:
			LOG_ERROR("Unsupported bit depth %d",CONFIG_I2S_BITS_PER_CHANNEL);
			break;
	}
	// ensure output rate is specified to avoid test open
	if (!rates[0]) {
		rates[0] = 44100;
	}
	running=true;
	// get common output configuration details
	output_init_common(level, device, output_buf_size, rates, idle);

	out_bytes_per_frame = get_bytes_per_frame(output.format);

	output.start_frames = FRAME_BLOCK;
	output.write_cb = &_dac_write_frames;
	output.rate_delay = rate_delay;

	i2s_config.mode = I2S_MODE_MASTER | I2S_MODE_TX;                    // Only TX
	i2s_config.sample_rate = output.current_sample_rate;
	i2s_config.bits_per_sample = get_bytes_per_frame(output.format) * 8/2;
	i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;           //2-channels
	i2s_config.communication_format = I2S_COMM_FORMAT_I2S| I2S_COMM_FORMAT_I2S_MSB;
	// todo: tune this parameter. Expressed in number of samples. Byte size depends on bit depth.
	i2s_config.dma_buf_count = 64; //todo: tune this parameter. Expressed in numbrer of buffers.
	i2s_config.dma_buf_len =  128;
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

	dac_buffer_size = 10*FRAME_BLOCK*get_bytes_per_frame(output.format);
	LOG_DEBUG("Allocating local DAC transfer buffer of %u bytes.",dac_buffer_size);
	buf_init(dacbuffer,dac_buffer_size );
	if (!dacbuffer->buf) {
		LOG_ERROR("unable to malloc i2s buffer");
		exit(0);
	}
	LOG_SDEBUG("Current buffer free: %d",_buf_space(dacbuffer));


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
	LOG_INFO("Init completed.");

}


/****************************************************************************************
 * Terminate DAC output
 */
void output_close_dac(void) {
	LOG_INFO("close output");

	LOCK;
	running = false;
	UNLOCK;
	i2s_driver_uninstall(CONFIG_I2S_NUM);
	output_close_common();
	buf_destroy(dacbuffer);
}

/****************************************************************************************
 * Write frames to the output buffer
 */
static int _dac_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {

	size_t actual_out_bytes=FRAME_TO_BYTES(out_frames);
	assert(out_bytes_per_frame>0);
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
		
		memcpy(dacbuffer->writep, outputbuf->readp, actual_out_bytes);
#else
		obuf = outputbuf->readp;	
#endif		

	} else {
#if !REPACK
		IF_DSD(
			if (output.outfmt != PCM) {
				obuf = silencebuf_dsd;
				update_dop((u32_t *) obuf, out_frames, false); // don't invert silence
			}
		)

		memcpy(dacbuffer->writep, silencebuf,  actual_out_bytes);
#endif		
	}

#if REPACK
	_scale_and_pack_frames(optr, (s32_t *)(void *)obuf, out_frames, gainL, gainR, output.format);
#endif	
	_buf_inc_writep(dacbuffer,actual_out_bytes);
	return (int)BYTES_TO_FRAME(actual_out_bytes);
}


/****************************************************************************************
 * Wait for a duration based on a frame count
 */
void wait_for_frames(size_t frames)
{
	usleep((1000* frames/output.current_sample_rate*.90) );
}

/****************************************************************************************
 * Main output thread
 */
static void *output_thread() {
	frames_t frames=0;
	frames_t available_frames_space=0;
	size_t bytes_to_send_i2s=0, // Contiguous buffer which can be addressed
		 i2s_bytes_written = 0; //actual size that the i2s port was able to write
	uint32_t timer_start=0;
	static int count = 0;

	DECLARE_ALL_MIN_MAX;

	while (running) {
		i2s_bytes_written=0;
		frames=0;
		available_frames_space=0;
		bytes_to_send_i2s=0, // Contiguous buffer which can be addressed
		i2s_bytes_written = 0; //actual size that the i2s port was able to write
		TIME_MEASUREMENT_START(timer_start);

		LOCK;
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			LOG_INFO("Output state is off.");
			LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(dacbuffer),_buf_cont_read(dacbuffer));
			if(isI2SStarted) {
				isI2SStarted=false;
				i2s_stop(CONFIG_I2S_NUM);
			}
			usleep(500000);
			continue;
		}
		LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(dacbuffer),_buf_cont_read(dacbuffer));
		available_frames_space = BYTES_TO_FRAME(min(_buf_space(dacbuffer), _buf_cont_write(dacbuffer)));
		frames = _output_frames( available_frames_space ); // Keep the transfer buffer full
		UNLOCK;
		LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(dacbuffer),_buf_cont_read(dacbuffer));
		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),buffering);
		SET_MIN_MAX( available_frames_space,req);
		SET_MIN_MAX(frames,rec);
		if(frames>0){
				//LOG_DEBUG("Frames available : %u.",frames);
		}
		else
		{
			//LOG_DEBUG("No frame available");
			usleep(10000);
		}

		SET_MIN_MAX(_buf_used(dacbuffer),loci2sbuf);
		bytes_to_send_i2s = _buf_cont_read(dacbuffer);
		SET_MIN_MAX(bytes_to_send_i2s,i2savailable);
		if (bytes_to_send_i2s>0  )
		{
			TIME_MEASUREMENT_START(timer_start);
			if(!isI2SStarted)
			{
				isI2SStarted=true;
				LOG_INFO("Restarting I2S.");
				i2s_start(CONFIG_I2S_NUM);
				if( i2s_config.sample_rate != output.current_sample_rate)
				{
					i2s_config.sample_rate = output.current_sample_rate;
					i2s_set_sample_rates(CONFIG_I2S_NUM, i2s_config.sample_rate);
				}
			}
			count++;
			LOG_SDEBUG("Outputting to I2S");
			LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(dacbuffer),_buf_cont_read(dacbuffer));

			i2s_write(CONFIG_I2S_NUM, dacbuffer->readp,bytes_to_send_i2s, &i2s_bytes_written, portMAX_DELAY);
			_buf_inc_readp(dacbuffer,i2s_bytes_written);
			if(i2s_bytes_written!=bytes_to_send_i2s)
			{
				LOG_WARN("I2S DMA Overflow! available bytes: %d, I2S wrote %d bytes", bytes_to_send_i2s,i2s_bytes_written);

			}
			LOG_SDEBUG("DONE Outputting to I2S. Wrote: %d bytes out of %d", i2s_bytes_written,bytes_to_send_i2s);
			LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(dacbuffer),_buf_cont_read(dacbuffer));


			output.device_frames =0;
			output.updated = gettime_ms();
			output.frames_played_dmp = output.frames_played;
			SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),i2s_time);

		}
		SET_MIN_MAX(bytes_to_send_i2s-i2s_bytes_written,over);
		SET_MIN_MAX(_buf_used(outputbuf),o);
		SET_MIN_MAX(_buf_used(streambuf),s);

		/*
		 * Statistics reporting
		 */
#define STATS_PERIOD_MS 5000
		count++;
		TIMED_SECTION_START_MS(STATS_PERIOD_MS);

		LOG_INFO( "count:%d, current sample rate: %d, bytes per frame: %d, avg cycle duration (ms): %d",count,output.current_sample_rate, out_bytes_per_frame,STATS_PERIOD_MS/count);
		LOG_INFO( "              ----------+----------+-----------+  +----------+----------+----------------+");
		LOG_INFO( "                    max |      min |    current|  |      max |      min |        current |");
		LOG_INFO( "                   (ms) |     (ms) |       (ms)|  |  (bytes) |  (bytes) |        (bytes) |");
		LOG_INFO( "              ----------+----------+-----------+  +----------+----------+----------------+");
		LOG_INFO(LINE_MIN_MAX_FORMAT_STREAM, LINE_MIN_MAX_STREAM("stream",s));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("output",o));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("i2swrite",i2savailable));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("local free",loci2sbuf));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("requested",req));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("received",rec));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("overflow",over));
		LOG_INFO("              ----------+----------+-----------+  +----------+----------+----------------+");
		LOG_INFO("");
		LOG_INFO("              max (us)  | min (us) |current(us)|  ");
		LOG_INFO("              ----------+----------+-----------+  ");
		LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Buffering(us)",buffering));
		LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("i2s tfr(us)",i2s_time));
		LOG_INFO("              ----------+----------+-----------+  ");
		RESET_ALL_MIN_MAX;
		count=0;
		TIMED_SECTION_END;
		/*
		 * End Statistics reporting
		 */
		//wait_for_frames(BYTES_TO_FRAME(i2s_bytes_written));

		/*
		 * Statistics reporting
		 */
#define STATS_PERIOD_MS 5000
		count++;
		TIMED_SECTION_START_MS(STATS_PERIOD_MS);

		LOG_INFO( "count:%d, current sample rate: %d, bytes per frame: %d, avg cycle duration (ms): %d",count,output.current_sample_rate, out_bytes_per_frame,STATS_PERIOD_MS/count);
		LOG_INFO( "              ----------+----------+-----------+  +----------+----------+----------------+");
		LOG_INFO( "                    max |      min |    current|  |      max |      min |        current |");
		LOG_INFO( "                   (ms) |     (ms) |       (ms)|  |  (bytes) |  (bytes) |        (bytes) |");
		LOG_INFO( "              ----------+----------+-----------+  +----------+----------+----------------+");
		LOG_INFO(LINE_MIN_MAX_FORMAT_STREAM, LINE_MIN_MAX_STREAM("stream",s));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("output",o));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("i2swrite",i2savailable));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("local free",loci2sbuf));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("requested",req));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("received",rec));
		LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("overflow",over));
		LOG_INFO("              ----------+----------+-----------+  +----------+----------+----------------+");
		LOG_INFO("");
		LOG_INFO("              max (us)  | min (us) |current(us)|  ");
		LOG_INFO("              ----------+----------+-----------+  ");
		LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Buffering(us)",buffering));
		LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("i2s tfr(us)",i2s_time));
		LOG_INFO("              ----------+----------+-----------+  ");
		RESET_ALL_MIN_MAX;
		count=0;
		TIMED_SECTION_END;
		/*
		 * End Statistics reporting
		 */
		wait_for_frames(BYTES_TO_FRAME(i2s_bytes_written));
	}


	return 0;
}

bool test_open(const char *device, unsigned rates[], bool userdef_rates) {
	unsigned _rates[] = { 96000, 88200, 48000, 44100, 32000, 0 };	
	memcpy(rates, _rates, sizeof(_rates));
	return true;
}


