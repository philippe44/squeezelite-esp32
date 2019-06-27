/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2017, ralph_irving@hotmail.com
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

// upsampling using libsoxr - only included if RESAMPLE set

#include "squeezelite.h"

#if RESAMPLE16

#include <resample16.h>

extern log_level loglevel;

struct resample16 {
	struct resample16_s *resampler;
	bool max_rate;
	bool exception;
	bool interp;
	resample16_filter_e filter;
};

static struct resample16 r;

void resample_samples(struct processstate *process) {
	ssize_t odone;
	
	odone = resample16(r.resampler, (HWORD*) process->inbuf, process->in_frames, (HWORD*) process->outbuf);

	if (odone < 0) {
		LOG_INFO("resample16 error");
		return;
	}
	
	process->out_frames = odone;
	process->total_in  += process->in_frames;
	process->total_out += odone;
}

bool resample_drain(struct processstate *process) {
	process->out_frames = 0;
	
	LOG_INFO("resample track complete");

	resample16_delete(r.resampler);
	r.resampler = NULL;

	return true;
}

bool resample_newstream(struct processstate *process, unsigned raw_sample_rate, unsigned supported_rates[]) {
	unsigned outrate = 0;
	int i;

	if (r.exception) {
		// find direct match - avoid resampling
		for (i = 0; supported_rates[i]; i++) {
			if (raw_sample_rate == supported_rates[i]) {
				outrate = raw_sample_rate;
				break;
			}
		}
		// else find next highest sync sample rate
		while (!outrate && i >= 0) {
			if (supported_rates[i] > raw_sample_rate && supported_rates[i] % raw_sample_rate == 0) {
				outrate = supported_rates[i];
				break;
			}
			i--;
		}
	}

	if (!outrate) {
		if (r.max_rate) {
			// resample to max rate for device
			outrate = supported_rates[0];
		} else {
			// resample to max sync sample rate
			for (i = 0; supported_rates[i]; i++) {
				if (supported_rates[i] % raw_sample_rate == 0 || raw_sample_rate % supported_rates[i] == 0) {
					outrate = supported_rates[i];
					break;
				}
			}
		}
		if (!outrate) {
			outrate = supported_rates[0];
		}
	}

	process->in_sample_rate = raw_sample_rate;
	process->out_sample_rate = outrate;

	if (r.resampler) {
		resample16_delete(r.resampler);
		r.resampler = NULL;
	}

	if (raw_sample_rate != outrate) {

		LOG_INFO("resampling from %u -> %u", raw_sample_rate, outrate);
		r.resampler = resample16_create((float) outrate / raw_sample_rate, RESAMPLE16_SMALL, false);

		return true;

	} else {

		LOG_INFO("disable resampling - rates match");
		return false;
	}
}

void resample_flush(void) {
	if (r.resampler) {
		resample16_delete(r.resampler);
		r.resampler = NULL;
	}
}

bool resample_init(char *opt) {
	char *filter = NULL, *interp = NULL;
	
	r.resampler = NULL;
	r.max_rate = false;
	r.exception = false;

	if (opt) {
		filter = next_param(opt, ':');
		interp = next_param(NULL, ':');
	}

	if (filter) {
		if (*filter == 'm') r.filter = RESAMPLE16_SMALL;
		else r.filter = RESAMPLE16_FAST;
	}

	if (interp && *interp == 'i') {
		r.interp = true;	
	}
	
	LOG_INFO("Resampling with filter %d %s", r.filter, r.interp ? "(interpolated)" : "");

	return true;
}

#endif // #if RESAMPLE16
