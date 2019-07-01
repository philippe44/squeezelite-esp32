#ifndef EMBEDDED_H
#define EMBEDDED_H

#include <inttypes.h>

#define HAS_MUTEX_CREATE_P		0
#define HAS_PTHREAD_SETNAME_NP	1

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

int pthread_setname_np(pthread_t thread, const char *name);

#endif // EMBEDDED_H
