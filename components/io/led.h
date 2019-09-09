/* 
 *  Squeezelite for esp32
 *
 *  (c) Sebastien 2019
 *      Philippe G. 2019, philippe_44@outlook.com
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
 
#ifndef LED_H
#define LED_H
#include "driver/gpio.h"

enum { LED_GREEN = 0, LED_RED };

#define led_on(idx)						led_blink_core(idx, 1, 0, false)
#define led_off(idx)					led_blink_core(idx, 0, 0, false)
#define led_blink(idx, on, off)			led_blink_core(idx, on, off, false)
#define led_blink_pushed(idx, on, off)	led_blink_core(idx, on, off, true)

bool led_config(int idx, gpio_num_t gpio, int onstate);
bool led_unconfig(int idx);
bool led_blink_core(int idx, int ontime, int offtime, bool push);
bool led_unpush(int idx);

#endif
