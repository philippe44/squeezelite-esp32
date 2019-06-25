#
# Component Makefile
#
# This Makefile should, at the very least, just include $(SDK_PATH)/Makefile. By default,
# this will take the sources in the src/ directory, compile them and link them into
# lib(subdirectory_name).a in the build directory. This behaviour is entirely configurable,
# please read the SDK documents if you need to do this.
#

COMPONENT_ADD_INCLUDEDIRS := .
CFLAGS += -Os -DPOSIX -DLINKALL -DLOOPBACK -DNO_FAAD -DEMBEDDED -DTREMOR_ONLY -DBYTES_PER_FRAME=4 	
CFLAGS += -D LOG_LOCAL_LEVEL=ESP_LOG_DEBUG
