#include "squeezelite.h"


static log_level loglevel;

static bool running = true;

extern struct outputstate output;
extern struct buffer *outputbuf;
extern struct buffer *streambuf;


#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

#define FRAME_BLOCK MAX_SILENCE_FRAMES

extern u8_t *silencebuf;
extern u8_t *bt_optr;
void hal_bluetooth_init(log_level);
static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, ISAMPLE_T **cross_ptr);
#if BTAUDIO
void set_volume(unsigned left, unsigned right) {
	LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
	LOCK;
	output.gainL = left;
	output.gainR = right;
	// TODO
	output.gainL = FIXED_ONE;
	output.gainR = FIXED_ONE;
	UNLOCK;
}
#endif


void output_init_bt(log_level level, char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay, unsigned idle) {
	loglevel = level;

	LOG_INFO("init output BT");

	memset(&output, 0, sizeof(output));

	output.start_frames = 0; //CONFIG_ //FRAME_BLOCK * 2;
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
	device = "BT";
	output_init_common(level, device, output_buf_size, rates, idle);


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
		/* TODO need 16 bit fix 
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}
		
		if (gainL != FIXED_ONE || gainR!= FIXED_ONE) {
			_apply_gain(outputbuf, out_frames, gainL, gainR);
		}
		*/

#if BYTES_PER_FRAME == 4		
		memcpy(bt_optr, outputbuf->readp, out_frames * BYTES_PER_FRAME);
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

		memcpy(bt_optr, buf, out_frames * 4);
	}
	
	bt_optr += out_frames * 4;

	return (int)out_frames;
}

