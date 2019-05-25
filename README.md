# Adding squeezelite
- libmad must be in a separated component otherwise linker whines about long call 
- libfaad
	- mlongcalls -O2 -DFIXED_POINT -DSMALL_STACK
	- change ac_link in configure and case ac_files, remove ''
	- compiler but in cfft.c and cffti1, must disable optimization using 
			#pragma GCC push_options
			#pragma GCC optimize ("O0")
			#pragma GCC pop_options
 - libflac can use espressif's version	
 - vorbis
	- set SPIRAM_MALLOC_ALWAYSINTERNAL to 2048 as it consumes a lot of 8K blocks and uses all internal memory - when no memoru, WiFI chip fails
 - set IDF_PATH=/home/esp-idf
 - set ESPPORT=COM9
 - <esp-idf>\components\partition_table\partitions_singleapp.csv to 2M instead of 1M (or more)
 - sdkconfig.defaults now has configuration options set to load a local partitions.csv file that was setup with 2M size
 - Make sure you validate the flash's size in serial flash config (for example set to 16M)
 - sdkconfig.defaults has main stack size set to 8000
 - change SPIRAM_MALLOC_ALWAYSINTERNAL to 2048 so that vorbis does not exhaust ISRAM, but allocates to SPIRAM instead. When it is echausted, WiFi driver can't allocate SPIRAM (although it should and setting the option to ask it to allocated SPIRAM does not work)
 - Other options are available through menuconfig. Ideally, build should be reconfigured or at least compared with sdkconfig.default
 
# Supporting Bluetooth a2dp output
- menuconfig now has a section for setting output type
- Output types are A2DP or DAC over I2S
- When A2DP is chosen, the audio device name has to be specified here

# Wifi SCAN Example

This example shows how to use scan of ESP32.

We have two way to scan, fast scan and all channel scan:

* fast scan: in this mode, scan will finish after find match AP even didn't scan all the channel, you can set thresholds for signal and authmode, it will ignore the AP which below the thresholds.

* all channel scan : scan will end after checked all the channel, it will store four of the whole matched AP, you can set the sort method base on rssi or authmode, after scan, it will choose the best one 

and try to connect. Because it need malloc dynamic memory to store match AP, and most of cases is to connect to better signal AP, so it needn't record all the AP matched. The number of matches is limited to 4 in order to limit dynamic memory usage. Four matches allows APs with the same SSID name and all possible auth modes - Open, WEP, WPA and WPA2.
