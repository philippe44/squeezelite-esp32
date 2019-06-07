
#pragma once
#define FRAMES_TO_MS(f) output.current_sample_rate>0?1000*f/output.current_sample_rate:LONG_MIN
#ifdef BYTES_TO_FRAME
#define BYTES_TO_MS(b) FRAMES_TO_MS(BYTES_TO_FRAME(b))
#else
#define BYTES_TO_MS(b) FRAMES_TO_MS(b/BYTES_PER_FRAME)
#endif
#define LINE_MIN_MAX_FORMAT          "%14s%10d|%10d|%11d|%11d|"                 "  |%10d|%10d|%16d|%16d|%16d|"
#define LINE_MIN_MAX_DURATION_FORMAT "%14s%10d|%10d|%11d|%11d|%11d|"
#define LINE_MIN_MAX_FORMAT_STREAM   "%14s%10s|%10s|%11s|%11s|"                 "  |%10d|%10d|%16d|%16d|%16d|"
#define SET_MIN_MAX(val,var) var=val; if(var<min_##var) min_##var=var; if(var>max_##var) max_##var=var; count_##var++; avgtot_##var+= var
#define RESET_MIN_MAX(var,mv) min_##var=mv##_MAX; max_##var=mv##_MIN; avgtot_##var=0;count_##var=0
#define DECLARE_MIN_MAX(var,t,mv) static t min_##var = mv##_MAX, max_##var = mv##_MIN; uint32_t avgtot_##var = 0, count_##var=0; t var=0
#define LINE_MIN_MAX_AVG(var) count_##var>0?avgtot_##var/count_##var:-1
#define LINE_MIN_MAX(name,var) name,BYTES_TO_MS(max_##var), BYTES_TO_MS(min_##var),BYTES_TO_MS( var),BYTES_TO_MS( LINE_MIN_MAX_AVG(var)),max_##var,min_##var,LINE_MIN_MAX_AVG(var),count_##var,var
#define LINE_MIN_MAX_STREAM(name,var) name,"n/a","n/a","n/a","n/a",max_##var,min_##var,LINE_MIN_MAX_AVG(var),count_##var,var
#define LINE_MIN_MAX_DURATION(name,var) name,max_##var, min_##var, LINE_MIN_MAX_AVG(var), count_##var,var

#define TIME_MEASUREMENT_START(x) x=esp_timer_get_time()
#define TIME_MEASUREMENT_GET(x) (esp_timer_get_time()-x)/1000

#define TIMED_SECTION_START_MS_FORCE(x,force) if(hasTimeElapsed(x,force)) {
#define TIMED_SECTION_START_MS(x) 		if(hasTimeElapsed(x,false)){
#define TIMED_SECTION_START_FORCE(x,force) 			TIMED_SECTION_START_MS(x * 1000UL,force)
#define TIMED_SECTION_START(x) 			TIMED_SECTION_START_MS(x * 1000UL)
#define TIMED_SECTION_END				}
static inline bool hasTimeElapsed(time_t delayMS, bool bforce)
{
	static time_t lastTime=0;
	if (lastTime <= gettime_ms() ||bforce)
	{
		lastTime = gettime_ms() + delayMS;
		return true;
	}
	else
		return false;
}
//#define MAX_PERF_NAME_LEN 10
//#define MAX_PERF_FORMAT_LEN 12
//typedef enum  {BUFFER_TYPE,DURATION_TYPE,LAST } perf_formats;
//typedef struct _perf_stats {
//	uint32_t min;
//	uint32_t max;
//	uint32_t current;
//	char name[MAX_PERF_NAME_LEN+1];
//	uint32_t timer_start;
//	perf_formats fmt;
//} perf_stats;
