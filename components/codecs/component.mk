#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)
COMPONENT_ADD_LDFLAGS=-l$(COMPONENT_NAME) 	\
	$(COMPONENT_PATH)/lib/libmad.a 			\
	$(COMPONENT_PATH)/lib/libesp-flac.a 	\
	$(COMPONENT_PATH)/lib/libhelix-aac.a 	\
	$(COMPONENT_PATH)/lib/libvorbisidec.a	\
	$(COMPONENT_PATH)/lib/libogg.a			\
	$(COMPONENT_PATH)/lib/libalac.a			\
	$(COMPONENT_PATH)/lib/libsoxr.a
	
	
	#$(COMPONENT_PATH)/lib/libfaad.a 	
	#$(COMPONENT_PATH)/lib/libvorbisidec.a
	#$(COMPONENT_PATH)/lib/libogg.a
	#$(COMPONENT_PATH)/lib/libesp-tremor.a
	#$(COMPONENT_PATH)/lib/libesp-ogg-container.a
	

	
	
