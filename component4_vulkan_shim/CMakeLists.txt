# CMakeLists.txt — Component 4 (Vulkan test program; the LD_PRELOAD shim lands
# here later in weeks 4-5). For now it builds the compute-only allocation probe.
#
# Cross-compile for the Quest with the NDK toolchain file:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
#   cmake --build build
#
# libvulkan is provided by the NDK sysroot; the Vulkan headers come with it.

cmake_minimum_required(VERSION 3.18)
project(xrmemmap_component4 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Reuse Component 1's pagemap library in-tree.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../component1_pagemap
                 ${CMAKE_BINARY_DIR}/component1_build)

find_library(VULKAN_LIB vulkan)
if(NOT VULKAN_LIB)
    message(FATAL_ERROR "libvulkan not found — are you using the NDK toolchain file?")
endif()

add_executable(test_vulkan src/test_vulkan.c)
target_link_libraries(test_vulkan PRIVATE pagemap ${VULKAN_LIB})
target_compile_options(test_vulkan PRIVATE -Wall -Wextra -O2)
