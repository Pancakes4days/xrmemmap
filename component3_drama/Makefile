# Makefile — Component 3 DRAMA timing attack, Android ARM64 via NDK.
#
#   make NDK=/c/Users/lbrug/Android/ndk/<version>
#   make run        # push + run under su on the Quest

NDK        ?= $(ANDROID_NDK_HOME)
API        ?= 29
ABI        ?= arm64-v8a
TRIPLE     ?= aarch64-linux-android
HOST_TAG   ?= windows-x86_64

TOOLCHAIN  := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)
CC         := $(TOOLCHAIN)/bin/$(TRIPLE)$(API)-clang

ADB        ?= /c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe
DEVICE_DIR ?= /data/local/tmp

PAGEMAP    := ../component1_pagemap/src
CFLAGS     := -std=c11 -O2 -Wall -Wextra -I$(PAGEMAP) -Isrc
BUILD      := build

SRCS       := test/test_drama.c src/drama.c $(PAGEMAP)/pagemap.c

.PHONY: all check-ndk push run clean

all: check-ndk $(BUILD)/test_drama

check-ndk:
	@if [ -z "$(NDK)" ]; then \
	  echo "ERROR: set NDK=/path/to/android-ndk (or export ANDROID_NDK_HOME)"; exit 1; \
	fi
	@if [ ! -x "$(CC)" ]; then \
	  echo "ERROR: clang not found at $(CC) (check NDK/HOST_TAG)"; exit 1; \
	fi

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_drama: $(SRCS) src/drama.h $(PAGEMAP)/pagemap.h | $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) -o $@
	@echo "built $@ for $(ABI) (API $(API))"

push: all
	"$(ADB)" push $(BUILD)/test_drama $(DEVICE_DIR)/test_drama
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/test_drama

run: push
	"$(ADB)" shell su -c $(DEVICE_DIR)/test_drama

clean:
	rm -rf $(BUILD)
