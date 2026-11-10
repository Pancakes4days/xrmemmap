# Component 1 — pagemap reader (CPU virtual → physical)

Resolves a userspace virtual address to its backing physical address by parsing
`/proc/self/pagemap`. This is the first link in xrmemmap's address chain; the
Vulkan test program (Component 4) links `libpagemap` in-process and calls
`virt_to_phys()` on memory it maps with `vkMapMemory`.

## API

```c
#include "pagemap.h"

uint64_t pa = virt_to_phys(ptr);           // PAGEMAP_INVALID_PADDR on failure, errno set
uint64_t pa = pagemap_lookup(ptr, &entry); // same, plus decoded present/swap/dirty flags
int rc = pagemap_lookup_range(ptr, len, entries, &n);  // one entry per page
```

See `src/pagemap.h` for the full contract.

## Root requirement

Since Linux 4.0, unprivileged reads of `pagemap` return a **zeroed PFN** field
(the present/swap bits stay visible) as side-channel hardening. On this device
that means real physical addresses require root (CAP_SYS_ADMIN) — i.e. Magisk
must be installed and the binary run under `su`. Without root, `virt_to_phys()`
returns `PAGEMAP_INVALID_PADDR` with `errno == EACCES` for present pages, and
the self-test reports those as a soft skip rather than a failure.

## Build (Android ARM64, for the Quest)

With the NDK installed and `NDK` pointing at it:

```sh
make NDK=/c/Users/lbrug/Android/ndk/<version>
make run        # push to /data/local/tmp and run under su over adb
```

Or via CMake with the NDK toolchain file:

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
cmake --build build
```

A plain host `cmake -S . -B build` also compiles (Linux/WSL), useful for a quick
syntax/logic check — but PFN values only mean anything on the target under root.

## Self-test

`test/test_pagemap.c` allocates a multi-page anonymous mapping, touches every
page, and resolves each; checks that an untouched page reads back not-present;
and exercises the range API. Under root it also reports whether the
virtually-contiguous buffer is physically contiguous (it generally is not —
that fragmentation is exactly what the later components map).

Expected output (under root) looks like:

```
[anon mmap, 4 pages, touched]
  page[0]    VA=0x...  present=1 ... PFN=0x...  PA=0x...
  ...
  physically NOT contiguous
PASS (0 hard failures)
```
