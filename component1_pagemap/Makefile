# Makefile — Component 1 pagemap reader, Android ARM64 cross-build via NDK.
#
# Targets the Quest 1 (Snapdragon 835, arm64-v8a, API 29 / Android 10).
# Point NDK at your install, then `make`. Output lands in build/.
#
#   make NDK=/c/Users/lbrug/Android/ndk/26.1.10909125
#   make push        # adb push the test binary to the device
#   make run         # run it under su on the device
#
# The host adb is the Windows platform-tools build; override ADB if it moves.

NDK        ?= $(ANDROID_NDK_HOME)
API        ?= 29
ABI        ?= arm64-v8a
TRIPLE     ?= aarch64-linux-android
HOST_TAG   ?= windows-x86_64

TOOLCHAIN  := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)
CC         := $(TOOLCHAIN)/bin/$(TRIPLE)$(API)-clang

ADB        ?= /c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe
DEVICE_DIR ?= /data/local/tmp

CFLAGS     := -std=c11 -O2 -Wall -Wextra -Wpedantic -Isrc -static-libgcc
BUILD      := build

SRC        := src/pagemap.c
HDR        := src/pagemap.h
TEST_SRC   := test/test_pagemap.c

.PHONY: all check-ndk push run clean

all: check-ndk $(BUILD)/test_pagemap

check-ndk:
	@if [ -z "$(NDK)" ]; then \
	  echo "ERROR: set NDK=/path/to/android-ndk (or export ANDROID_NDK_HOME)"; exit 1; \
	fi
	@if [ ! -x "$(CC)" ]; then \
	  echo "ERROR: clang not found at $(CC)"; \
	  echo "       check NDK version/HOST_TAG (HOST_TAG=$(HOST_TAG))"; exit 1; \
	fi

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_pagemap: $(SRC) $(HDR) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(TEST_SRC) -o $@
	@echo "built $@ for $(ABI) (API $(API))"

push: all
	"$(ADB)" push $(BUILD)/test_pagemap $(DEVICE_DIR)/test_pagemap
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/test_pagemap

run: push
	"$(ADB)" shell su -c $(DEVICE_DIR)/test_pagemap

clean:
	rm -rf $(BUILD)
