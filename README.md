MOST IMPORTANT: create the right default config file
- make defconfig
Then adapt the config file to your wifi/BT/I2C device (can alos be done on the command line)
- make menuconfig
Then 
- make -j4
- make flash monitor

Once the application is running, under monitor, add autoexec to launch squeezelite at boot

1/ setup WiFi

nvs_set autoexec1 str -v "join <SSID> <password>"

2/ setup squeezelite command line (optional)

nvs_set autoexec2 str -v "squeezelite -o I2S -b 500:2000 -d all=info -m ESP32"

3/ enable autoexec

nvs_set autoexec u8 -v 1		

The "join" and "squeezelite" commands can also be typed at the prompt to start manually. Use "help" to see the list.

The squeezelite options are very similar to the regular squeezelite options. Differences are :

	- the output is -o \"[BT -n <sinkname>] | [I2S]"\
	
	- if you've compiled RESAMPLE option, normal soxr options are available using -R [-u <options>]. Note that anything above LQ or MQ will overload the CPU
	
	- if you've used RESAMPLE16, <options> are (b|l|m)[:i], with b = basic linear interpolation, l = 13 taps, m = 21 taps, i = interpolate filter coefficients
	
To add options that require quotes ("), escape them with \"

# Additional misc notes
- for all libraries, add -mlongcalls 
- libmad, libflac (no esp's version), libvorbis (tremor - not esp's version), alac work
- libfaad does not really support real time, but if you want to try
	- -O3 -DFIXED_POINT -DSMALL_STACK
	- change ac_link in configure and case ac_files, remove ''
	- compiler but in cfft.c and cffti1, must disable optimization using 
			#pragma GCC push_options
			#pragma GCC optimize ("O0")
			#pragma GCC pop_options
- better use helixacc			
- set IDF_PATH=/home/esp-idf
- set ESPPORT=COM9
- update flash partition size
- other compiler #define 
	- use no resampling or set RESAMPLE (soxr) or set RESAMPLE16 for fast fixed 16 bits resampling
	- use LOOPBACK (mandatory)
	- use BYTES_PER_FRAME=4 (8 is not fully functionnal)
	- LINKALL (mandatory)
	- NO_FAAD unless you want to us faad, which currently overloads the CPU
	- TREMOR_ONLY (mandatory)
	
