TOOL = $(CROSS_COMPILE)
CC = $(TOOL)gcc
CXX = $(TOOL)g++

FLAGS = -Wall -O2 -g $(INCLUDES) --sysroot=${PATH_SYSROOT}
LDFLAGS = -L${PATH_SYSROOT}/usr/lib/aarch64-linux-gnu -ldrm -lva-drm -lva
INCLUDES += -I. -I./common

TARGETS = h264dec_vaapi_test hevcdec_vaapi_test

COMMON_DIR = common
COMMON_SRCS = $(COMMON_DIR)/va_display.c $(COMMON_DIR)/bitstream.cpp \
              $(COMMON_DIR)/dumpframe.cpp
COMMON_OBJS = $(COMMON_SRCS:.c=.o)
COMMON_OBJS := $(COMMON_OBJS:.cpp=.o)

h264decode_SRCS	= h264decode.cpp
hevcdecode_SRCS	= hevcdecode.cpp

h264decode_OBJS = $(h264decode_SRCS:.cpp=.o)
hevcdecode_OBJS = $(hevcdecode_SRCS:.cpp=.o)

all: $(TARGETS)

h264dec_vaapi_test: $(COMMON_OBJS) $(h264decode_OBJS)
	$(CXX) $(FLAGS) -o $@ $^ $(LDFLAGS)

hevcdec_vaapi_test: $(COMMON_OBJS) $(hevcdecode_OBJS)
	$(CXX) $(FLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(FLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(FLAGS) -c -o $@ $<

clean:
	rm -f $(COMMON_OBJS) $(h264decode_OBJS) $(hevcdecode_OBJS) $(TARGETS)

.PHONY: all clean
