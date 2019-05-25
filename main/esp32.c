#include <signal.h>

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

#ifndef CONFIG_AUDIO_FAAD
struct codec *register_faad(void) {
	LOG_INFO("aac unavailable");
	return NULL;
}
#endif

#ifndef CONFIG_AUDIO_MAD
struct codec *register_mad(void) {
	LOG_INFO("mad unavailable");
	return NULL;
}
#endif

#ifndef CONFIG_AUDIO_FLAC
struct codec *register_flac(void) {
	LOG_INFO("flac unavailable");
	return NULL;
}
#endif

