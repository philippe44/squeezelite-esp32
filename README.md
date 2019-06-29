MOST IMPORTANT: create the right default config file
- make defconfig
Then adapt the config file to your wifi/BT/I2C device (can alos be done on the command line)
- make menuconfig
Then 
- make -j4
- make flash monitor

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
