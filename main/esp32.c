#include <signal.h>

#include "sdkconfig.h"
#include "esp_system.h" 
#include "squeezelite.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"


#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#define BT_AV_TAG               "BT_AV"
u8_t *bt_optr;
extern log_level loglevel;
extern struct outputstate output;
extern struct buffer *outputbuf;
extern struct buffer *streambuf;
#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)
int64_t connecting_timeout = 0;
#ifndef CONFIG_A2DP_SINK_NAME
#define CONFIG_A2DP_SINK_NAME "btspeaker" // fix some compile errors when BT is not chosen
#endif
#ifndef CONFIG_A2DP_CONNECT_TIMEOUT_MS
#define CONFIG_A2DP_CONNECT_TIMEOUT_MS 2000
#endif
#ifndef CONFIG_A2DP_DEV_NAME
#define CONFIG_A2DP_DEV_NAME "espsqueezelite"
#endif
#ifndef CONFIG_A2DP_CONTROL_DELAY_MS
#define CONFIG_A2DP_CONTROL_DELAY_MS 1000
#endif
#define A2DP_TIMER_INIT connecting_timeout = esp_timer_get_time() +(CONFIG_A2DP_CONNECT_TIMEOUT_MS * 1000)
#define IS_A2DP_TIMER_OVER esp_timer_get_time() >= connecting_timeout


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

#define LOG_DEBUG_EVENT(e) LOG_DEBUG("evt: " STR(e))
#define LOG_SDEBUG_EVENT(e) LOG_SDEBUG("evt: " STR(e))

/* event for handler "bt_av_hdl_stack_up */
enum {
    BT_APP_EVT_STACK_UP = 0,
};

/* A2DP global state */
enum {
    APP_AV_STATE_IDLE,
    APP_AV_STATE_DISCOVERING,
    APP_AV_STATE_DISCOVERED,
    APP_AV_STATE_UNCONNECTED,
    APP_AV_STATE_CONNECTING,
    APP_AV_STATE_CONNECTED,
    APP_AV_STATE_DISCONNECTING,
};

char * APP_AV_STATE_DESC[] = {
	    "APP_AV_STATE_IDLE",
	    "APP_AV_STATE_DISCOVERING",
	    "APP_AV_STATE_DISCOVERED",
	    "APP_AV_STATE_UNCONNECTED",
	    "APP_AV_STATE_CONNECTING",
	    "APP_AV_STATE_CONNECTED",
	    "APP_AV_STATE_DISCONNECTING"
};



/* sub states of APP_AV_STATE_CONNECTED */

enum {
    APP_AV_MEDIA_STATE_IDLE,
    APP_AV_MEDIA_STATE_STARTING,
    APP_AV_MEDIA_STATE_STARTED,
    APP_AV_MEDIA_STATE_STOPPING,
	APP_AV_MEDIA_STATE_WAIT_DISCONNECT
};

#define BT_APP_HEART_BEAT_EVT                (0xff00)

/// handler for bluetooth stack enabled events
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/// callback function for A2DP source
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/// callback function for A2DP source audio data stream
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);

static void a2d_app_heart_beat(void *arg);

/// A2DP application state machine
static void bt_app_av_sm_hdlr(uint16_t event, void *param);

/* A2DP application state machine handler for each state */
static void bt_app_av_state_unconnected(uint16_t event, void *param);
static void bt_app_av_state_connecting(uint16_t event, void *param);
static void bt_app_av_state_connected(uint16_t event, void *param);
static void bt_app_av_state_disconnecting(uint16_t event, void *param);

static esp_bd_addr_t s_peer_bda = {0};
static uint8_t s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static int s_a2d_state = APP_AV_STATE_IDLE;
static int s_media_state = APP_AV_MEDIA_STATE_IDLE;
static int s_intv_cnt = 0;
static uint32_t s_pkt_cnt = 0;

static TimerHandle_t s_tmr;

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}
void hal_bluetooth_init(log_level level)
{

	/*
	 * Bluetooth audio source init Start
	 */
	loglevel = level;
	//running_test = false;
	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

	if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
		LOG_ERROR("%s initialize controller failed\n", __func__);
		return;
	}

	if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
		LOG_ERROR("%s enable controller failed\n", __func__);
		return;
	}

	if (esp_bluedroid_init() != ESP_OK) {
		LOG_ERROR("%s initialize bluedroid failed\n", __func__);
		return;
	}

	if (esp_bluedroid_enable() != ESP_OK) {
		LOG_ERROR("%s enable bluedroid failed\n", __func__);
		return;
	}
   /* create application task */
	bt_app_task_start_up();

	/* Bluetooth device name, connection mode and profile set up */
	bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

	#if (CONFIG_BT_SSP_ENABLED == true)
	/* Set default parameters for Secure Simple Pairing */
	esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
	esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
	esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
	#endif

	/*
	 * Set default parameters for Legacy Pairing
	 * Use variable pin, input pin code when pairing
	 */
	esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
	esp_bt_pin_code_t pin_code;
	esp_bt_gap_set_pin(pin_type, 0, pin_code);

}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}
#define LOG_INFO_NO_LF(fmt, ...)   if (loglevel >= lINFO)  logprint(fmt, ##__VA_ARGS__)
static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint32_t cod = 0;
    int32_t rssi = -129; /* invalid value */
    uint8_t *eir = NULL;
    uint8_t nameLen = 0;
    esp_bt_gap_dev_prop_t *p;
    memset(s_peer_bdname, 0x00,sizeof(s_peer_bdname));

    LOG_INFO("\n=======================\nScanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(p->val);
            LOG_INFO_NO_LF("\n-- Class of Device: 0x%x", cod);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(p->val);
            LOG_INFO_NO_LF("\n-- RSSI: %d", rssi);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = (uint8_t *)(p->val);
            LOG_INFO_NO_LF("\n-- EIR: %d", eir);
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            nameLen = (p->len > ESP_BT_GAP_MAX_BDNAME_LEN) ? ESP_BT_GAP_MAX_BDNAME_LEN : (uint8_t)p->len;
            memcpy(s_peer_bdname, (uint8_t *)(p->val), nameLen);
            s_peer_bdname[nameLen] = '\0';
            LOG_INFO_NO_LF("\n-- Name: %s", s_peer_bdname);
            break;
        default:
            break;
        }
    }
    if (!esp_bt_gap_is_valid_cod(cod)){
    /* search for device with MAJOR service class as "rendering" in COD */
    	LOG_INFO_NO_LF("\n--Invalid class of device. Skipping.\n");
    	return;
    }
    else if (!(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING))
    {
    	LOG_INFO_NO_LF("\n--Not a rendering device. Skipping.\n");
    	return;
    }


    /* search for device named "ESP_SPEAKER" in its extended inqury response */
    if (eir) {
    	LOG_INFO_NO_LF("\n--Getting details from eir.\n");
        get_name_from_eir(eir, s_peer_bdname, NULL);
        LOG_INFO("--\nDevice name is %s",s_peer_bdname);
    }

    if (strcmp((char *)s_peer_bdname, CONFIG_A2DP_SINK_NAME) == 0) {
    	LOG_INFO("Found a target device, address %s, name %s", bda_str, s_peer_bdname);

        if(esp_bt_gap_cancel_discovery()!=ESP_ERR_INVALID_STATE)
        {
        	LOG_INFO("Cancel device discovery ...");
			memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        	s_a2d_state = APP_AV_STATE_DISCOVERED;
        }
        else
        {
        	LOG_ERROR("Cancel device discovery failed...");
        }
    }
    else
    {
    	LOG_INFO("Not the device we are looking for. Continuing scan.");
    }
}


void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        filter_inquiry_scan_result(param);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
        {
            if (s_a2d_state == APP_AV_STATE_DISCOVERED)
            {
                if(esp_a2d_source_connect(s_peer_bda)!=ESP_ERR_INVALID_STATE)
                {
                	s_a2d_state = APP_AV_STATE_CONNECTING;
					LOG_INFO("Device discovery stopped. a2dp connecting to peer: %s", s_peer_bdname);
					A2DP_TIMER_INIT;
                }
                else
                {
                	// not discovered, continue to discover
					LOG_INFO("Attempt at connecting failed, resuming discover...");
					esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
                }
            }
            else
            {
                // not discovered, continue to discover
                LOG_INFO("Device discovery failed, continue to discover...");
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        }
        else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            LOG_INFO("Discovery started.");
        }
        else
        {
        	LOG_DEBUG("This shouldn't happen.  Discovery has only 2 states (for now).");
        }
        break;
    }
    case ESP_BT_GAP_RMT_SRVCS_EVT:
    	LOG_DEBUG_EVENT(ESP_BT_GAP_RMT_SRVCS_EVT);
    	break;
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:
    	LOG_DEBUG_EVENT(ESP_BT_GAP_RMT_SRVC_REC_EVT);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
    	if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            LOG_INFO("authentication success: %s", param->auth_cmpl.device_name);
            //esp_log_buffer_hex(param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            LOG_ERROR("authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
    	LOG_INFO("ESP_BT_GAP_PIN_REQ_EVT min_16_digit:%d", param->pin_req.min_16_digit);
        if (param->pin_req.min_16_digit) {
            LOG_INFO("Input pin code: 0000 0000 0000 0000");
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            LOG_INFO("Input pin code: 1234");
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        LOG_INFO("ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        LOG_INFO("ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
        break;
        LOG_INFO("ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif

    default: {
        LOG_INFO("event: %d", event);
        break;
    }
    }
    return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{

    switch (event) {
    case BT_APP_EVT_STACK_UP: {
    	LOG_INFO("BT Stack going up.");
        /* set up device name */
        char *dev_name = CONFIG_A2DP_DEV_NAME;
        esp_bt_dev_set_device_name(dev_name);
        LOG_INFO("Preparing to connect to device: %s",CONFIG_A2DP_SINK_NAME);

        /* register GAP callback function */
        esp_bt_gap_register_callback(bt_app_gap_cb);

        /* initialize A2DP source */
        esp_a2d_register_callback(&bt_app_a2d_cb);
        esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);
        esp_a2d_source_init();

        /* set discoverable and connectable mode */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

        /* start device discovery */
        LOG_INFO("Starting device discovery...");
        s_a2d_state = APP_AV_STATE_DISCOVERING;
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

        /* create and start heart beat timer */
        do {
            int tmr_id = 0;
            s_tmr = xTimerCreate("connTmr", (CONFIG_A2DP_CONTROL_DELAY_MS / portTICK_RATE_MS),
                               pdTRUE, (void *)tmr_id, a2d_app_heart_beat);
            xTimerStart(s_tmr, portMAX_DELAY);
        } while (0);
        break;
    }
    default:
        LOG_ERROR("%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL);
}

static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
	frames_t frames;
	static int count = 0;
	static unsigned min_o = -1, max_o = 0, min_s = -1, max_s = 0;
	unsigned o, s;


	if (len < 0 || data == NULL ) {
        return 0;
    }

	// bail out if A2DP isn't connected
	LOCK;
//	if(s_media_state != APP_AV_MEDIA_STATE_STARTED)
//    {
//		UNLOCK;
//    	return 0;
//    }


//
///*	Normally, we would want BT to not call us back unless we are not in BUFFERING state.
//	That requires BT to not start until we are > OUTPUT_BUFFER
//	// come back later, we are buffering (or stopped, need to handle that case ...) but we don't want silence */
//	if (output.state == OUTPUT_BUFFER) {
//		UNLOCK;
//		int32_t silence_bytes = (len >MAX_SILENCE_FRAMES * BYTES_PER_FRAME?MAX_SILENCE_FRAMES * BYTES_PER_FRAME:len;
//		memcpy(bt_optr, (u8_t *)silencebuf, silence_bytes);
//		return actual_len;
//	}
// This is how the BTC layer calculates the number of bytes to
// for us to send. (BTC_SBC_DEC_PCM_DATA_LEN * sizeof(OI_INT16) - availPcmBytes
   	frames = len / BYTES_PER_FRAME;
   	output.device_frames = 0;
   	output.updated = gettime_ms();
   	output.frames_played_dmp = output.frames_played;
	//if (output.threshold < 20) output.threshold = 20;
	int ret;

	frames_t wanted_frames=len/BYTES_PER_FRAME;
	bt_optr = data; // needed for the _write_frames callback
	do {
			frames = _output_frames(wanted_frames);
			wanted_frames -= frames;
		} while (wanted_frames > 0 && frames != 0);

		if (wanted_frames > 0) {
			LOG_DEBUG("need to pad with silence");
			memset(bt_optr, 0, wanted_frames * BYTES_PER_FRAME);
		}


	UNLOCK;

	o = _buf_used(outputbuf);
	if (o < min_o) min_o = o;
	if (o > max_o) max_o = o;

	s = _buf_used(streambuf);
	if (s < min_s) min_s = s;
	if (s > max_s) max_s = s;

	if (!(count++ & 0x1ff)) {
		LOG_INFO("frames %d (count:%d) (out:%d/%d/%d, stream:%d/%d/%d)", frames, count, max_o, min_o, o, max_s, min_s, s);
		min_o = min_s = -1;
		max_o = max_s = -0;
	}

	return frames * BYTES_PER_FRAME;
}
static bool running_test;
#ifdef BTAUDIO
bool test_open(const char *device, unsigned rates[], bool userdef_rates) {

//	running_test  = true;
//	while(running_test)
//	{
//		// wait until BT playback has started
//		// this will allow querying the sample rate
//		usleep(100000);
//	}

	memset(rates, 0, MAX_SUPPORTED_SAMPLERATES * sizeof(unsigned));
	if (!strcmp(device, "BT")) {
		rates[0] = 44100;
	} else {
		unsigned _rates[] = { 96000, 88200, 48000, 44100, 32000, 0 };
		memcpy(rates, _rates, sizeof(_rates));
	}
	return true;
}
#endif
static void a2d_app_heart_beat(void *arg)
{
    bt_app_work_dispatch(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT, NULL, 0, NULL);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
    //LOG_DEBUG("%s state %s, evt 0x%x, output state: %d", __func__, APP_AV_STATE_DESC[s_a2d_state], event, output.state);
    switch (s_a2d_state) {
    case APP_AV_STATE_DISCOVERING:
    	LOG_DEBUG("state %s, evt 0x%x, output state: %d", APP_AV_STATE_DESC[s_a2d_state], event, output.state);
    	break;
    case APP_AV_STATE_DISCOVERED:
    	LOG_DEBUG("state %s, evt 0x%x, output state: %d", APP_AV_STATE_DESC[s_a2d_state], event, output.state);
        break;
    case APP_AV_STATE_UNCONNECTED:
        bt_app_av_state_unconnected(event, param);
        break;
    case APP_AV_STATE_CONNECTING:
        bt_app_av_state_connecting(event, param);
        break;
    case APP_AV_STATE_CONNECTED:
        bt_app_av_state_connected(event, param);
        break;
    case APP_AV_STATE_DISCONNECTING:
        bt_app_av_state_disconnecting(event, param);
        break;
    default:
        LOG_ERROR("%s invalid state %d", __func__, s_a2d_state);
        break;
    }
}

static void bt_app_av_state_unconnected(uint16_t event, void *param)
{
	switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_CONNECTION_STATE_EVT);
    	break;
    case ESP_A2D_AUDIO_STATE_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_STATE_EVT);
    	break;
    case ESP_A2D_AUDIO_CFG_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_CFG_EVT);
    	break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_MEDIA_CTRL_ACK_EVT);
    	break;
    case BT_APP_HEART_BEAT_EVT: {
        uint8_t *p = s_peer_bda;
        LOG_INFO("BT_APP_HEART_BEAT_EVT a2dp connecting to peer: %02x:%02x:%02x:%02x:%02x:%02x",p[0], p[1], p[2], p[3], p[4], p[5]);
        switch (esp_bluedroid_get_status()) {
		case ESP_BLUEDROID_STATUS_UNINITIALIZED:
			LOG_INFO("BlueDroid Status is ESP_BLUEDROID_STATUS_UNINITIALIZED.");
			break;
		case ESP_BLUEDROID_STATUS_INITIALIZED:
			LOG_INFO("BlueDroid Status is ESP_BLUEDROID_STATUS_INITIALIZED.");
			break;
		case ESP_BLUEDROID_STATUS_ENABLED:
			LOG_INFO("BlueDroid Status is ESP_BLUEDROID_STATUS_ENABLED.");
			break;
			default:
				break;
		}
        if(esp_a2d_source_connect(s_peer_bda)!=ESP_ERR_INVALID_STATE)
		{
			s_a2d_state = APP_AV_STATE_CONNECTING;
			LOG_INFO("a2dp connecting to peer: %s", s_peer_bdname);
			A2DP_TIMER_INIT;
		}
		else
		{
			// not discovered, continue to discover
			LOG_INFO("Attempt at connecting failed, resuming discover...");
			esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
		}
        break;
    }
    default:
        LOG_ERROR("%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_state_connecting(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            LOG_INFO("a2dp connected! Stopping scan. ");
            s_a2d_state =  APP_AV_STATE_CONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_STATE_EVT);
    	break;
    case ESP_A2D_AUDIO_CFG_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_CFG_EVT);
    	break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_MEDIA_CTRL_ACK_EVT);
    	break;
    case BT_APP_HEART_BEAT_EVT:
    	if (IS_A2DP_TIMER_OVER)
    	{
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            LOG_DEBUG("Connect timed out.  Setting state to Unconnected. ");
        }
    	LOG_SDEBUG("BT_APP_HEART_BEAT_EVT");
        break;
    default:
        LOG_ERROR("%s unhandled evt %d", __func__, event);
        break;
    }
}


static void bt_app_av_media_proc(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (s_media_state) {
    case APP_AV_MEDIA_STATE_IDLE: {
    	if (event == BT_APP_HEART_BEAT_EVT) {
            if(output.state < OUTPUT_STOPPED )
        	{
        		// TODO: anything to do while we are waiting? Should we check if we're still connected?
        	}
            else if(output.state >= OUTPUT_BUFFER )
            {
            	LOG_INFO("buffering output, a2dp media ready and connected. Starting checking if ready...");
            	esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            }
//            else if(running_test)
//			{
//            	LOG_INFO("buffering output, a2dp media ready and connected. Starting checking if ready...");
//
//            	esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
//			}


        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
        	a2d = (esp_a2d_cb_param_t *)(param);
			if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
					a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS
					) {
				LOG_INFO("a2dp media ready, starting media playback ...");
				s_media_state = APP_AV_MEDIA_STATE_STARTING;
				esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);

			}
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTING: {
    	if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                LOG_INFO("a2dp media started successfully.");
                s_intv_cnt = 0;
                s_media_state = APP_AV_MEDIA_STATE_STARTED;
            } else {
                // not started succesfully, transfer to idle state
                LOG_INFO("a2dp media start failed.");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTED: {
        if (event == BT_APP_HEART_BEAT_EVT) {
        	if(output.state <= OUTPUT_STOPPED) {
                LOG_INFO("Output state is stopped. Stopping a2dp media ...");
                s_media_state = APP_AV_MEDIA_STATE_STOPPING;
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);

                s_intv_cnt = 0;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STOPPING: {
    	LOG_DEBUG_EVENT(APP_AV_MEDIA_STATE_STOPPING);
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_STOP &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                LOG_INFO("a2dp media stopped successfully...");
                //s_media_state = APP_AV_MEDIA_STATE_WAIT_DISCONNECT;

                s_media_state = APP_AV_MEDIA_STATE_IDLE;
                // todo:  should we disconnect?
//                esp_a2d_source_disconnect(s_peer_bda);
//                s_a2d_state = APP_AV_STATE_DISCONNECTING;
            } else {
                LOG_INFO("a2dp media stopping...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
            }
        }
        break;
    }
    }
}

static void bt_app_av_state_connected(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
    	a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            LOG_INFO("a2dp disconnected");
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_STATE_EVT);
        a2d = (esp_a2d_cb_param_t *)(param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            s_pkt_cnt = 0;
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        // not suppposed to occur for A2DP source
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_CFG_EVT);
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:{
        	LOG_DEBUG_EVENT(ESP_A2D_MEDIA_CTRL_ACK_EVT);
            bt_app_av_media_proc(event, param);
            break;
        }
    case BT_APP_HEART_BEAT_EVT: {
    	LOG_SDEBUG_EVENT(BT_APP_HEART_BEAT_EVT);
        bt_app_av_media_proc(event, param);
        break;
    }
    default:
        LOG_ERROR("%s unhandled evt %d", __func__, event);
        break;
    }
}

static void bt_app_av_state_disconnecting(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
    	LOG_DEBUG_EVENT(ESP_A2D_CONNECTION_STATE_EVT);
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            LOG_INFO("a2dp disconnected");
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_STATE_EVT);
    	break;
    case ESP_A2D_AUDIO_CFG_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_AUDIO_CFG_EVT);
    	break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    	LOG_DEBUG_EVENT(ESP_A2D_MEDIA_CTRL_ACK_EVT);
    	break;
    case BT_APP_HEART_BEAT_EVT:
    	LOG_DEBUG_EVENT(BT_APP_HEART_BEAT_EVT);
    	break;
    default:
        LOG_ERROR("%s unhandled evt %d", __func__, event);
        break;
    }
}
