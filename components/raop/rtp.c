/*
 * HairTunes - RAOP packet handler and slave-clocked replay engine
 * Copyright (c) James Laird 2011
 * All rights reserved.
 *
 * Modularisation: philippe_44@outlook.com, 2019
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdint.h>
#include <fcntl.h>
#include <assert.h>

#include "platform.h"
#include "rtp.h"
#include "log_util.h"
#include "util.h"

#ifdef WIN32
#include <openssl/aes.h>
#include "alac.h"
#else
#include "esp_pthread.h"
#include "esp_system.h"
#include <mbedtls/version.h>
#include <mbedtls/aes.h>
//#include "alac_wrapper.h"
#include "alac.h"
#endif

#define NTP2MS(ntp) ((((ntp) >> 10) * 1000L) >> 22)
#define MS2NTP(ms) (((((u64_t) (ms)) << 22) / 1000) << 10)
#define NTP2TS(ntp, rate) ((((ntp) >> 16) * (rate)) >> 16)
#define TS2NTP(ts, rate)  (((((u64_t) (ts)) << 16) / (rate)) << 16)
#define MS2TS(ms, rate) ((((u64_t) (ms)) * (rate)) / 1000)
#define TS2MS(ts, rate) NTP2MS(TS2NTP(ts,rate))

#define GAP_THRES	8
#define GAP_COUNT	20

extern log_level 	raop_loglevel;
static log_level 	*loglevel = &raop_loglevel;

//#define __RTP_STORE

// default buffer size
#define BUFFER_FRAMES (44100 / 352 + 1)
#define MAX_PACKET    1408

#define RTP_SYNC	(0x01)
#define NTP_SYNC	(0x02)

#define RESEND_TO	200

enum { DATA, CONTROL, TIMING };

static const u8_t silence_frame[MAX_PACKET] = { 0 };

typedef u16_t seq_t;
typedef struct audio_buffer_entry {   // decoded audio packets
	int ready;
	u32_t rtptime, last_resend;
	s16_t *data;
	int len;
} abuf_t;

typedef struct rtp_s {
#ifdef __RTP_STORE
	FILE *rtpIN, *rtpOUT;
#endif
	bool running;
	unsigned char aesiv[16];
#ifdef WIN32
	AES_KEY aes;
#else
	mbedtls_aes_context aes;
#endif
	bool decrypt, range;
	int frame_size, frame_duration;
	int in_frames, out_frames;
	struct in_addr host;
	struct sockaddr_in rtp_host;
	struct {
		unsigned short rport, lport;
		int sock;
	} rtp_sockets[3]; 					 // data, control, timing
	struct timing_s {
		bool drift;
		u64_t local, remote;
		u32_t count, gap_count;
		s64_t gap_sum, gap_adjust;
	} timing;
	struct {
		u32_t 	rtp, time;
		u8_t  	status;
		bool	first, required;
	} synchro;
	struct {
		u32_t time;
		seq_t seqno;
		u32_t rtptime;
	} record;
	int latency;			// rtp hold depth in samples
	u32_t resent_frames;	// total recovered frames
	u32_t silent_frames;	// total silence frames
	u32_t filled_frames;    // silence frames in current silence episode
	int skip;				// number of frames to skip to keep sync alignement
	abuf_t audio_buffer[BUFFER_FRAMES];
	seq_t ab_read, ab_write;
	pthread_mutex_t ab_mutex;
#ifdef WIN32
	pthread_t rtp_thread;
#else
	TaskHandle_t rtp_thread, joiner;
#endif
	alac_file *alac_codec;
	int flush_seqno;
	bool playing;
	rtp_data_cb_t callback;
} rtp_t;


#define BUFIDX(seqno) ((seq_t)(seqno) % BUFFER_FRAMES)
static void 	buffer_alloc(abuf_t *audio_buffer, int size);
static void 	buffer_release(abuf_t *audio_buffer);
static void 	buffer_reset(abuf_t *audio_buffer);
static void 	buffer_push_packet(rtp_t *ctx);
static bool 	rtp_request_resend(rtp_t *ctx, seq_t first, seq_t last);
static bool 	rtp_request_timing(rtp_t *ctx);
static void*	rtp_thread_func(void *arg);
static int	  	seq_order(seq_t a, seq_t b);

/*---------------------------------------------------------------------------*/
static alac_file* alac_init(int fmtp[32]) {
	alac_file *alac;
	int sample_size = fmtp[3];

	if (sample_size != 16) {
		LOG_ERROR("sample size must be 16 %d", sample_size);
		return false;
	}

	alac = create_alac(sample_size, 2);

	if (!alac) {
		LOG_ERROR("cannot create alac codec", NULL);
		return NULL;
	}

	alac->setinfo_max_samples_per_frame = fmtp[1];
	alac->setinfo_7a 				= fmtp[2];
	alac->setinfo_sample_size 		= sample_size;
	alac->setinfo_rice_historymult = fmtp[4];
	alac->setinfo_rice_initialhistory = fmtp[5];
	alac->setinfo_rice_kmodifier 	= fmtp[6];
	alac->setinfo_7f 				= fmtp[7];
	alac->setinfo_80 				= fmtp[8];
	alac->setinfo_82 			    = fmtp[9];
	alac->setinfo_86 				= fmtp[10];
	alac->setinfo_8a_rate			= fmtp[11];
	allocate_buffers(alac);

	return alac;
}

/*---------------------------------------------------------------------------*/
rtp_resp_t rtp_init(struct in_addr host, bool sync, bool drift, bool range,
								int latency, char *aeskey, char *aesiv, char *fmtpstr,
								short unsigned pCtrlPort, short unsigned pTimingPort,
								rtp_data_cb_t callback)
{
	int i = 0;
	char *arg;
	int fmtp[12];
	bool rc = true;
	rtp_t *ctx = calloc(1, sizeof(rtp_t));
	rtp_resp_t resp = { 0, 0, 0, NULL };

	if (!ctx) return resp;

	ctx->host = host;
	ctx->decrypt = false;
	ctx->callback = callback;
	ctx->rtp_host.sin_family = AF_INET;
	ctx->rtp_host.sin_addr.s_addr = INADDR_ANY;
	pthread_mutex_init(&ctx->ab_mutex, 0);
	ctx->flush_seqno = -1;
	ctx->latency = latency;
	ctx->synchro.required = sync;
	ctx->timing.drift = drift;
	ctx->range = range;

	// write pointer = last written, read pointer = next to read so fill = w-r+1
	ctx->ab_read = ctx->ab_write + 1;

#ifdef __RTP_STORE
	ctx->rtpIN = fopen("airplay.rtpin", "wb");
	ctx->rtpOUT = fopen("airplay.rtpout", "wb");
#endif

	ctx->rtp_sockets[CONTROL].rport = pCtrlPort;
	ctx->rtp_sockets[TIMING].rport = pTimingPort;

	if (aesiv && aeskey) {
		memcpy(ctx->aesiv, aesiv, 16);
#ifdef WIN32
		AES_set_decrypt_key((unsigned char*) aeskey, 128, &ctx->aes);
#else
		memset(&ctx->aes, 0, sizeof(mbedtls_aes_context));
		mbedtls_aes_setkey_dec(&ctx->aes, (unsigned char*) aeskey, 128);
#endif
		ctx->decrypt = true;
	}

	memset(fmtp, 0, sizeof(fmtp));
	while ((arg = strsep(&fmtpstr, " \t")) != NULL) fmtp[i++] = atoi(arg);

	ctx->frame_size = fmtp[1];
	ctx->frame_duration = (ctx->frame_size * 1000) / 44100;

	// alac decoder
	ctx->alac_codec = alac_init(fmtp);
	rc &= ctx->alac_codec != NULL;

	buffer_alloc(ctx->audio_buffer, ctx->frame_size*4);

	// create rtp ports
	for (i = 0; i < 3; i++) {
		ctx->rtp_sockets[i].sock = bind_socket(&ctx->rtp_sockets[i].lport, SOCK_DGRAM);
		rc &= ctx->rtp_sockets[i].sock > 0;
	}

	// create http port and start listening
	resp.cport = ctx->rtp_sockets[CONTROL].lport;
	resp.tport = ctx->rtp_sockets[TIMING].lport;
	resp.aport = ctx->rtp_sockets[DATA].lport;

	if (rc) {
		ctx->running = true;
#ifdef WIN32
		pthread_create(&ctx->rtp_thread, NULL, rtp_thread_func, (void *) ctx);
#else
		xTaskCreate((TaskFunction_t) rtp_thread_func, "RTP_thread", 4096, ctx, configMAX_PRIORITIES - 3, &ctx->rtp_thread);
#endif
	} else {
		rtp_end(ctx);
		ctx = NULL;
	}

	resp.ctx = ctx;

	return resp;
}

/*---------------------------------------------------------------------------*/
void rtp_end(rtp_t *ctx)
{
	int i;

	if (!ctx) return;

	if (ctx->running) {
		ctx->running = false;
#ifdef WIN32
		pthread_join(ctx->rtp_thread, NULL);
#else
		ctx->joiner = xTaskGetCurrentTaskHandle();
		xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
#endif
	}

	for (i = 0; i < 3; i++) shutdown_socket(ctx->rtp_sockets[i].sock);

	delete_alac(ctx->alac_codec);

	buffer_release(ctx->audio_buffer);
	free(ctx);

#ifdef __RTP_STORE
	fclose(ctx->rtpIN);
	fclose(ctx->rtpOUT);
#endif
}

/*---------------------------------------------------------------------------*/
bool rtp_flush(rtp_t *ctx, unsigned short seqno, unsigned int rtptime)
{
	bool rc = true;
	u32_t now = gettime_ms();

	if (now < ctx->record.time + 250 || (ctx->record.seqno == seqno && ctx->record.rtptime == rtptime)) {
		rc = false;
		LOG_ERROR("[%p]: FLUSH ignored as same as RECORD (%hu - %u)", ctx, seqno, rtptime);
	} else {
		pthread_mutex_lock(&ctx->ab_mutex);
		buffer_reset(ctx->audio_buffer);
		ctx->playing = false;
		ctx->flush_seqno = seqno;
		ctx->synchro.first = false;
		pthread_mutex_unlock(&ctx->ab_mutex);
	}

	LOG_INFO("[%p]: flush %hu %u", ctx, seqno, rtptime);

	return rc;
}

/*---------------------------------------------------------------------------*/
void rtp_record(rtp_t *ctx, unsigned short seqno, unsigned rtptime)
{
	ctx->record.seqno = seqno;
	ctx->record.rtptime = rtptime;
	ctx->record.time = gettime_ms();

	LOG_INFO("[%p]: record %hu %u", ctx, seqno, rtptime);
}

/*---------------------------------------------------------------------------*/
static void buffer_alloc(abuf_t *audio_buffer, int size) {
	int i;
	for (i = 0; i < BUFFER_FRAMES; i++) {
		audio_buffer[i].data = malloc(size);
		audio_buffer[i].ready = 0;
	}
}

/*---------------------------------------------------------------------------*/
static void buffer_release(abuf_t *audio_buffer) {
	int i;
	for (i = 0; i < BUFFER_FRAMES; i++) {
		free(audio_buffer[i].data);
	}
}

/*---------------------------------------------------------------------------*/
static void buffer_reset(abuf_t *audio_buffer) {
	int i;
	for (i = 0; i < BUFFER_FRAMES; i++) audio_buffer[i].ready = 0;
}

/*---------------------------------------------------------------------------*/
// the sequence numbers will wrap pretty often.
// this returns true if the second arg is after the first
static int seq_order(seq_t a, seq_t b) {
	s16_t d = b - a;
	return d > 0;
}

/*---------------------------------------------------------------------------*/
static void alac_decode(rtp_t *ctx, s16_t *dest, char *buf, int len, int *outsize) {
	unsigned char packet[MAX_PACKET];
	unsigned char iv[16];
	int aeslen;
	assert(len<=MAX_PACKET);

	if (ctx->decrypt) {
		aeslen = len & ~0xf;
		memcpy(iv, ctx->aesiv, sizeof(iv));
#ifdef WIN32
		AES_cbc_encrypt((unsigned char*)buf, packet, aeslen, &ctx->aes, iv, AES_DECRYPT);
#else
		mbedtls_aes_crypt_cbc(&ctx->aes, MBEDTLS_AES_DECRYPT, aeslen, iv, (unsigned char*) buf, packet);
#endif
		memcpy(packet+aeslen, buf+aeslen, len-aeslen);
		decode_frame(ctx->alac_codec, packet, dest, outsize);
	} else decode_frame(ctx->alac_codec, (unsigned char*) buf, dest, outsize);
}


/*---------------------------------------------------------------------------*/
static void buffer_put_packet(rtp_t *ctx, seq_t seqno, unsigned rtptime, bool first, char *data, int len) {
	abuf_t *abuf = NULL;

	pthread_mutex_lock(&ctx->ab_mutex);

	if (!ctx->playing) {
		if ((ctx->flush_seqno == -1 || seq_order(ctx->flush_seqno, seqno)) &&
		   ((ctx->synchro.required && ctx->synchro.first) || !ctx->synchro.required)) {
			ctx->ab_write = seqno-1;
			ctx->ab_read = seqno;
			ctx->skip = 0;
			ctx->flush_seqno = -1;
			ctx->playing = true;
			ctx->synchro.first = false;
			ctx->resent_frames = ctx->silent_frames = 0;
		} else {
			pthread_mutex_unlock(&ctx->ab_mutex);
			return;
		}
	}

	if (seqno == ctx->ab_write+1) {
		// expected packet
		abuf = ctx->audio_buffer + BUFIDX(seqno);
		ctx->ab_write = seqno;
		LOG_SDEBUG("packet expected seqno:%hu rtptime:%u (W:%hu R:%hu)", seqno, rtptime, ctx->ab_write, ctx->ab_read);
	} else if (seq_order(ctx->ab_write, seqno)) {
		// newer than expected
		if (seqno - ctx->ab_write - 1 > ctx->latency / ctx->frame_size) {
			// only get rtp latency-1 frames back (last one is seqno)
			LOG_WARN("[%p] too many missing frames %hu", ctx, seqno - ctx->ab_write - 1);
			ctx->ab_write = seqno - ctx->latency / ctx->frame_size;
		}
		if (seqno - ctx->ab_read + 1 > ctx->latency / ctx->frame_size) {
			// if ab_read is lagging more than http latency, advance it
			LOG_WARN("[%p] on hold for too long %hu", ctx, seqno - ctx->ab_read + 1);
			ctx->ab_read = seqno - ctx->latency / ctx->frame_size + 1;
		}
		if (rtp_request_resend(ctx, ctx->ab_write + 1, seqno-1)) {
			seq_t i;
			u32_t now = gettime_ms();
			for (i = ctx->ab_write + 1; i <= seqno-1; i++) {
				ctx->audio_buffer[BUFIDX(i)].rtptime = rtptime - (seqno-i)*ctx->frame_size;
				ctx->audio_buffer[BUFIDX(i)].last_resend = now;
			}
		}
		LOG_DEBUG("[%p]: packet newer seqno:%hu rtptime:%u (W:%hu R:%hu)", ctx, seqno, rtptime, ctx->ab_write, ctx->ab_read);
		abuf = ctx->audio_buffer + BUFIDX(seqno);
		ctx->ab_write = seqno;
	} else if (seq_order(ctx->ab_read, seqno + 1)) {
		// recovered packet, not yet sent
		abuf = ctx->audio_buffer + BUFIDX(seqno);
		LOG_DEBUG("[%p]: packet recovered seqno:%hu rtptime:%u (W:%hu R:%hu)", ctx, seqno, rtptime, ctx->ab_write, ctx->ab_read);
	} else {
		// too late
		LOG_DEBUG("[%p]: packet too late seqno:%hu rtptime:%u (W:%hu R:%hu)", ctx, seqno, rtptime, ctx->ab_write, ctx->ab_read);
	}

	if (ctx->in_frames++ > 1000) {
		LOG_INFO("[%p]: fill [level:%hd] [W:%hu R:%hu]", ctx, (seq_t) (ctx->ab_write - ctx->ab_read + 1), ctx->ab_write, ctx->ab_read);
		ctx->in_frames = 0;
	}

	if (abuf) {
		alac_decode(ctx, abuf->data, data, len, &abuf->len);
		abuf->ready = 1;
		// this is the local rtptime when this frame is expected to play
		abuf->rtptime = rtptime;
#ifdef __RTP_STORE
		fwrite(data, len, 1, ctx->rtpIN);
		fwrite(abuf->data, abuf->len, 1, ctx->rtpOUT);
#endif
	}

	buffer_push_packet(ctx);

	pthread_mutex_unlock(&ctx->ab_mutex);
}

/*---------------------------------------------------------------------------*/
// push as many frames as possible through callback
static void buffer_push_packet(rtp_t *ctx) {
	abuf_t *curframe = NULL;
	u32_t now, playtime;
	int i;

	// not ready to play yet
	if (!ctx->playing ||  ctx->synchro.status != (RTP_SYNC | NTP_SYNC)) return;

	// maybe re-evaluate time in loop in case data callback blocks ...
	now = gettime_ms();

	// there is always at least one frame in the buffer
	do {

		curframe = ctx->audio_buffer + BUFIDX(ctx->ab_read);
		playtime = ctx->synchro.time + (((s32_t)(curframe->rtptime - ctx->synchro.rtp)) * 1000) / 44100;

		/*
		if (now > playtime + ctx->frame_duration) {
			//LOG_INFO("[%p]: discarded frame (W:%hu R:%hu)", ctx, ctx->ab_write, ctx->ab_read);
		} else if (curframe->ready) {
			ctx->callback((const u8_t*) curframe->data, curframe->len);
		} else if (now >= playtime) {
			LOG_DEBUG("[%p]: created zero frame (W:%hu R:%hu)", ctx, ctx->ab_write, ctx->ab_read);
			ctx->callback(silence_frame, ctx->frame_size * 4);
			ctx->silent_frames++;
		} else break;
		*/

		if (curframe->ready) {
			ctx->callback((const u8_t*) curframe->data, curframe->len);
		} else if (now >= playtime) {
			LOG_DEBUG("[%p]: created zero frame (W:%hu R:%hu)", ctx, ctx->ab_write, ctx->ab_read);
			ctx->callback(silence_frame, ctx->frame_size * 4);
			ctx->silent_frames++;
		} else break;

		ctx->ab_read++;
		ctx->out_frames++;

	} while (ctx->ab_write - ctx->ab_read + 1 > 0);

	if (ctx->out_frames > 1000) {
		LOG_INFO("[%p]: drain [level:%hd gap:%d] [W:%hu R:%hu] [R:%u S:%u F:%u]",
				ctx, ctx->ab_write - ctx->ab_read, playtime - now, ctx->ab_write, ctx->ab_read,
				ctx->resent_frames, ctx->silent_frames, ctx->filled_frames);
		ctx->out_frames = 0;
	}

	LOG_SDEBUG("playtime %u %d [W:%hu R:%hu] %d", playtime, playtime - now, ctx->ab_write, ctx->ab_read, curframe->ready);

	// each missing packet will be requested up to (latency_frames / 16) times
	for (i = 16; seq_order(ctx->ab_read + i, ctx->ab_write); i += 16) {
		abuf_t *frame = ctx->audio_buffer + BUFIDX(ctx->ab_read + i);
		if (!frame->ready && now - frame->last_resend > RESEND_TO) {
			rtp_request_resend(ctx, ctx->ab_read + i, ctx->ab_read + i);
			frame->last_resend = now;
		}
	}
}


/*---------------------------------------------------------------------------*/
static void *rtp_thread_func(void *arg) {
	fd_set fds;
	int i, sock = -1;
	int count = 0;
	bool ntp_sent;
	char *packet = malloc(MAX_PACKET);
	rtp_t *ctx = (rtp_t*) arg;

	for (i = 0; i < 3; i++) {
		if (ctx->rtp_sockets[i].sock > sock) sock = ctx->rtp_sockets[i].sock;
		// send synchro requets 3 times
		ntp_sent = rtp_request_timing(ctx);
	}

	while (ctx->running) {
		ssize_t plen;
		char type;
		socklen_t rtp_client_len = sizeof(struct sockaddr_storage);
		int idx = 0;
		char *pktp = packet;
		struct timeval timeout = {0, 50*1000};

		FD_ZERO(&fds);
		for (i = 0; i < 3; i++)	{ FD_SET(ctx->rtp_sockets[i].sock, &fds); }

		if (select(sock + 1, &fds, NULL, NULL, &timeout) <= 0) continue;

		for (i = 0; i < 3; i++)
			if (FD_ISSET(ctx->rtp_sockets[i].sock, &fds)) idx = i;

		plen = recvfrom(ctx->rtp_sockets[idx].sock, packet, MAX_PACKET, 0, (struct sockaddr*) &ctx->rtp_host, &rtp_client_len);

		if (!ntp_sent) {
			LOG_WARN("[%p]: NTP request not send yet", ctx);
			ntp_sent = rtp_request_timing(ctx);
		}

		if (plen < 0) continue;
		assert(plen <= MAX_PACKET);

		type = packet[1] & ~0x80;
		pktp = packet;

		switch (type) {
			seq_t seqno;
			unsigned rtptime;

			// re-sent packet
			case 0x56: {
				pktp += 4;
				plen -= 4;
			}

			// data packet
			case 0x60: {
				seqno = ntohs(*(u16_t*)(pktp+2));
				rtptime = ntohl(*(u32_t*)(pktp+4));

				// adjust pointer and length
				pktp += 12;
				plen -= 12;

				LOG_SDEBUG("[%p]: seqno:%hu rtp:%u (type: %x, first: %u)", ctx, seqno, rtptime, type, packet[1] & 0x80);

				// check if packet contains enough content to be reasonable
				if (plen < 16) break;

				if ((packet[1] & 0x80) && (type != 0x56)) {
					LOG_INFO("[%p]: 1st audio packet received", ctx);
				}

				buffer_put_packet(ctx, seqno, rtptime, packet[1] & 0x80, pktp, plen);

				break;
			}

			// sync packet
			case 0x54: {
				u32_t rtp_now_latency = ntohl(*(u32_t*)(pktp+4));
				u64_t remote = (((u64_t) ntohl(*(u32_t*)(pktp+8))) << 32) + ntohl(*(u32_t*)(pktp+12));
				u32_t rtp_now = ntohl(*(u32_t*)(pktp+16));

				pthread_mutex_lock(&ctx->ab_mutex);

				// re-align timestamp and expected local playback time
				if (!ctx->latency) ctx->latency = rtp_now - rtp_now_latency;
				ctx->synchro.rtp = rtp_now - ctx->latency;
				ctx->synchro.time = ctx->timing.local + (u32_t) NTP2MS(remote - ctx->timing.remote);

				// now we are synced on RTP frames
				ctx->synchro.status |= RTP_SYNC;

				// 1st sync packet received (signals a restart of playback)
				if (packet[0] & 0x10) {
					ctx->synchro.first = true;
					LOG_INFO("[%p]: 1st sync packet received", ctx);
				}

				pthread_mutex_unlock(&ctx->ab_mutex);

				LOG_DEBUG("[%p]: sync packet rtp_latency:%u rtp:%u remote ntp:%Lx, local time %u (now:%u)",
						  ctx, rtp_now_latency, rtp_now, remote, ctx->synchro.time, gettime_ms());

				if (!count--) {
					rtp_request_timing(ctx);
					count = 3;
				}

				break;
			}

			// NTP timing packet
			case 0x53: {
				u64_t expected;
				s64_t delta = 0;
				u32_t reference   = ntohl(*(u32_t*)(pktp+12)); // only low 32 bits in our case
				u64_t remote 	  =(((u64_t) ntohl(*(u32_t*)(pktp+16))) << 32) + ntohl(*(u32_t*)(pktp+20));
				u32_t roundtrip   = gettime_ms() - reference;

				// better discard sync packets when roundtrip is suspicious
				if (roundtrip > 100) {
					LOG_WARN("[%p]: discarding NTP roundtrip of %u ms", ctx, roundtrip);
					break;
				}

				/*
				  The expected elapsed remote time should be exactly the same as
				  elapsed local time between the two request, corrected by the
				  drifting
				*/
				expected = ctx->timing.remote + MS2NTP(reference - ctx->timing.local);

				ctx->timing.remote = remote;
				ctx->timing.local = reference;
				ctx->timing.count++;

				if (!ctx->timing.drift && (ctx->synchro.status & NTP_SYNC)) {
					delta = NTP2MS((s64_t) expected - (s64_t) ctx->timing.remote);
					ctx->timing.gap_sum += delta;

					pthread_mutex_lock(&ctx->ab_mutex);

					/*
					 if expected time is more than remote, then our time is
					 running faster and we are transmitting frames too quickly,
					 so we'll run out of frames, need to add one
					*/
					if (ctx->timing.gap_sum > GAP_THRES && ctx->timing.gap_count++ > GAP_COUNT) {
						LOG_INFO("[%p]: Sending packets too fast %Ld [W:%hu R:%hu]", ctx, ctx->timing.gap_sum, ctx->ab_write, ctx->ab_read);
						ctx->ab_read--;
						ctx->audio_buffer[BUFIDX(ctx->ab_read)].ready = 1;
						ctx->timing.gap_sum -= GAP_THRES;
						ctx->timing.gap_adjust -= GAP_THRES;
					/*
					 if expected time is less than remote, then our time is
					 running slower and we are transmitting frames too slowly,
					 so we'll overflow frames buffer, need to remove one
					*/
					} else if (ctx->timing.gap_sum < -GAP_THRES && ctx->timing.gap_count++ > GAP_COUNT) {
						if (seq_order(ctx->ab_read, ctx->ab_write + 1)) {
							ctx->audio_buffer[BUFIDX(ctx->ab_read)].ready = 0;
							ctx->ab_read++;
						} else ctx->skip++;
						ctx->timing.gap_sum += GAP_THRES;
						ctx->timing.gap_adjust += GAP_THRES;
						LOG_INFO("[%p]: Sending packets too slow %Ld (skip: %d)  [W:%hu R:%hu]", ctx, ctx->timing.gap_sum, ctx->skip, ctx->ab_write, ctx->ab_read);
					}

					if (llabs(ctx->timing.gap_sum) < 8) ctx->timing.gap_count = 0;

					pthread_mutex_unlock(&ctx->ab_mutex);
				}

				// now we are synced on NTP (mutex not needed)
				ctx->synchro.status |= NTP_SYNC;

				LOG_DEBUG("[%p]: Timing references local:%Lu, remote:%Lx (delta:%Ld, sum:%Ld, adjust:%Ld, gaps:%d)",
						  ctx, ctx->timing.local, ctx->timing.remote, delta, ctx->timing.gap_sum, ctx->timing.gap_adjust, ctx->timing.gap_count);

				break;
			}
		}
	}

	free(packet);
	LOG_INFO("[%p]: terminating", ctx);

#ifndef WIN32
	xTaskNotify(ctx->joiner, 0, eNoAction);
	vTaskDelete(NULL);
#endif

	return NULL;
}

/*---------------------------------------------------------------------------*/
static bool rtp_request_timing(rtp_t *ctx) {
	unsigned char req[32];
	u32_t now = gettime_ms();
	int i;
	struct sockaddr_in host;

	LOG_DEBUG("[%p]: timing request now:%u (port: %hu)", ctx, now, ctx->rtp_sockets[TIMING].rport);

	req[0] = 0x80;
	req[1] = 0x52|0x80;
	*(u16_t*)(req+2) = htons(7);
	*(u32_t*)(req+4) = htonl(0);  // dummy
	for (i = 0; i < 16; i++) req[i+8] = 0;
	*(u32_t*)(req+24) = 0;
	*(u32_t*)(req+28) = htonl(now); // this is not a real NTP, but a 32 ms counter in the low part of the NTP

	if (ctx->host.s_addr != INADDR_ANY) {
		host.sin_family = AF_INET;
		host.sin_addr =	ctx->host;
	} else host = ctx->rtp_host;

	// no address from sender, need to wait for 1st packet to be received
	if (host.sin_addr.s_addr == INADDR_ANY) return false;

	host.sin_port = htons(ctx->rtp_sockets[TIMING].rport);

	if (sizeof(req) != sendto(ctx->rtp_sockets[TIMING].sock, req, sizeof(req), 0, (struct sockaddr*) &host, sizeof(host))) {
		LOG_WARN("[%p]: SENDTO failed (%s)", ctx, strerror(errno));
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static bool rtp_request_resend(rtp_t *ctx, seq_t first, seq_t last) {
	unsigned char req[8];    // *not* a standard RTCP NACK

	// do not request silly ranges (happens in case of network large blackouts)
	if (seq_order(last, first) || last - first > BUFFER_FRAMES / 2) return false;

	ctx->resent_frames += last - first + 1;

	LOG_DEBUG("resend request [W:%hu R:%hu first=%hu last=%hu]", ctx->ab_write, ctx->ab_read, first, last);

	req[0] = 0x80;
	req[1] = 0x55|0x80;  // Apple 'resend'
	*(u16_t*)(req+2) = htons(1);  // our seqnum
	*(u16_t*)(req+4) = htons(first);  // missed seqnum
	*(u16_t*)(req+6) = htons(last-first+1);  // count

	ctx->rtp_host.sin_port = htons(ctx->rtp_sockets[CONTROL].rport);

	if (sizeof(req) != sendto(ctx->rtp_sockets[CONTROL].sock, req, sizeof(req), 0, (struct sockaddr*) &ctx->rtp_host, sizeof(ctx->rtp_host))) {
		LOG_WARN("[%p]: SENDTO failed (%s)", ctx, strerror(errno));
	}

	return true;
}


#if 0
/*---------------------------------------------------------------------------*/
// get the next frame, when available. return 0 if underrun/stream reset.
static short *_buffer_get_frame(rtp_t *ctx, int *len) {
	short buf_fill;
	abuf_t *curframe = 0;
	int i;
	u32_t now, playtime;

	if (!ctx->playing) return NULL;

	// skip frames if we are running late and skip could not be done in SYNC
	while (ctx->skip && seq_order(ctx->ab_read, ctx->ab_write + 1)) {
		ctx->audio_buffer[BUFIDX(ctx->ab_read)].ready = 0;
		ctx->ab_read++;
		ctx->skip--;
		LOG_INFO("[%p]: Sending packets too slow (skip: %d) [W:%hu R:%hu]", ctx, ctx->skip, ctx->ab_write, ctx->ab_read);
	}

	buf_fill = ctx->ab_write - ctx->ab_read + 1;

	if (buf_fill >= BUFFER_FRAMES) {
		LOG_ERROR("[%p]: Buffer overrun %hu", ctx, buf_fill);
		ctx->ab_read = ctx->ab_write - (BUFFER_FRAMES - 64);
		buf_fill = ctx->ab_write - ctx->ab_read + 1;
	}

	now = gettime_ms();
	curframe = ctx->audio_buffer + BUFIDX(ctx->ab_read);

	// use next frame when buffer is empty or silence continues to be sent
	if (!buf_fill) curframe->rtptime = ctx->audio_buffer[BUFIDX(ctx->ab_read - 1)].rtptime + ctx->frame_size;

	playtime = ctx->synchro.time + (((s32_t)(curframe->rtptime - ctx->synchro.rtp))*1000)/44100;

	LOG_SDEBUG("playtime %u %d [W:%hu R:%hu] %d", playtime, playtime - now, ctx->ab_write, ctx->ab_read, curframe->ready);

	// wait if not ready but have time, otherwise send silence
	if (!buf_fill || ctx->synchro.status != (RTP_SYNC | NTP_SYNC) || (now < playtime && !curframe->ready)) {
		LOG_SDEBUG("[%p]: waiting (fill:%hd, W:%hu R:%hu) now:%u, playtime:%u, wait:%d", ctx, buf_fill, ctx->ab_write, ctx->ab_read, now, playtime, playtime - now);
		// look for "blocking" frames at the top of the queue and try to catch-up
		for (i = 0; i < min(16, buf_fill); i++) {
			abuf_t *frame = ctx->audio_buffer + BUFIDX(ctx->ab_read + i);
			if (!frame->ready && now - frame->last_resend > RESEND_TO) {
				rtp_request_resend(ctx, ctx->ab_read + i, ctx->ab_read + i);
				frame->last_resend = now;
			}
		}
		return NULL;
	}

	// when silence is inserted at the top, need to move write pointer
	if (!buf_fill) {
		if (!ctx->filled_frames) {
			LOG_WARN("[%p]: start silence (late %d ms) [W:%hu R:%hu]", ctx, now - playtime, ctx->ab_write, ctx->ab_read);
		}
		ctx->ab_write++;
		ctx->filled_frames++;
	} else ctx->filled_frames = 0;

	if (!(ctx->out_frames++ & 0x1ff)) {
		LOG_INFO("[%p]: drain [level:%hd gap:%d] [W:%hu R:%hu] [R:%u S:%u F:%u]",
					ctx, buf_fill-1, playtime - now, ctx->ab_write, ctx->ab_read,
					ctx->resent_frames, ctx->silent_frames, ctx->filled_frames);
	}

	// each missing packet will be requested up to (latency_frames / 16) times
	for (i = 16; seq_order(ctx->ab_read + i, ctx->ab_write); i += 16) {
		abuf_t *frame = ctx->audio_buffer + BUFIDX(ctx->ab_read + i);
		if (!frame->ready && now - frame->last_resend > RESEND_TO) {
			rtp_request_resend(ctx, ctx->ab_read + i, ctx->ab_read + i);
			frame->last_resend = now;
		}
	}

	if (!curframe->ready) {
		LOG_DEBUG("[%p]: created zero frame (W:%hu R:%hu)", ctx, ctx->ab_write, ctx->ab_read);
		memset(curframe->data, 0, ctx->frame_size*4);
		curframe->len = ctx->frame_size * 4;
		ctx->silent_frames++;
	} else {
		LOG_SDEBUG("[%p]: prepared frame (fill:%hd, W:%hu R:%hu)", ctx, buf_fill-1, ctx->ab_write, ctx->ab_read);
	}

	*len = curframe->len;
	curframe->ready = 0;
	ctx->ab_read++;

	return curframe->data;
}
#endif




