/*
 *  AirCast: Chromecast to AirPlay
 *
 *  (c) Philippe 2016-2017, philippe_44@outlook.com
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

#ifndef __RAOP_H
#define __RAOP_H

#include "platform.h"
#include "raop_sink.h"

struct raop_ctx_s*   raop_create(struct in_addr host, char *name, unsigned char mac[6], int latency,
							     raop_cmd_cb_t cmd_cb, raop_data_cb_t data_cb);
void  		  raop_delete(struct raop_ctx_s *ctx);
void		  raop_cmd(struct raop_ctx_s *ctx, raop_event_t event, void *param);

#endif
