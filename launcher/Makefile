# RetroOPI launcher — cross-compilation for Allwinner H618 (Cortex-A53, AArch64)
#
# Standalone build against the Buildroot toolchain:
#   make SYSROOT=$HOME/opi/output/host/aarch64-buildroot-linux-gnu/sysroot \
#        CROSS_COMPILE=$HOME/opi/output/host/bin/aarch64-buildroot-linux-gnu-
#
# Output geometry (see launcher.c). It must be a mode the HDMI sink offers:
#   make                              # 1280x720 default
#   make SCREEN_W=1920 SCREEN_H=1080  # 1080p
#
# When built as a Buildroot package this inherits TARGET_CC.

CROSS_COMPILE ?=
SYSROOT       ?=
SCREEN_W      ?= 1280
SCREEN_H      ?= 720

CC    = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

TARGET = retroopi_launcher
OBJS   = launcher.o gfx_accel.o

# AArch64 has NEON and hard-float unconditionally, so there are no -mfpu or
# -mfloat-abi flags to carry over from the ARMv7 build.
CFLAGS  = -Wall -Wextra -O2 -mcpu=cortex-a53
CFLAGS += -DSCREEN_WIDTH=$(SCREEN_W) -DSCREEN_HEIGHT=$(SCREEN_H)
ifneq ($(SYSROOT),)
CFLAGS += --sysroot=$(SYSROOT)
CFLAGS += -I$(SYSROOT)/usr/include/SDL2
CFLAGS += -I$(SYSROOT)/usr/include/libdrm
endif

LDFLAGS =
ifneq ($(SYSROOT),)
LDFLAGS += --sysroot=$(SYSROOT)
endif
# No -lrga: there is no RGA2 engine off Rockchip (see gfx_accel.c).
LDFLAGS += -lSDL2_ttf -lSDL2_image -lSDL2 -ldrm -lfreetype -lz -lpthread -lm

all: $(TARGET)

launcher.o: launcher.c gfx_accel.h
	$(CC) $(CFLAGS) -c -o $@ $<

gfx_accel.o: gfx_accel.c gfx_accel.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)
	$(STRIP) $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
