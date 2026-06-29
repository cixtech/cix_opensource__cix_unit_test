TOOL = $(CROSS_COMPILE)
CC = $(TOOL)gcc
CXX = $(TOOL)g++

FLAGS = -Wall -O2 -g $(INCLUDES) --sysroot=${PATH_SYSROOT}
LDFLAGS = -L${PATH_SYSROOT}/usr/lib/aarch64-linux-gnu -ldrm -lva-drm -lva
INCLUDES += -I. -I./common

TARGETS = h264dec_vaapi_test hevcdec_vaapi_test vp9dec_vaapi_test

COMMON_SRCS := $(wildcard common/*.cpp)

all: $(TARGETS)

h264dec_vaapi_test: $(COMMON_SRCS) h264decode.cpp
	$(CXX) $(FLAGS) -o $@ $(COMMON_SRCS) h264decode.cpp $(LDFLAGS)

hevcdec_vaapi_test: $(COMMON_SRCS) hevcdecode.cpp
	$(CXX) $(FLAGS) -o $@ $(COMMON_SRCS) hevcdecode.cpp $(LDFLAGS)

vp9dec_vaapi_test: $(COMMON_SRCS) vp9decode.cpp
	$(CXX) $(FLAGS) -o $@ $(COMMON_SRCS) vp9decode.cpp $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
