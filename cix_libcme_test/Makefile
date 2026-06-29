TOOL = $(CROSS_COMPILE)
CC = $(TOOL)gcc
CXX = $(TOOL)g++

FLAGS = -Wall -O2 -g $(INCLUDES) --sysroot=${PATH_SYSROOT}
LDFLAGS = -L${PATH_SYSROOT}/usr/share/cix/lib -L${PATH_SYSROOT}/usr/share/cix/lib/cme -L$(PATH_SYSROOT)/usr/lib/aarch64-linux-gnu
LDFLAGS += -lyuv -ljpeg -lOpenCL -ldrm -ldrm_cix -lcme
INCLUDES += -I. -I${PATH_SYSROOT}/usr/include/cme

TARGETS = test_libcme

test_libcme_SRCS = $(wildcard *.c)

all: $(TARGETS)

test_libcme: $(test_libcme_SRCS)
	$(CXX) $(FLAGS) -o $@ $(test_libcme_SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
