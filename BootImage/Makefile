# SPDX-License-Identifier: GPL-2.0-only
CC := aarch64-linux-gnu-gcc
CFLAGS ?= -Os -static -Wall -Wextra -Werror
LDFLAGS ?= -static
ROOT := $(abspath ..)
DRM_SOURCE := $(ROOT)/module/drm_linear/drm_linear.c

.PHONY: all clean

all: build/init build/drm_linear

build/init: init.c
	mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

build/drm_linear: $(DRM_SOURCE)
	mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -rf build
