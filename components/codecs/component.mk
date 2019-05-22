#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)
COMPONENT_ADD_LDFLAGS=$(COMPONENT_PATH)/lib/libmad.a $(COMPONENT_PATH)/lib/libesp-flac.a $(COMPONENT_PATH)/lib/libfaad.a -l$(COMPONENT_NAME)
#COMPONENT_ADD_LDFLAGS=$(COMPONENT_PATH)/lib/libesp-flac.a $(COMPONENT_PATH)/lib/libfaad.a -l$(COMPONENT_NAME)
