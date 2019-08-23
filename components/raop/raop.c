/*
 *
 *  (c) Philippe 2019, philippe_44@outlook.com
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

#include <stdio.h>

#include "platform.h"

#ifdef WIN32
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/engine.h>
#include "mdns.h"
#include "mdnsd.h"
#include "mdnssd-itf.h"
#else
#include "esp_pthread.h"
#include "mdns.h"
#include "mbedtls/version.h"
#include <mbedtls/x509.h>
#endif

#include "util.h"
#include "raop.h"
#include "rtp.h"
#include "dmap_parser.h"
#include "log_util.h"

typedef struct raop_ctx_s {
#ifdef WIN32
	struct mdns_service *svc;
	struct mdnsd *svr;
#endif
	struct in_addr host;	// IP of bridge
	short unsigned port;    // RTSP port for AirPlay
	int sock;               // socket of the above
	struct in_addr peer;	// IP of the iDevice (airplay sender)
	bool running;
#ifdef WIN32
	pthread_t thread, search_thread;
#else
	TaskHandle_t thread, search_thread, joiner;
#endif
	unsigned char mac[6];
	int latency;
	struct {
		char *aesiv, *aeskey;
		char *fmtp;
	} rtsp;
	struct rtp_s *rtp;
	raop_cmd_cb_t	cmd_cb;
	raop_data_cb_t	data_cb;
	/*
	struct {
		char				DACPid[32], id[32];
		struct in_addr		host;
		u16_t				port;
		struct mDNShandle_s *handle;
	} active_remote;
	*/
	void *owner;
} raop_ctx_t;

extern struct mdnsd* glmDNSServer;
extern log_level	raop_loglevel;
static log_level 	*loglevel = &raop_loglevel;

static void*	rtsp_thread(void *arg);
static bool 	handle_rtsp(raop_ctx_t *ctx, int sock);

static char*	rsa_apply(unsigned char *input, int inlen, int *outlen, int mode);
static int  	base64_pad(char *src, char **padded);
static int 		base64_encode(const void *data, int size, char **str);
static int 		base64_decode(const char *str, void *data);
static void* 	search_remote(void *args);

extern char private_key[];
enum { RSA_MODE_KEY, RSA_MODE_AUTH };

static void on_dmap_string(void *ctx, const char *code, const char *name, const char *buf, size_t len);

/*----------------------------------------------------------------------------*/
struct raop_ctx_s *raop_create(struct in_addr host, char *name,
						unsigned char mac[6], int latency,
						raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb) {
	struct raop_ctx_s *ctx = malloc(sizeof(struct raop_ctx_s));
	struct sockaddr_in addr;
	char id[64];
#ifdef WIN32
	socklen_t nlen = sizeof(struct sockaddr);
	char *txt[] = { "am=esp32", "tp=UDP", "sm=false", "sv=false", "ek=1",
					"et=0,1", "md=0,1,2", "cn=0,1", "ch=2",
					"ss=16", "sr=44100", "vn=3", "txtvers=1",
					NULL };
#else
	mdns_txt_item_t txt[] = {
		{"am", "esp32"},
		{"tp", "UDP"},
		{"sm","false"},
		{"sv","false"},
		{"ek","1"},
		{"et","0,1"},
		{"md","0,1,2"},
		{"cn","0,1"},
		{"ch","2"},
		{"ss","16"},
		{"sr","44100"},
		{"vn","3"},
		{"txtvers","1"},
	};

#endif

	if (!ctx) return NULL;

	// make sure we have a clean context
	memset(ctx, 0, sizeof(raop_ctx_t));

#ifdef WIN32
	ctx->svr = glmDNSServer;
#endif
	ctx->host = host;
	ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
	ctx->cmd_cb = cmd_cb;
	ctx->data_cb = data_cb;
	ctx->latency = min(latency, 88200);
	if (ctx->sock == -1) {
		LOG_ERROR("Cannot create listening socket", NULL);
		free(ctx);
		return NULL;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_addr.s_addr = host.s_addr;
	addr.sin_family = AF_INET;
#ifdef WIN32
	addr.sin_port = 0;
#else
	ctx->port = 5000;
	addr.sin_port = htons(ctx->port);
#endif

	if (bind(ctx->sock, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(ctx->sock, 1)) {
		LOG_ERROR("Cannot bind or listen RTSP listener: %s", strerror(errno));
		free(ctx);
		closesocket(ctx->sock);
		return NULL;
	}

#ifdef WIN32
	getsockname(ctx->sock, (struct sockaddr *) &addr, &nlen);
	ctx->port = ntohs(addr.sin_port);
#endif
	ctx->running = true;
	memcpy(ctx->mac, mac, 6);
	snprintf(id, 64, "%02X%02X%02X%02X%02X%02X@%s",  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);
#ifdef WIN32
	// seems that Windows snprintf does not add NULL char if actual size > max
	id[63] = '\0';
	ctx->svc = mdnsd_register_svc(ctx->svr, id, "_raop._tcp.local", ctx->port, NULL, (const char**) txt);
	pthread_create(&ctx->thread, NULL, &rtsp_thread, ctx);
#else
	LOG_INFO("starting mDNS with %s", id);
	ESP_ERROR_CHECK( mdns_service_add(id, "_raop", "_tcp", ctx->port, txt, sizeof(txt) / sizeof(mdns_txt_item_t)) );
	xTaskCreate((TaskFunction_t) rtsp_thread, "RTSP_thread", 8*1024, ctx, ESP_TASK_PRIO_MIN + 1, &ctx->thread);
#endif

	return ctx;
}


/*----------------------------------------------------------------------------*/
void raop_delete(struct raop_ctx_s *ctx) {
	int sock;
	struct sockaddr addr;
	socklen_t nlen = sizeof(struct sockaddr);

	if (!ctx) return;

	ctx->running = false;

	// wake-up thread by connecting socket, needed for freeBSD
	sock = socket(AF_INET, SOCK_STREAM, 0);
	getsockname(ctx->sock, (struct sockaddr *) &addr, &nlen);
	connect(sock, (struct sockaddr*) &addr, sizeof(addr));
	closesocket(sock);

#ifdef WIN32
	pthread_join(ctx->thread, NULL);
#else
	ctx->joiner = xTaskGetCurrentTaskHandle();
	xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
#endif

	rtp_end(ctx->rtp);

#ifdef WIN32
	shutdown(ctx->sock, SD_BOTH);
#else
	shutdown(ctx->sock, SHUT_RDWR);
#endif
	closesocket(ctx->sock);

	/*
	// terminate search, but do not reclaim memory of pthread if never launched
	if (ctx->active_remote.handle) {
		close_mDNS(ctx->active_remote.handle);
		pthread_join(ctx->search_thread, NULL);
	}
	*/

	NFREE(ctx->rtsp.aeskey);
	NFREE(ctx->rtsp.aesiv);
	NFREE(ctx->rtsp.fmtp);

	// stop broadcasting devices
#ifdef WIN32
	mdns_service_remove(ctx->svr, ctx->svc);
	mdnsd_stop(ctx->svr);
#endif

	free(ctx);
}


/*----------------------------------------------------------------------------*/
void  raop_cmd(struct raop_ctx_s *ctx, raop_event_t event, void *param) {
/*
	struct sockaddr_in addr;
	int sock;
	char *command = NULL;

	// first notify the remote controller (if any)
	switch(event) {
		case RAOP_PAUSE:
			command = strdup("pause");
			break;
		case RAOP_PLAY:
			command = strdup("play");
			break;
		case RAOP_STOP:
			command = strdup("stop");
			break;
		case RAOP_VOLUME: {
			float Volume = *((float*) param);
			Volume = Volume ? (Volume - 1) * 30 : -144;
			asprintf(&command,"setproperty?dmcp.device-volume=%0.4lf", Volume);
			break;
		}
		default:
			break;
	}

	// no command to send to remote or no remote found yet
	if (!command || !ctx->active_remote.port) {
		NFREE(command);
		return;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = S_ADDR(ctx->active_remote.host);
	addr.sin_port = htons(ctx->active_remote.port);

	if (!connect(sock, (struct sockaddr*) &addr, sizeof(addr))) {
		char *method, *buf, resp[512] = "";
		int len;
		key_data_t headers[4] = { {NULL, NULL} };

		asprintf(&method, "GET /ctrl-int/1/%s HTTP/1.0", command);
		kd_add(headers, "Active-Remote", ctx->active_remote.id);
		kd_add(headers, "Connection", "close");

		buf = http_send(sock, method, headers);
		len = recv(sock, resp, 512, 0);
		if (len > 0) resp[len-1] = '\0';
		LOG_INFO("[%p]: sending airplay remote\n%s<== received ==>\n%s", ctx, buf, resp);

		NFREE(method);
		NFREE(buf);
		kd_free(headers);
	}

	free(command);

	closesocket(sock);
*/
	// then notify local system
	ctx->cmd_cb(event, param);
}

/*----------------------------------------------------------------------------*/
static void *rtsp_thread(void *arg) {
	raop_ctx_t *ctx = (raop_ctx_t*) arg;
	int  sock = -1;

	while (ctx->running) {
		fd_set rfds;
		struct timeval timeout = {0, 100*1000};
		int n;
		bool res = false;

		if (sock == -1) {
			struct sockaddr_in peer;
			socklen_t addrlen = sizeof(struct sockaddr_in);

			sock = accept(ctx->sock, (struct sockaddr*) &peer, &addrlen);
			ctx->peer.s_addr = peer.sin_addr.s_addr;

			if (sock != -1 && ctx->running) {
				LOG_INFO("got RTSP connection %u", sock);
			} else continue;
		}

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		n = select(sock + 1, &rfds, NULL, NULL, &timeout);

		if (!n) continue;

		if (n > 0) res = handle_rtsp(ctx, sock);

		if (n < 0 || !res) {
			closesocket(sock);
			LOG_INFO("RTSP close %u", sock);
			sock = -1;
		}
	}

	if (sock != -1) closesocket(sock);

#ifndef WIN32
	xTaskNotify(ctx->joiner, 0, eNoAction);
	vTaskDelete(NULL);
#endif

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool handle_rtsp(raop_ctx_t *ctx, int sock)
{
	char *buf = NULL, *body = NULL, method[16] = "";
	key_data_t headers[16], resp[8] = { {NULL, NULL} };
	int len;
	bool success = true;
	
	if (!http_parse(sock, method, headers, &body, &len)) {
		NFREE(body);
		kd_free(headers);
		return false;
	}
	
	if (strcmp(method, "OPTIONS")) {
		LOG_INFO("[%p]: received %s", ctx, method);
	}

	if ((buf = kd_lookup(headers, "Apple-Challenge")) != NULL) {
		int n;
		char *buf_pad, *p, *data_b64 = NULL, data[32];

		LOG_INFO("[%p]: challenge %s", ctx, buf);

		// need to pad the base64 string as apple device don't
		base64_pad(buf, &buf_pad);

		p = data + min(base64_decode(buf_pad, data), 32-10);
		p = (char*) memcpy(p, &S_ADDR(ctx->host), 4) + 4;
		p = (char*) memcpy(p, ctx->mac, 6) + 6;
		memset(p, 0, 32 - (p - data));
		p = rsa_apply((unsigned char*) data, 32, &n, RSA_MODE_AUTH);
		n = base64_encode(p, n, &data_b64);

		// remove padding as well (seems to be optional now)
		for (n = strlen(data_b64) - 1; n > 0 && data_b64[n] == '='; data_b64[n--] = '\0');

		kd_add(resp, "Apple-Response", data_b64);

		NFREE(p);
		NFREE(buf_pad);
		NFREE(data_b64);
	}
	
	if (!strcmp(method, "OPTIONS")) {

		kd_add(resp, "Public", "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");

	} else if (!strcmp(method, "ANNOUNCE")) {
		char *padded, *p;

		NFREE(ctx->rtsp.aeskey);
		NFREE(ctx->rtsp.aesiv);
		NFREE(ctx->rtsp.fmtp);

		if ((p = strcasestr(body, "rsaaeskey")) != NULL) {
			unsigned char *aeskey;
			int len, outlen;

			p = strextract(p, ":", "\r\n");
			base64_pad(p, &padded);
			aeskey = malloc(strlen(padded));
			len = base64_decode(padded, aeskey);
			ctx->rtsp.aeskey = rsa_apply(aeskey, len, &outlen, RSA_MODE_KEY);

			NFREE(p);
			NFREE(aeskey);
			NFREE(padded);
		}

		if ((p = strcasestr(body, "aesiv")) != NULL) {
			p = strextract(p, ":", "\r\n");
			base64_pad(p, &padded);
			ctx->rtsp.aesiv = malloc(strlen(padded));
			base64_decode(padded, ctx->rtsp.aesiv);

			NFREE(p);
			NFREE(padded);
		}

		if ((p = strcasestr(body, "fmtp")) != NULL) {
			p = strextract(p, ":", "\r\n");
			ctx->rtsp.fmtp = strdup(p);
			NFREE(p);
		}

		// on announce, search remote
		/*
		if ((buf = kd_lookup(headers, "DACP-ID")) != NULL) strcpy(ctx->active_remote.DACPid, buf);
		if ((buf = kd_lookup(headers, "Active-Remote")) != NULL) strcpy(ctx->active_remote.id, buf);

		ctx->active_remote.handle = init_mDNS(false, ctx->host);
		pthread_create(&ctx->search_thread, NULL, &search_remote, ctx);
		*/

	} else if (!strcmp(method, "SETUP") && ((buf = kd_lookup(headers, "Transport")) != NULL)) {
		char *p;
		rtp_resp_t rtp = { 0 };
		short unsigned tport = 0, cport = 0;

		// we are about to stream, do something if needed
		ctx->cmd_cb(RAOP_SETUP, NULL);

		if ((p = strcasestr(buf, "timing_port")) != NULL) sscanf(p, "%*[^=]=%hu", &tport);
		if ((p = strcasestr(buf, "control_port")) != NULL) sscanf(p, "%*[^=]=%hu", &cport);

		rtp = rtp_init(ctx->peer, ctx->latency,	ctx->rtsp.aeskey, ctx->rtsp.aesiv,
					   ctx->rtsp.fmtp, cport, tport, ctx->cmd_cb, ctx->data_cb);
						
		ctx->rtp = rtp.ctx;
		
		if (cport * tport * rtp.cport * rtp.tport * rtp.aport && rtp.ctx) {
			char *transport;
			asprintf(&transport, "RTP/AVP/UDP;unicast;mode=record;control_port=%u;timing_port=%u;server_port=%u", rtp.cport, rtp.tport, rtp.aport);
			LOG_DEBUG("[%p]: audio=(%hu:%hu), timing=(%hu:%hu), control=(%hu:%hu)", ctx, 0, rtp.aport, tport, rtp.tport, cport, rtp.cport);
			kd_add(resp, "Transport", transport);
			kd_add(resp, "Session", "DEADBEEF");
			free(transport);
		} else {
			success = false;
			LOG_INFO("[%p]: cannot start session, missing ports", ctx);
		}

	} else if (!strcmp(method, "RECORD")) {
		unsigned short seqno = 0;
		unsigned rtptime = 0;
		char *p;

		if (ctx->latency) {
			char latency[6];
			snprintf(latency, 6, "%u", ctx->latency);
			kd_add(resp, "Audio-Latency", latency);
		}

		buf = kd_lookup(headers, "RTP-Info");
		if (buf && (p = strcasestr(buf, "seq")) != NULL) sscanf(p, "%*[^=]=%hu", &seqno);
		if (buf && (p = strcasestr(buf, "rtptime")) != NULL) sscanf(p, "%*[^=]=%u", &rtptime);

		if (ctx->rtp) rtp_record(ctx->rtp, seqno, rtptime);

		ctx->cmd_cb(RAOP_STREAM, NULL);

	}  else if (!strcmp(method, "FLUSH")) {
		unsigned short seqno = 0;
		unsigned rtptime = 0;
		char *p;

		buf = kd_lookup(headers, "RTP-Info");
		if ((p = strcasestr(buf, "seq")) != NULL) sscanf(p, "%*[^=]=%hu", &seqno);
		if ((p = strcasestr(buf, "rtptime")) != NULL) sscanf(p, "%*[^=]=%u", &rtptime);

		// only send FLUSH if useful (discards frames above buffer head and top)
		if (ctx->rtp && rtp_flush(ctx->rtp, seqno, rtptime))
			ctx->cmd_cb(RAOP_FLUSH, NULL);

	}  else if (!strcmp(method, "TEARDOWN")) {

		rtp_end(ctx->rtp);

		ctx->rtp = NULL;

		/*
		// need to make sure no search is on-going and reclaim pthread memory
		if (ctx->active_remote.handle) close_mDNS(ctx->active_remote.handle);
		pthread_join(ctx->search_thread, NULL);
		memset(&ctx->active_remote, 0, sizeof(ctx->active_remote));
		*/

		NFREE(ctx->rtsp.aeskey);
		NFREE(ctx->rtsp.aesiv);
		NFREE(ctx->rtsp.fmtp);

		ctx->cmd_cb(RAOP_STOP, NULL);

	} if (!strcmp(method, "SET_PARAMETER")) {
		char *p;

		if (body && (p = strcasestr(body, "volume")) != NULL) {
			float volume;

			sscanf(p, "%*[^:]:%f", &volume);
			LOG_INFO("[%p]: SET PARAMETER volume %f", ctx, volume);
			volume = (volume == -144.0) ? 0 : (1 + volume / 30);
			ctx->cmd_cb(RAOP_VOLUME, &volume);
		}
/*
		if (body && ((p = kd_lookup(headers, "Content-Type")) != NULL) && !strcasecmp(p, "application/x-dmap-tagged")) {
			struct metadata_s metadata;
			dmap_settings settings = {
				NULL, NULL, NULL, NULL,	NULL, NULL,	NULL, on_dmap_string, NULL,
				NULL
			};

			settings.ctx = &metadata;
			memset(&metadata, 0, sizeof(struct metadata_s));
			if (!dmap_parse(&settings, body, len)) {
				LOG_INFO("[%p]: received metadata\n\tartist: %s\n\talbum:  %s\n\ttitle:  %s",
						 ctx, metadata.artist, metadata.album, metadata.title);
				free_metadata(&metadata);
			}
		}
*/
	}

	// don't need to free "buf" because kd_lookup return a pointer, not a strdup

	kd_add(resp, "Audio-Jack-Status", "connected; type=analog");
	kd_add(resp, "CSeq", kd_lookup(headers, "CSeq"));

	if (success) buf = http_send(sock, "RTSP/1.0 200 OK", resp);
	else buf = http_send(sock, "RTSP/1.0 500 ERROR", NULL);

	if (strcmp(method, "OPTIONS")) {
		LOG_INFO("[%p]: responding:\n%s", ctx, buf ? buf : "<void>");
	}

	NFREE(body);
	NFREE(buf);
	kd_free(resp);
	kd_free(headers);

	return true;
}

/*----------------------------------------------------------------------------*/
/*
bool search_remote_cb(mDNSservice_t *slist, void *cookie, bool *stop) {
	mDNSservice_t *s;
	raop_ctx_t *ctx = (raop_ctx_t*) cookie;

	// see if we have found an active remote for our ID
	for (s = slist; s; s = s->next) {
		if (strcasestr(s->name, ctx->active_remote.DACPid)) {
			ctx->active_remote.host = s->addr;
			ctx->active_remote.port = s->port;
			LOG_INFO("[%p]: found ActiveRemote for %s at %s:%u", ctx, ctx->active_remote.DACPid,
								inet_ntoa(ctx->active_remote.host), ctx->active_remote.port);
			*stop = true;
			break;
		}
	}

	// let caller clear list
	return false;
}
*/


/*----------------------------------------------------------------------------*/
/*
static void* search_remote(void *args) {
	raop_ctx_t *ctx = (raop_ctx_t*) args;

	query_mDNS(ctx->active_remote.handle, "_dacp._tcp.local", 0, 0, &search_remote_cb, (void*) ctx);

	return NULL;
}
*/

/*----------------------------------------------------------------------------*/
static char *rsa_apply(unsigned char *input, int inlen, int *outlen, int mode)
{
	static char super_secret_key[] =
	"-----BEGIN RSA PRIVATE KEY-----\n"
	"MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
	"wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
	"wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
	"/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
	"UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
	"BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
	"LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
	"NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
	"lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
	"aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
	"a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
	"oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
	"oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
	"k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
	"AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
	"cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
	"54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
	"17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
	"1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
	"LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
	"2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
	"-----END RSA PRIVATE KEY-----";
#ifdef WIN32
	unsigned char *out;
	RSA *rsa;

	BIO *bmem = BIO_new_mem_buf(super_secret_key, -1);
	rsa = PEM_read_bio_RSAPrivateKey(bmem, NULL, NULL, NULL);
	BIO_free(bmem);

	out = malloc(RSA_size(rsa));
	switch (mode) {
		case RSA_MODE_AUTH:
			*outlen = RSA_private_encrypt(inlen, input, out, rsa,
										  RSA_PKCS1_PADDING);
			break;
		case RSA_MODE_KEY:
			*outlen = RSA_private_decrypt(inlen, input, out, rsa,
										  RSA_PKCS1_OAEP_PADDING);
			break;
	}

	RSA_free(rsa);

	return (char*) out;
#else
	mbedtls_pk_context pkctx;
	mbedtls_rsa_context *trsa;
	size_t olen;
	
	/*
	we should do entropy initialization & pass a rng function but this
	consumes a ton of stack and there is no security concern here. Anyway,
	mbedtls takes a lot of stack, unfortunately ...
	*/

	mbedtls_pk_init(&pkctx);
	mbedtls_pk_parse_key(&pkctx, (unsigned char *)super_secret_key,
		sizeof(super_secret_key), NULL, 0);

	uint8_t *outbuf = NULL;
	trsa = mbedtls_pk_rsa(pkctx);

	switch (mode) {
	case RSA_MODE_AUTH:
		mbedtls_rsa_set_padding(trsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
		outbuf = malloc(trsa->len);
		mbedtls_rsa_pkcs1_encrypt(trsa, NULL, NULL, MBEDTLS_RSA_PRIVATE, inlen, input, outbuf);
		*outlen = trsa->len;
		break;
	case RSA_MODE_KEY:
		mbedtls_rsa_set_padding(trsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
		outbuf = malloc(trsa->len);
		mbedtls_rsa_pkcs1_decrypt(trsa, NULL, NULL, MBEDTLS_RSA_PRIVATE, &olen, input, outbuf, trsa->len);
		*outlen = olen;
		break;
	}

	mbedtls_pk_free(&pkctx);

	return (char*) outbuf;
#endif
}

#define DECODE_ERROR 0xffffffff

static char base64_chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*----------------------------------------------------------------------------*/
static int  base64_pad(char *src, char **padded)
{
	int n;

	n = strlen(src) + strlen(src) % 4;
	*padded = malloc(n + 1);
	memset(*padded, '=', n);
	memcpy(*padded, src, strlen(src));
	(*padded)[n] = '\0';

	return strlen(*padded);
}

/*----------------------------------------------------------------------------*/
static int pos(char c)
{
	char *p;
	for (p = base64_chars; *p; p++)
	if (*p == c)
		return p - base64_chars;
	return -1;
}

/*----------------------------------------------------------------------------*/
static int base64_encode(const void *data, int size, char **str)
{
	char *s, *p;
	int i;
	int c;
	const unsigned char *q;

	p = s = (char *) malloc(size * 4 / 3 + 4);
	if (p == NULL) return -1;
	q = (const unsigned char *) data;
	i = 0;
	for (i = 0; i < size;) {
		c = q[i++];
		c *= 256;
		if (i < size) c += q[i];
		i++;
		c *= 256;
		if (i < size) c += q[i];
		i++;
		p[0] = base64_chars[(c & 0x00fc0000) >> 18];
		p[1] = base64_chars[(c & 0x0003f000) >> 12];
		p[2] = base64_chars[(c & 0x00000fc0) >> 6];
		p[3] = base64_chars[(c & 0x0000003f) >> 0];
		if (i > size) p[3] = '=';
		if (i > size + 1) p[2] = '=';
		p += 4;
	}
	*p = 0;
	*str = s;
	return strlen(s);
}

/*----------------------------------------------------------------------------*/
static unsigned int token_decode(const char *token)
{
	int i;
	unsigned int val = 0;
	int marker = 0;
	if (strlen(token) < 4)
	return DECODE_ERROR;
	for (i = 0; i < 4; i++) {
	val *= 64;
	if (token[i] == '=')
		marker++;
	else if (marker > 0)
		return DECODE_ERROR;
	else
		val += pos(token[i]);
	}
	if (marker > 2)
	return DECODE_ERROR;
	return (marker << 24) | val;
}

/*----------------------------------------------------------------------------*/
static int base64_decode(const char *str, void *data)
{
	const char *p;
	unsigned char *q;

	q = data;
	for (p = str; *p && (*p == '=' || strchr(base64_chars, *p)); p += 4) {
	unsigned int val = token_decode(p);
	unsigned int marker = (val >> 24) & 0xff;
	if (val == DECODE_ERROR)
		return -1;
	*q++ = (val >> 16) & 0xff;
	if (marker < 2)
		*q++ = (val >> 8) & 0xff;
	if (marker < 1)
		*q++ = val & 0xff;
	}
	return q - (unsigned char *) data;
}

/*----------------------------------------------------------------------------*/
static void on_dmap_string(void *ctx, const char *code, const char *name, const char *buf, size_t len) {
	struct metadata_s *metadata = (struct metadata_s *) ctx;

	if (!strcasecmp(code, "asar")) metadata->artist = strndup(buf, len);
	else if (!strcasecmp(code, "asal")) metadata->album = strndup(buf, len);
	else if (!strcasecmp(code, "minm")) metadata->title = strndup(buf, len);
}

