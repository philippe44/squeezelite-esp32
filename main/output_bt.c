#include "squeezelite.h"
#include "perf_trace.h"

static log_level loglevel;

static bool running = true;

extern struct outputstate output;
extern struct buffer *outputbuf;
extern struct buffer *streambuf;
size_t bt_buffer_size=0;

static struct buffer bt_buf_structure;
struct buffer *btbuf=&bt_buf_structure;


#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)
#define LOCK_BT   mutex_lock(btbuf->mutex)
#define UNLOCK_BT mutex_unlock(btbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES
#define BUFFERING_FRAME_BLOCK FRAME_BLOCK*2

extern u8_t *silencebuf;

void hal_bluetooth_init(log_level);
static void *output_thread_bt();
static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
extern void wait_for_frames(size_t frames, uint8_t pct);

#define DECLARE_ALL_MIN_MAX \
	DECLARE_MIN_MAX(req, long,LONG); \
	DECLARE_MIN_MAX(rec, long,LONG); \
	DECLARE_MIN_MAX(o, long,LONG); \
	DECLARE_MIN_MAX(s, long,LONG); \
	DECLARE_MIN_MAX(d, long,LONG); \
	DECLARE_MIN_MAX(locbtbuff, long,LONG); \
	DECLARE_MIN_MAX(mutex1, long,LONG); \
	DECLARE_MIN_MAX(mutex2, long,LONG); \
	DECLARE_MIN_MAX(total, long,LONG); \
DECLARE_MIN_MAX(buffering, long,LONG);
#define RESET_ALL_MIN_MAX \
	RESET_MIN_MAX(d,LONG); \
	RESET_MIN_MAX(o,LONG); \
	RESET_MIN_MAX(s,LONG); \
	RESET_MIN_MAX(locbtbuff, LONG); \
	RESET_MIN_MAX(req,LONG);  \
	RESET_MIN_MAX(rec,LONG);  \
	RESET_MIN_MAX(mutex1,LONG);  \
	RESET_MIN_MAX(mutex2,LONG);  \
	RESET_MIN_MAX(total,LONG);  \
	RESET_MIN_MAX(buffering,LONG);


#if CONFIG_BTAUDIO
void set_volume_bt(unsigned left, unsigned right) {
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	LOCK;
	output.gainL = left;
	output.gainR = right;
	UNLOCK;
}
#endif

static thread_type thread_bt;
void output_init_bt(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;

	LOG_INFO("init output BT");

	memset(&output, 0, sizeof(output));

	output.start_frames = FRAME_BLOCK; //CONFIG_ //FRAME_BLOCK * 2;
	output.write_cb = &_write_frames;
	output.rate_delay = rate_delay;

	// ensure output rate is specified to avoid test open
	if (!rates[0]) {
		rates[0] = 44100;
	}
	hal_bluetooth_init(loglevel);
/*
 * Bluetooth audio source init Start
 */
	device = CONFIG_OUTPUT_NAME;
	output_init_common(level, device, output_buf_size, rates, idle);

	bt_buffer_size = 3*FRAME_BLOCK*get_bytes_per_frame(output.format);
	LOG_DEBUG("Allocating local BT transfer buffer of %u bytes.",bt_buffer_size);
	buf_init(btbuf, bt_buffer_size );
	if (!btbuf->buf) {
		LOG_ERROR("unable to malloc BT buffer");
		exit(0);
	}


#if LINUX || OSX || FREEBSD || POSIX
	pthread_attr_t attr;
	pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
#endif
	pthread_create(&thread_bt, &attr, output_thread_bt, NULL);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&output_thread_bt, NULL, 0, NULL);
#endif
	LOG_INFO("Init completed.");

}

/****************************************************************************************
 * Main output thread
 */
static void *output_thread_bt() {
	frames_t frames=0;
	frames_t available_frames_space=0;
	uint32_t timer_start=0, mutex_start=0, total_start=0;
	static int count = 0;

	DECLARE_ALL_MIN_MAX;

	while (running) {
		frames=0;
		available_frames_space=0;

		TIME_MEASUREMENT_START(timer_start);
		TIME_MEASUREMENT_START(total_start);
		TIME_MEASUREMENT_START(mutex_start);
		LOCK;
		SET_MIN_MAX(TIME_MEASUREMENT_GET(mutex_start),mutex1);
		if (output.state == OUTPUT_OFF) {
			UNLOCK;
			LOG_SDEBUG("Output state is off.");
			usleep(500000);
			continue;
		}
		TIME_MEASUREMENT_START(mutex_start);
		LOCK_BT;
		SET_MIN_MAX(TIME_MEASUREMENT_GET(mutex_start),mutex2);
		available_frames_space = min(_buf_space(btbuf), _buf_cont_write(btbuf))/BYTES_PER_FRAME;
		SET_MIN_MAX( available_frames_space,req);
		SET_MIN_MAX(_buf_used(outputbuf)/BYTES_PER_FRAME,o);
		SET_MIN_MAX(_buf_used(streambuf)/BYTES_PER_FRAME,s);
		if(available_frames_space==0)
		{
			UNLOCK;
			UNLOCK_BT;
			usleep(10000);
			continue;
		}
		frames = _output_frames( available_frames_space ); // Keep the transfer buffer full
		SET_MIN_MAX(_buf_used(btbuf),locbtbuff);
		UNLOCK;
		UNLOCK_BT;
		//LOG_SDEBUG("Current buffer free: %10d, cont read: %10d",_buf_space(btbuf),_buf_cont_read(btbuf));
		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),buffering);
		SET_MIN_MAX( available_frames_space,req);
		SET_MIN_MAX(frames,rec);
//		if(frames>0 ){
//			// let's hold processing a bit, while frames are being processed
//			// we're waiting long enough to avoid hogging the CPU too much
//			// while we're ramping up this transfer buffer
//			wait_for_frames(frames,95);
//		}
		wait_for_frames(FRAME_BLOCK,100);

		/*
		 * Statistics reporting
		 */
#define STATS_PERIOD_MS 10000
		count++;
		TIMED_SECTION_START_MS(STATS_PERIOD_MS);
		if(count>1){
			LOG_INFO( "count:%d, current sample rate: %d, avg cycle duration (ms): %d",count,output.current_sample_rate, STATS_PERIOD_MS/count);
			LOG_INFO( "              ----------+----------+-----------+-----------+  +----------+----------+----------------+----------------+----------------+");
			LOG_INFO( "                    max |      min |       avg |    current|  |      max |      min |        average |        count   |        current |");
			LOG_INFO( "                   (ms) |     (ms) |       (ms)|      (ms) |  |  (bytes) |  (bytes) |        (bytes) |                |        (bytes) |");
			LOG_INFO( "              ----------+----------+-----------+-----------+  +----------+----------+----------------+----------------+----------------+");
			LOG_INFO(LINE_MIN_MAX_FORMAT_STREAM, LINE_MIN_MAX_STREAM("stream",s));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("output",o));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("requested",req));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("received",rec));
			LOG_INFO(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("local bt buf",locbtbuff));
			LOG_INFO("              ----------+----------+-----------+-----------+  +----------+----------+----------------+----------------+");
			LOG_INFO("");
			LOG_INFO("              ----------+----------+-----------+-----------+-----------+  ");
			LOG_INFO("              max (us)  | min (us) |   avg(us) |  count    |current(us)|  ");
			LOG_INFO("              ----------+----------+-----------+-----------+-----------+  ");
			LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Buffering(us)",buffering));
			LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Output mux(us)",mutex1));
			LOG_INFO(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("BT mux(us)",mutex2));
			LOG_INFO("              ----------+----------+-----------+-----------+-----------+");
			RESET_ALL_MIN_MAX;
			count=0;
		}
		TIMED_SECTION_END;
		/*
		 * End Statistics reporting
		 */


	}


	return 0;
}
void output_close_bt(void) {
	LOG_INFO("close output");
	LOCK;
	running = false;
	UNLOCK;

	output_close_common();
}

static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
						 s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {

	if (!silence ) {
		DEBUG_LOG_TIMED(200,"Not silence, Writing audio out.");
		// TODO need 16 bit fix

		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}

		if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
			_apply_gain(outputbuf, out_frames, gainL, gainR);
		}

#if BYTES_PER_FRAME == 4

		memcpy(btbuf->writep, outputbuf->readp, out_frames * BYTES_PER_FRAME);

#else
	{
		frames_t count = out_frames;
		s32_t *_iptr = (s32_t*) outputbuf->readp;
		s16_t *_optr = (s16_t*) bt_optr;
		while (count--) {
			*_optr++ = *_iptr++ >> 16;
			*_optr++ = *_iptr++ >> 16;
		}
	}
#endif

	} else {
		DEBUG_LOG_TIMED(200,"Silence flag true. Writing silence to audio out.");

		u8_t *buf = silencebuf;
		memcpy(btbuf->writep, buf, out_frames * BYTES_PER_FRAME);
	}
	_buf_inc_writep(btbuf,out_frames * BYTES_PER_FRAME);
	return (int)out_frames;
}
