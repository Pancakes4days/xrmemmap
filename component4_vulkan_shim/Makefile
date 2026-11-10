# Makefile — Component 4 Vulkan allocation probe, Android ARM64 via NDK.
#
# Builds test_vulkan by compiling Component 1's pagemap.c in directly and
# linking libvulkan from the NDK sysroot (no separate Vulkan SDK needed).
#
#   make NDK=/c/Users/lbrug/Android/ndk/<version>
#   make run        # push + run under su on the Quest
#
# Vulkan on Android needs API >= 24; we target 29 to match the project.

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
LDLIBS     := -lvulkan
BUILD      := build

SRCS       := src/test_vulkan.c $(PAGEMAP)/pagemap.c

.PHONY: all check-ndk push run clean

all: check-ndk $(BUILD)/test_vulkan

check-ndk:
	@if [ -z "$(NDK)" ]; then \
	  echo "ERROR: set NDK=/path/to/android-ndk (or export ANDROID_NDK_HOME)"; exit 1; \
	fi
	@if [ ! -x "$(CC)" ]; then \
	  echo "ERROR: clang not found at $(CC) (check NDK/HOST_TAG)"; exit 1; \
	fi

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_vulkan: $(SRCS) $(PAGEMAP)/pagemap.h src/test_vulkan.c | $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) $(LDLIBS) -o $@
	@echo "built $@ for $(ABI) (API $(API))"

push: all
	"$(ADB)" push $(BUILD)/test_vulkan $(DEVICE_DIR)/test_vulkan
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/test_vulkan

run: push
	"$(ADB)" shell su -c $(DEVICE_DIR)/test_vulkan

clean:
	rm -rf $(BUILD)
