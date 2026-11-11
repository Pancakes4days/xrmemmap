# Makefile — Component 4 Vulkan LD_PRELOAD shim + probe, Android ARM64 via NDK.
#
# Builds:
#   build/libxrmemmap.so  — LD_PRELOAD shim (interceptor + correlator)
#   build/test_vulkan     — standalone Vulkan allocation probe
#   build/observe         — external process observer (reads /proc/<pid>/*)
#
#   make NDK=/c/Users/lbrug/Android/ndk/<version>
#   make run-shim    # push shim + test_vulkan, run with LD_PRELOAD under su
#   make run         # push + run standalone test_vulkan under su
#   make observe     # push + observe vrshell under su

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
KGSL       := ../component2_kgsl_ftrace/src
DRAMA      := ../component3_drama/src
CFLAGS     := -std=c11 -O2 -Wall -Wextra -I$(PAGEMAP) -I$(KGSL) -I$(DRAMA) -Isrc
LDLIBS     := -lvulkan
BUILD      := build

PROBE_SRCS   := src/test_vulkan.c $(PAGEMAP)/pagemap.c
SHIM_SRCS    := src/shim.c src/correlator.c $(PAGEMAP)/pagemap.c $(KGSL)/kgsl_ftrace.c $(DRAMA)/drama.c
SHIM_FLAGS   := -shared -fPIC
SHIM_LIBS    := -ldl
OBSERVE_SRCS := src/observe.c $(PAGEMAP)/pagemap.c $(DRAMA)/drama.c

.PHONY: all check-ndk push push-shim run run-shim observe clean

all: check-ndk $(BUILD)/test_vulkan $(BUILD)/libxrmemmap.so $(BUILD)/observe

check-ndk:
	@if [ -z "$(NDK)" ]; then \
	  echo "ERROR: set NDK=/path/to/android-ndk (or export ANDROID_NDK_HOME)"; exit 1; \
	fi
	@if [ ! -x "$(CC)" ]; then \
	  echo "ERROR: clang not found at $(CC) (check NDK/HOST_TAG)"; exit 1; \
	fi

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_vulkan: $(PROBE_SRCS) $(PAGEMAP)/pagemap.h src/test_vulkan.c | $(BUILD)
	$(CC) $(CFLAGS) $(PROBE_SRCS) $(LDLIBS) -o $@
	@echo "built $@ for $(ABI) (API $(API))"

$(BUILD)/libxrmemmap.so: $(SHIM_SRCS) src/correlator.h $(DRAMA)/drama.h | $(BUILD)
	$(CC) $(CFLAGS) $(SHIM_FLAGS) $(SHIM_SRCS) $(SHIM_LIBS) -o $@
	@echo "built $@ for $(ABI) (API $(API))"

$(BUILD)/observe: $(OBSERVE_SRCS) $(PAGEMAP)/pagemap.h $(DRAMA)/drama.h | $(BUILD)
	$(CC) $(CFLAGS) $(OBSERVE_SRCS) -o $@
	@echo "built $@ for $(ABI) (API $(API))"

push: all
	"$(ADB)" push $(BUILD)/test_vulkan $(DEVICE_DIR)/test_vulkan
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/test_vulkan

push-shim: all
	"$(ADB)" push $(BUILD)/libxrmemmap.so $(DEVICE_DIR)/libxrmemmap.so
	"$(ADB)" push $(BUILD)/test_vulkan $(DEVICE_DIR)/test_vulkan
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/test_vulkan
	"$(ADB)" shell chmod 644 $(DEVICE_DIR)/libxrmemmap.so

run: push
	"$(ADB)" shell su -c $(DEVICE_DIR)/test_vulkan

run-shim: push-shim
	"$(ADB)" shell su -c "LD_PRELOAD=$(DEVICE_DIR)/libxrmemmap.so $(DEVICE_DIR)/test_vulkan"

observe: all
	"$(ADB)" push $(BUILD)/observe $(DEVICE_DIR)/observe
	"$(ADB)" shell chmod 755 $(DEVICE_DIR)/observe
	"$(ADB)" shell su -c $(DEVICE_DIR)/observe

clean:
	rm -rf $(BUILD)
