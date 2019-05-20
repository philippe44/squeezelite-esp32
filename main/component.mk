#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)
CFLAGS += -DPOSIX -DLINKALL -DLOOPBACK -DDACAUDIO -I$(COMPONENT_PATH)/../libmad -I$(COMPONENT_PATH)/../libflac/include -I$(COMPONENT_PATH)/../faad2/include
LDFLAGS += -s



