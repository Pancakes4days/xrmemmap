# CMakeLists.txt — Component 1 pagemap reader.
#
# Builds a static lib (libpagemap) linked by the test exerciser here and, later,
# by the Vulkan shim/test program in Component 4. Cross-compile for the Quest
# with the NDK toolchain file:
#
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
#   cmake --build build
#
# Builds for the host too (plain `cmake -S . -B build`) for quick edit/compile
# checks, though PFN resolution only means anything on the target under root.

cmake_minimum_required(VERSION 3.18)
project(xrmemmap_component1 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(pagemap STATIC src/pagemap.c)
target_include_directories(pagemap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(pagemap PRIVATE -Wall -Wextra -Wpedantic -O2)

add_executable(test_pagemap test/test_pagemap.c)
target_link_libraries(test_pagemap PRIVATE pagemap)
target_compile_options(test_pagemap PRIVATE -Wall -Wextra -Wpedantic -O2)
