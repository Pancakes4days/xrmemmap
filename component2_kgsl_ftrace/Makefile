# Makefile — Component 2 KGSL ftrace tracer, Android ARM64 cross-build via NDK.
#
# Targets the Quest 1 (Snapdragon 835, arm64-v8a, API 29 / Android 10).
#
#   make NDK=/c/Users/lbrug/Android/ndk/android-ndk-r27c
#   make push
#   make run

NDK        ?= $(ANDROID_NDK_HOME)
API        ?= 29
ABI        ?= arm64-v8a
TRIPLE     ?= aarch64-linux-android
HOST_TAG   ?= windows-x86_64

TOOLCHAIN  := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)
CC         := $(TOOLCHAIN)/bin/$(TRIPLE)$(API)-clang

ADB        ?= /c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe
DEVICE_DIR ?= /data/local/tmp

CFLAGS     := -std=c11 -O2 -Wall -Wextra -Wpedantic -Isrc -I.. -static-libgcc
LDFLAGS    := -ldl
BUILD      := build

# Component 2 sources
SRC        := src/kgsl_ftrace.c
HDR        := src/kgsl_ftrace.h
TEST_SRC   := test/test_kgsl_ftrace.c

# Component 1 (pagemap) dependency — used by the test for full chain
PAGEMAP_SRC := ../component1_pagemap/src/pagemap.c
PAGEMAP_HDR := ../component1_pagemap/src/pagemap.h

.PHONY: all check-ndk push run clean

all: check-ndk $(BUILD)/test_kgsl_ftrace

check-ndk:
	@if [ -z "$(NDK)" ]; then \
	  echo "ERROR: set NDK=/path/to/android-ndk (or export ANDROID_NDK_HOME)"; exit 1; \
	fi
	@if [ ! -x "$(CC)" ]; then \
	  echo "ERROR: clang not found at $(CC)"; exit 1; \
	fi

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_kgsl_ftrace: $(SRC) $(HDR) $(TEST_SRC) $(PAGEMAP_SRC) $(PAGEMAP_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -I../component1_pagemap/src $(SRC) $(PAGEMAP_SRC) $(TEST_SRC) -o $@ $(LDFLAGS)
	@echo "built $@ for $(ABI) (API $(API))"

push: all
	"$(ADB)" push $(BUILD)/test_kgsl_ftrace $(DEVICE_DIR)/test_kgsl_ftrace
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/test_kgsl_ftrace

run: push
	"$(ADB)" shell su -c $(DEVICE_DIR)/test_kgsl_ftrace

clean:
	rm -rf $(BUILD)
