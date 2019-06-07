#include "squeezelite.h"
#include "perf_trace.h"

static log_level loglevel;

static bool running = true;

extern struct outputstate output;
extern struct buffer *outputbuf;
extern struct buffer *streambuf;


#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#ifdef USE_BT_RING_BUFFER
size_t bt_buffer_size=0;
uint8_t bt_buf_used_threshold = 25;
uint16_t output_bt_thread_heartbeat_ms=1000;
thread_type thread_bt;
#define LOCK_BT   mutex_lock(btbuf->mutex)
#define UNLOCK_BT mutex_unlock(btbuf->mutex)
thread_cond_type output_bt_suspend_cond;
mutex_type output_bt_suspend_mutex;
static struct buffer bt_buf_structure;
struct buffer *btbuf=&bt_buf_structure;
static void *output_thread_bt();
extern void wait_for_frames(size_t frames, uint8_t pct);
#else
uint8_t * btout;
#endif

#define FRAME_BLOCK MAX_SILENCE_FRAMES
extern u8_t *silencebuf;

void hal_bluetooth_init(log_level);

static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
#define DECLARE_ALL_MIN_MAX \
	DECLARE_MIN_MAX(req);\
	DECLARE_MIN_MAX(rec);\
	DECLARE_MIN_MAX(o);\
	DECLARE_MIN_MAX(s);\
	DECLARE_MIN_MAX(locbtbuff);\
	DECLARE_MIN_MAX(under);\
	DECLARE_MIN_MAX_DURATION(mutex1);\
	DECLARE_MIN_MAX_DURATION(mutex2);\
	DECLARE_MIN_MAX_DURATION(buffering);\
	DECLARE_MIN_MAX_DURATION(sleep_time);
#define RESET_ALL_MIN_MAX \
	RESET_MIN_MAX(o); \
	RESET_MIN_MAX(s); \
	RESET_MIN_MAX(locbtbuff); \
	RESET_MIN_MAX(req);  \
	RESET_MIN_MAX(rec);  \
	RESET_MIN_MAX(under);  \
	RESET_MIN_MAX_DURATION(mutex1);  \
	RESET_MIN_MAX_DURATION(mutex2);  \
	RESET_MIN_MAX_DURATION(sleep_time);  \
	RESET_MIN_MAX_DURATION(buffering);


#if CONFIG_BTAUDIO
void set_volume_bt(unsigned left, unsigned right) {
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	LOCK;
	output.gainL = left;
	output.gainR = right;
	UNLOCK;
}
#endif



void output_bt_check_buffer()
{
#ifdef USE_BT_RING_BUFFER
	LOCK_BT;
	uint8_t tot_buf_used_pct=100*_buf_used(btbuf)/btbuf->size;
	UNLOCK_BT;
	if(tot_buf_used_pct<bt_buf_used_threshold)
	{
		// tell the thread to resume
		LOG_SDEBUG("Below threshold. Locking suspend mutex.");
		mutex_lock(output_bt_suspend_mutex);
		LOG_SDEBUG("Broadcasting suspend condition.");
		mutex_broadcast_cond(output_bt_suspend_cond);
		LOG_SDEBUG("Unlocking suspend mutex.");
		mutex_unlock(output_bt_suspend_mutex);
	}
#endif
}
void output_bt_suspend()
{
#ifdef USE_BT_RING_BUFFER
	struct timespec   ts;
	struct timeval    tp;
	int               rc;


	// if suspended, suspend until resumed
	LOG_SDEBUG("Locking suspend mutex.");
	mutex_lock(output_bt_suspend_mutex);
	LOG_SDEBUG("Waiting on condition to be signaled.");

	// suspend for up to a predetermined wait time.
	// this will allow flushing the BT buffer when the
	// playback stops.
	gettimeofday(&tp, NULL);
	/* Convert from timeval to timespec */
	ts.tv_sec  = tp.tv_sec;
	ts.tv_nsec = tp.tv_usec * 1000;
	ts.tv_nsec +=  output_bt_thread_heartbeat_ms*1000000; // micro seconds to nanosecs

	rc = pthread_cond_timedwait(&output_bt_suspend_cond,&output_bt_suspend_mutex,&ts);
	if(rc==ETIMEDOUT)
	{
		LOG_SDEBUG("Wait timed out. Resuming output.");
	}
    LOG_SDEBUG("Unlocking suspend mutex.");
    mutex_unlock(output_bt_suspend_mutex);
#endif
}

void output_init_bt(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;

	LOG_INFO("init output BT");

	memset(&output, 0, sizeof(output));

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
#ifdef USE_BT_RING_BUFFER
	LOG_DEBUG("Allocating local BT transfer buffer of %u bytes.",bt_buffer_size);
	buf_init(btbuf, bt_buffer_size );
	if (!btbuf->buf) {
		LOG_ERROR("unable to malloc BT buffer");
		exit(0);
	}
	mutex_create_p(output_bt_suspend_mutex);
	mutex_cond_init(output_bt_suspend_cond);
	PTHREAD_SET_NAME("output_bt");

#if LINUX || OSX || FREEBSD || POSIX
	pthread_attr_t attr;
	pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
#endif

	pthread_create(&thread_bt, &attr, output_thread_bt, NULL);
#endif
	pthread_attr_destroy(&attr);

#if WIN
	thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&output_thread_bt, NULL, 0, NULL);
#endif
#else
	output.start_frames =  FRAME_BLOCK;
	output.write_cb = &_write_frames;
	output.rate_delay = rate_delay;
#endif
	LOG_INFO("Init completed.");

}

/****************************************************************************************
 * Main output thread
 */
#ifdef USE_BT_RING_BUFFER
static void *output_thread_bt() {

	frames_t frames=0;
	frames_t requested_frames=0;
	uint32_t timer_start=0, mutex_start=0;
	unsigned btbuf_used=0;
	output_state state;
	DECLARE_ALL_MIN_MAX;
	while (running) {
		frames=0;
		requested_frames=0;
		TIME_MEASUREMENT_START(timer_start);

		// Get output state
		TIME_MEASUREMENT_START(mutex_start);
		LOCK;
		state=output.state;
		SET_MIN_MAX(TIME_MEASUREMENT_GET(mutex_start),mutex1);

		if(state < OUTPUT_STOPPED ){
			// Flushing the buffer will automatically
			// lock the mutex
			LOG_SDEBUG("Flushing BT buffer");
			buf_flush(btbuf);
		}
		if (state == OUTPUT_OFF) {
			UNLOCK;
			LOG_SDEBUG("Output state is off.");
			usleep(200000);
			continue;
		}
		output.device_frames = 0; // todo: check if this is the right way do to this.
		output.updated = gettime_ms();
		output.frames_played_dmp = output.frames_played;

		TIME_MEASUREMENT_START(mutex_start);
		LOCK_BT;
		SET_MIN_MAX(TIME_MEASUREMENT_GET(mutex_start),mutex2);
		btbuf_used=_buf_used(btbuf);
		SET_MIN_MAX_SIZED(btbuf_used,locbtbuff,btbuf->size);


		// only output more frames if we need them
		// so we can release the mutex as quickly as possible
		requested_frames = min(_buf_space(btbuf), _buf_cont_write(btbuf))/BYTES_PER_FRAME;
		SET_MIN_MAX( requested_frames*BYTES_PER_FRAME,req);
		SET_MIN_MAX_SIZED(_buf_used(outputbuf),o,outputbuf->size);
		SET_MIN_MAX_SIZED(_buf_used(streambuf),s,streambuf->size);
		if(requested_frames>0)
		{
			frames = _output_frames( requested_frames ); // Keep the transfer buffer full
			SET_MIN_MAX(frames*BYTES_PER_FRAME,rec);
			if(requested_frames>frames){
				SET_MIN_MAX((requested_frames-frames)*BYTES_PER_FRAME,under);
			}
		}

		UNLOCK;
		UNLOCK_BT;
		SET_MIN_MAX( TIME_MEASUREMENT_GET(timer_start),buffering);
		SET_MIN_MAX( requested_frames,req);

		// When playback has started, we want to
		// hold the BT out thread
		// so the BT data callback isn't constantly interrupted.
		TIME_MEASUREMENT_START(timer_start);
		if(state>OUTPUT_BUFFER){
			output_bt_suspend();
		}
		SET_MIN_MAX(TIME_MEASUREMENT_GET(timer_start),sleep_time);

		/*
		 * Statistics reporting
		 */
		static time_t lastTime=0;
		if (lastTime <= gettime_ms() )
		{
#define STATS_PERIOD_MS 15000
			lastTime = gettime_ms() + STATS_PERIOD_MS;
			LOG_DEBUG(LINE_MIN_MAX_FORMAT_HEAD1);
			LOG_DEBUG(LINE_MIN_MAX_FORMAT_HEAD2);
			LOG_DEBUG(LINE_MIN_MAX_FORMAT_HEAD3);
			LOG_DEBUG(LINE_MIN_MAX_FORMAT_HEAD4);
			LOG_DEBUG(LINE_MIN_MAX_FORMAT_STREAM, LINE_MIN_MAX_STREAM("stream",s));
			LOG_DEBUG(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("output",o));
			LOG_DEBUG(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("local bt buf",locbtbuff));
			LOG_DEBUG(LINE_MIN_MAX_FORMAT_FOOTER );
			LOG_DEBUG(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("requested",req));
			LOG_DEBUG(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("received",rec));
			LOG_DEBUG(LINE_MIN_MAX_FORMAT,LINE_MIN_MAX("Underrun",under));
			LOG_DEBUG(LINE_MIN_MAX_FORMAT_FOOTER );
			LOG_DEBUG("");
			LOG_DEBUG("              ----------+----------+-----------+-----------+  ");
			LOG_DEBUG("              max (us)  | min (us) |   avg(us) |  count    |  ");
			LOG_DEBUG("              ----------+----------+-----------+-----------+  ");
			LOG_DEBUG(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Buffering(us)",buffering));
			LOG_DEBUG(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("Output mux(us)",mutex1));
			LOG_DEBUG(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("BT mux(us)",mutex2));
			LOG_DEBUG(LINE_MIN_MAX_DURATION_FORMAT,LINE_MIN_MAX_DURATION("sleep(us)",mutex2));
			LOG_DEBUG("              ----------+----------+-----------+-----------+");
			RESET_ALL_MIN_MAX;
		}
		/*
		 * End Statistics reporting
		 */



	}
	return NULL;
}
#endif
void output_close_bt(void) {
	LOG_INFO("close output");
	LOCK;
	running = false;
	UNLOCK;
#ifdef USE_BT_RING_BUFFER
	LOCK_BT;
	buf_destroy(btbuf);
	UNLOCK_BT;
#endif
	output_close_common();
}

static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
						 s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr) {
#ifdef USE_BT_RING_BUFFER
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
		_buf_inc_writep(btbuf,out_frames * BYTES_PER_FRAME);

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

	} else if(output.state >OUTPUT_BUFFER){
		// Don't fill our local buffer with silence frames.
		u8_t *buf = silencebuf;
		memcpy(btbuf->writep, buf, out_frames * BYTES_PER_FRAME);
		_buf_inc_writep(btbuf,out_frames * BYTES_PER_FRAME);
	}
#else
	assert(btout!=NULL);
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
		memcpy(btout, outputbuf->readp, out_frames * BYTES_PER_FRAME);
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

		u8_t *buf = silencebuf;
		memcpy(btout, buf, out_frames * BYTES_PER_FRAME);
	}
#endif

	return (int)out_frames;
}
