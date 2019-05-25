#include <signal.h>

#include "sdkconfig.h"
#include "esp_system.h" 
#include "squeezelite.h"

extern log_level loglevel;

void get_mac(u8_t mac[]) {
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

_sig_func_ptr signal(int sig, _sig_func_ptr func) {
	return NULL;
}

void *audio_calloc(size_t nmemb, size_t size) {
		return calloc(nmemb, size);
}

struct codec *register_mpg(void) {
	LOG_INFO("mpg unavailable");
	return NULL;
}

#if !CONFIG_INCLUDE_FAAD
struct codec *register_faad(void) {
	LOG_INFO("aac unavailable");
	return NULL;
}
#endif

#if !CONFIG_INCLUDE_MAD
struct codec *register_mad(void) {
	LOG_INFO("mad unavailable");
	return NULL;
}
#endif

#if !CONFIG_INCLUDE_FLAC
struct codec *register_flac(void) {
	LOG_INFO("flac unavailable");
	return NULL;
}
#endif

#if !CONFIG_INCLUDE_VORBIS
struct codec *register_vorbis(void) {
	LOG_INFO("vorbis unavailable");
	return NULL;
}
#endif

#if !CONFIG_INCLUDE_ALAC
struct codec *register_alac(void) {
	LOG_INFO("alac unavailable");
	return NULL;
}
#endif

