
#pragma once
#include "time.h"
#include "sys/time.h"
#include "esp_system.h"
#define PERF_MAX LONG_MAX
#define MIN_MAX_VAL(x) x==PERF_MAX?0:x
#define CURR_SAMPLE_RATE output.current_sample_rate>0?output.current_sample_rate:1
#define FRAMES_TO_MS(f) (uint32_t)f*(uint32_t)1000/(uint32_t)(CURR_SAMPLE_RATE)
#ifdef BYTES_TO_FRAME
#define BYTES_TO_MS(b) FRAMES_TO_MS(BYTES_TO_FRAME(b))
#else
#define BYTES_TO_MS(b) FRAMES_TO_MS(b/BYTES_PER_FRAME)
#endif
#define SET_MIN_MAX(val,var) var=val; if(var<min_##var) min_##var=var; if(var>max_##var) max_##var=var; count_##var++; avgtot_##var+= var
#define SET_MIN_MAX_SIZED(val,var,siz) var=val; if(var<min_##var) min_##var=var; if(var>max_##var) max_##var=var; count_##var++; avgtot_##var+= var;size_##var=siz
#define RESET_MIN_MAX(var) min_##var=PERF_MAX; max_##var=0; avgtot_##var=0;count_##var=0;var=0;size_##var=0
#define RESET_MIN_MAX_DURATION(var) min_##var=PERF_MAX; max_##var=0; avgtot_##var=0;count_##var=0;var=0
#define DECLARE_MIN_MAX(var) static uint32_t min_##var = PERF_MAX, max_##var = 0, size_##var = 0, count_##var=0;uint64_t avgtot_##var = 0; uint32_t var=0
#define DECLARE_MIN_MAX_DURATION(var) static uint32_t min_##var = PERF_MAX, max_##var = 0, count_##var=0; uint64_t avgtot_##var = 0; uint32_t var=0
#define LINE_MIN_MAX_AVG(var) (uint32_t)(count_##var>0?avgtot_##var/count_##var:0)

#define LINE_MIN_MAX_FORMAT_HEAD1  "              +----------+----------+----------------+-----+----------------+"
#define LINE_MIN_MAX_FORMAT_HEAD2  "              |      max |      min |        average |     |        count   |"
#define LINE_MIN_MAX_FORMAT_HEAD3  "              |  (bytes) |  (bytes) |        (bytes) |     |                |"
#define LINE_MIN_MAX_FORMAT_HEAD4  "              +----------+----------+----------------+-----+----------------+"
#define LINE_MIN_MAX_FORMAT_FOOTER "              +----------+----------+----------------+-----+----------------+"
#define LINE_MIN_MAX_FORMAT                  "%14s|%10u|%10u|%16u|%5u|%16u|"
#define LINE_MIN_MAX(name,var) name,\
								MIN_MAX_VAL(max_##var),\
								MIN_MAX_VAL(min_##var),\
								LINE_MIN_MAX_AVG(var),\
								size_##var!=0?100*LINE_MIN_MAX_AVG(var)/size_##var:0,\
								count_##var

#define LINE_MIN_MAX_FORMAT_STREAM           "%14s|%10u|%10u|%16u|%5u|%16u|"
#define LINE_MIN_MAX_STREAM(name,var) name,\
										MIN_MAX_VAL(max_##var),\
										MIN_MAX_VAL(min_##var),\
										LINE_MIN_MAX_AVG(var),\
										size_##var!=0?100*LINE_MIN_MAX_AVG(var)/size_##var:0,\
										count_##var
#define LINE_MIN_MAX_DURATION_FORMAT "%14s%10u|%10u|%11u|%11u|"
#define LINE_MIN_MAX_DURATION(name,var) name,MIN_MAX_VAL(max_##var),MIN_MAX_VAL(min_##var), LINE_MIN_MAX_AVG(var), count_##var


#define TIME_MEASUREMENT_START(x) x=esp_timer_get_time()
#define TIME_MEASUREMENT_GET(x) (esp_timer_get_time()-x)

#define TIMED_SECTION_START_MS_FORCE(x,force) if(hasTimeElapsed(x,force)) {
#define TIMED_SECTION_START_MS(x) 		if(hasTimeElapsed(x,false)){
#define TIMED_SECTION_START_FORCE(x,force) 			TIMED_SECTION_START_MS(x * 1000UL,force)
#define TIMED_SECTION_START(x) 			TIMED_SECTION_START_MS(x * 1000UL)
#define TIMED_SECTION_END				}
static inline bool hasTimeElapsed(time_t delayMS, bool bforce)
{
	static time_t lastTime=0;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (lastTime <= tv.tv_sec * 1000 + tv.tv_usec / 1000 ||bforce)
	{
		lastTime = tv.tv_sec * 1000 + tv.tv_usec / 1000 + delayMS;
		return true;
	}
	else
		return false;
}

