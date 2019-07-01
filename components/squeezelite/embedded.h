#ifndef EMBEDDED_H
#define EMBEDDED_H

#include <inttypes.h>

/* 	must provide 
		- mutex_create_p
		- pthread_create_name
		- stack size
		- s16_t, s32_t, s64_t and u64_t
	can overload
		- exit
	recommended to add platform specific include(s) here
*/	
	
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN	256
#endif

#define STREAM_THREAD_STACK_SIZE  8 * 1024
#define DECODE_THREAD_STACK_SIZE 20 * 1024
#define OUTPUT_THREAD_STACK_SIZE  8 * 1024
#define IR_THREAD_STACK_SIZE      8 * 1024

typedef int16_t   s16_t;
typedef int32_t   s32_t;
typedef int64_t   s64_t;
typedef unsigned long long u64_t;

// all exit() calls are made from main thread (or a function called in main thread)
#define exit(code) { int ret = code; pthread_exit(&ret); }

#define mutex_create_p(m) mutex_create(m)

int	pthread_create_name(pthread_t *thread, _CONST pthread_attr_t  *attr, 
				   void *(*start_routine)( void * ), void *arg, char *name);

#endif // EMBEDDED_H
