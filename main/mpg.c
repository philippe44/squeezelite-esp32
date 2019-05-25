#include "squeezelite.h"

extern log_level loglevel;

struct codec *register_mpg(void) {
	LOG_INFO("mpg unavailable");
	return NULL;
}
