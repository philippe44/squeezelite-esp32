#ifndef _HAIRTUNES_H_
#define _HAIRTUNES_H_

#include "util.h"

typedef struct {
	unsigned short cport, tport, aport;
	struct rtp_s *ctx;
} rtp_resp_t;

typedef	void		(*rtp_data_cb_t)(const u8_t *data, size_t len);

rtp_resp_t 			rtp_init(struct in_addr host, bool sync, bool drift, bool range, int latency,
							char *aeskey, char *aesiv, char *fmtpstr,
							short unsigned pCtrlPort, short unsigned pTimingPort, rtp_data_cb_t data_cb);
void			 	rtp_end(struct rtp_s *ctx);
bool 				rtp_flush(struct rtp_s *ctx, unsigned short seqno, unsigned rtptime);
void 				rtp_record(struct rtp_s *ctx, unsigned short seqno, unsigned rtptime);
void 				rtp_metadata(struct rtp_s *ctx, struct metadata_s *metadata);

#endif
