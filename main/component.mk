#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)
CFLAGS += -DPOSIX -DLINKALL -DLOOPBACK -DDACAUDIO -DTREMOR_ONLY	-DBYTES_PER_FRAME=4	\
	-I$(COMPONENT_PATH)/../components/codecs/inc		\
	-I$(COMPONENT_PATH)/../components/codecs/inc/mad 	\
	-I$(COMPONENT_PATH)/../components/codecs/inc/faad2	\
	-I$(COMPONENT_PATH)/../components/codecs/inc/alac	\
	-I$(COMPONENT_PATH)/../components/codecs/inc/vorbis
LDFLAGS += -s



