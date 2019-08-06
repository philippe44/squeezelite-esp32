#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)
CFLAGS += -O3 -DLINKALL -DLOOPBACK -DNO_FAAD -DRESAMPLE16 -DEMBEDDED -DTREMOR_ONLY -DBYTES_PER_FRAME=4 	\
	-I$(COMPONENT_PATH)/../codecs/inc			\
	-I$(COMPONENT_PATH)/../codecs/inc/mad 		\
	-I$(COMPONENT_PATH)/../codecs/inc/alac		\
	-I$(COMPONENT_PATH)/../codecs/inc/helix-aac	\
	-I$(COMPONENT_PATH)/../codecs/inc/vorbis 	\
	-I$(COMPONENT_PATH)/../codecs/inc/soxr 		\
	-I$(COMPONENT_PATH)/../codecs/inc/resample16	\
	-I$(COMPONENT_PATH)/../tools				\
	-I$(COMPONENT_PATH)/../codecs/inc/opus 		\
	-I$(COMPONENT_PATH)/../codecs/inc/opusfile

#	-I$(COMPONENT_PATH)/../codecs/inc/faad2



