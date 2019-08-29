//size_t esp_console_split_argv(char *line, char **argv, size_t argv_size);
#include "cmd_squeezelite.h"

#include <stdio.h>
#include <string.h>
#include "cmd_decl.h"

#include "esp_log.h"
#include "esp_console.h"
#include "esp_pthread.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "pthread.h"
#include "platform_esp32.h"
#include "nvs.h"
#include "nvs_flash.h"
//extern char current_namespace[];
static const char * TAG = "squeezelite_cmd";
#define SQUEEZELITE_THREAD_STACK_SIZE 8192
extern int main(int argc, char **argv);
static int launchsqueezelite(int argc, char **argv);
pthread_t thread_squeezelite;
pthread_t thread_squeezelite_runner;
/** Arguments used by 'squeezelite' function */
static struct {
    struct arg_str *parameters;
    struct arg_end *end;
} squeezelite_args;
static struct {
	int argc;
	char ** argv;
} thread_parms ;
static void * squeezelite_runner_thread(){
    ESP_LOGI(TAG ,"Calling squeezelite");
	main(thread_parms.argc,thread_parms.argv);
	return NULL;
}
#define ADDITIONAL_SQUEEZELILTE_ARGS 5
static void * squeezelite_thread(){
	int * exit_code;
	static bool isRunning=false;
	if(isRunning) {
		ESP_LOGE(TAG,"Squeezelite already running. Exiting!");
		return NULL;
	}
	isRunning=true;
//  Let's not wait on WiFi to allow squeezelite to run in bluetooth mode
//	ESP_LOGI(TAG,"Waiting for WiFi.");
//	while(!wait_for_wifi()){usleep(100000);};
	ESP_LOGD(TAG ,"Number of args received: %u",thread_parms.argc );
	ESP_LOGD(TAG ,"Values:");
    for(int i = 0;i<thread_parms.argc; i++){
    	ESP_LOGD(TAG ,"     %s",thread_parms.argv[i]);
    }

    ESP_LOGD(TAG,"Starting Squeezelite runner Thread");
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name= "squeezelite-run";
    cfg.inherit_cfg = true;
    cfg.stack_size = SQUEEZELITE_THREAD_STACK_SIZE ;
    esp_pthread_set_cfg(&cfg);
	// no attribute if we want esp stack stack to prevail
    pthread_create(&thread_squeezelite_runner, NULL, squeezelite_runner_thread,NULL);
	// Wait for thread completion so we can free up memory.
	pthread_join(thread_squeezelite_runner,(void **)&exit_code);

	ESP_LOGV(TAG ,"Exited from squeezelite's main(). Freeing argv structure.");
	for(int i=0;i<thread_parms.argc;i++){
		ESP_LOGV(TAG ,"Freeing char buffer for parameter %u", i+1);
		free(thread_parms.argv[i]);
	}
	ESP_LOGV(TAG ,"Freeing argv pointer");
	free(thread_parms.argv);
	isRunning=false;
	return NULL;
}

static int launchsqueezelite(int argc, char **argv)
{
	ESP_LOGV(TAG ,"Begin");

    ESP_LOGD(TAG, "Parameters:");
    for(int i = 0;i<argc; i++){
    	ESP_LOGD(TAG, "     %s",argv[i]);
    }
    ESP_LOGV(TAG,"Saving args in thread structure");

    thread_parms.argc=0;
    thread_parms.argv = malloc(sizeof(char**)*(argc+ADDITIONAL_SQUEEZELILTE_ARGS));
	memset(thread_parms.argv,'\0',sizeof(char**)*(argc+ADDITIONAL_SQUEEZELILTE_ARGS));

	for(int i=0;i<argc;i++){
		ESP_LOGD(TAG ,"assigning parm %u : %s",i,argv[i]);
		thread_parms.argv[thread_parms.argc++]=strdup(argv[i]);
	}

	if(argc==1){
		// There isn't a default configuration that would actually work
		// if no parameter is passed.
		ESP_LOGV(TAG ,"Adding argv value at %u. Prev value: %s",thread_parms.argc,thread_parms.argv[thread_parms.argc-1]);
		thread_parms.argv[thread_parms.argc++]=strdup("-?");
	}

    ESP_LOGD(TAG,"Starting Squeezelite Thread");
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name= "squeezelite";
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
	pthread_create(&thread_squeezelite, NULL, squeezelite_thread,NULL);
	ESP_LOGD(TAG ,"Back to console thread!");
    return 0;
}
void register_squeezelite(){

	squeezelite_args.parameters = arg_str0(NULL, NULL, "<parms>", "command line for squeezelite. -h for help, --defaults to launch with default values.");
	squeezelite_args.end = arg_end(1);
	const esp_console_cmd_t launch_squeezelite = {
		.command = "squeezelite",
		.help = "Starts squeezelite",
		.hint = NULL,
		.func = &launchsqueezelite,
		.argtable = &squeezelite_args
	};
	ESP_ERROR_CHECK( esp_console_cmd_register(&launch_squeezelite) );

}
