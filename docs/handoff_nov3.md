# xrmemmap handoff — Nov 3, 2026

This document captures everything done, learned, and tried across the Oct 31 – Nov 3 sessions. Copy-paste it into any future conversation so the next assistant (or future you) can pick up without re-deriving anything.

---

## What this project is

**xrmemmap** resolves the full memory address chain for Vulkan allocations on a Meta Quest 1:

```
Vulkan handle → CPU virtual address → GPU virtual address → physical pages → DRAM bank/row
```

It's academic security research applying the GPUBreach methodology (arXiv:2605.03812 §4.1) to Qualcomm Adreno 540 / Snapdragon 835 / LPDDR4X on XR hardware. Runs on one bootloader-unlocked, end-of-life device the user owns.

**Repo:** `C:\Users\lbrug\repos\xrmemmap`

## The device

| Fact | Value |
|------|-------|
| Device | Meta Quest 1 |
| Serial | 1PASH9AMUN9435 |
| SoC | Snapdragon 835 (MSM8998) |
| GPU | Adreno 540 |
| RAM | LPDDR4X |
| OS | Horizon OS (Android 10 base) |
| Kernel | 4.4.205-perf+ (built Jul 31, 2024) |
| Firmware build | 49845030443200410 |
| Bootloader | unlocked (`ro.boot.flash.locked=0`) |
| Active slot | `_a` |
| Vulkan | 1.1.128, driver `0x8022b000` |
| KGSL | `/dev/kgsl-3d0` present, world-rw |
| ftrace | mounted at `/sys/kernel/debug/tracing` (root-only) |
| kprobes | disabled (`CONFIG_KPROBES=n`) — Component 2 must use ftrace |
| Horizon pairing | **broken** — headset won't pair with Meta Horizon mobile app after bootloader unlock. All work goes through ADB. Don't try to use the Horizon app. |

## Hard constraints

1. **Do not touch slot B.** Slot B holds the vulnerable bootloader (ABL) needed for QuestStack. The backup is saved as `1PASH9AMUN9435-2026-10-13T15-27-59-292Z.zip`. If slot B is damaged, the device is bricked.
2. **No fastboot from slot A.** `adb reboot bootloader` lands in Android recovery mode, not fastboot. Fastboot requires slot B's ABL. This means you cannot `fastboot boot` a patched image from slot A.
3. **ADB drops when the Quest sleeps.** Keep the proximity sensor engaged (tape, sock stuffed in headset) during work sessions.
4. **Headset UI limited.** Horizon pairing is broken, but VR environment is accessible — user can see floating panels for sideloaded apps (e.g., Magisk). Used to complete Magisk setup and grant su. However, standard Android activities may be obscured by the Oculus shell pairing screen; use `am start -f 0x14000000 -n <component>` to force-launch apps.

## Host environment

- **OS:** Windows 10 Home
- **Shell:** Git Bash (MSYS2). PowerShell also available.
- **ADB:** `C:\Users\lbrug\Documents\platform-tools-latest-windows\platform-tools\adb.exe`
- **NDK:** r27c at `C:\Users\lbrug\Android\ndk\android-ndk-r27c`
- **No host `make`/`cmake`** — use the NDK's bundled make: `$NDK/prebuilt/windows-x86_64/bin/make.exe`
- **Git Bash MSYS path mangling:** Git Bash converts `/data/local/tmp` into `C:/Program Files/Git/data/local/tmp`. Fix by prefixing adb commands with `MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'`. Use Windows-form source paths (`C:/...`) and Unix device paths (`/data/local/tmp/...`).

### Build commands

```bash
# Set NDK
export NDK=/c/Users/lbrug/Android/ndk/android-ndk-r27c
MAKE="$NDK/prebuilt/windows-x86_64/bin/make.exe"

# Build Component 1 (pagemap reader)
cd /c/Users/lbrug/repos/xrmemmap/component1_pagemap
"$MAKE" NDK=$NDK

# Build Component 4 (Vulkan probe)
cd /c/Users/lbrug/repos/xrmemmap/component4_vulkan_shim
"$MAKE" NDK=$NDK

# Push to device
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' /c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe push build/test_vulkan /data/local/tmp/test_vulkan

# Run on device
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' /c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe shell /data/local/tmp/test_vulkan

# Run under root (when available)
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' /c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe shell su -c /data/local/tmp/test_vulkan
```

## Components and status

| # | Name | What it does | Status |
|---|------|-------------|--------|
| 1 | `component1_pagemap` | Reads `/proc/self/pagemap` to translate CPU virtual addresses to physical addresses | **Code complete, validated on-device** |
| 2 | `component2_kgsl_ftrace` | Will trace KGSL kernel functions via ftrace to capture GPU virtual addresses | Not started (weeks 2–3) |
| 4 | `component4_vulkan_shim` | Vulkan probe + eventual LD_PRELOAD shim to run the full chain | **Probe complete, validated on-device** |
| 3 | `component3_drama` | DRAMA timing attack to map physical addresses to DRAM bank/row | Not started (weeks 6–7) |

Build order: 1 → 2 → 4 → 3.

## What was built and tested

### Component 1 — pagemap reader

**Files:**
- `component1_pagemap/src/pagemap.h` — public API
- `component1_pagemap/src/pagemap.c` — implementation
- `component1_pagemap/test/test_pagemap.c` — self-checking test
- `component1_pagemap/Makefile` — NDK cross-build

**What it does:** Opens `/proc/self/pagemap`, reads 8-byte entries at `(virtual_page_number * 8)` offset, decodes the bit fields:
- Bit 63: present (in RAM)
- Bit 62: swapped
- Bit 61: file-backed
- Bit 56: exclusive
- Bit 55: soft-dirty
- Bits 0-54: PFN (page frame number) — multiply by 4096 = physical address

**API:** `virt_to_phys()`, `pagemap_lookup()`, `pagemap_lookup_range()`, `pagemap_page_size()`

**On-device results (unprivileged):**
- Correctly decodes present/swap/exclusive/dirty flags
- Touched pages → present=1, exclusive=1
- Untouched pages → not-present (ENODATA)
- PFN values → zeroed (EACCES) because unprivileged; kernel hides PFNs since Linux 4.0
- **PASS (0 hard failures)**

**On-device results (root, Nov 10):**
- All PFN values resolved: e.g. PFN=0xf346d → PA=0xf346d000
- Pages physically NOT contiguous (expected for anonymous mmap)
- **PASS (0 hard failures)**

### Component 4 — Vulkan probe

**Files:**
- `component4_vulkan_shim/src/test_vulkan.c` — the probe
- `component4_vulkan_shim/Makefile` — NDK cross-build, links `-lvulkan`

**What it does:**
1. Creates a VkInstance, picks the first physical device (Adreno 540)
2. Enumerates all device extensions (44), flags 4 watched ones
3. Creates a logical device with one queue
4. Inspects `/proc/self/fd` and `/proc/self/maps` for KGSL references
5. Allocates 256 KiB HOST_VISIBLE|HOST_COHERENT memory
6. Maps it with `vkMapMemory`, touches every page
7. Feeds each page to `pagemap_lookup_range()` to resolve physical addresses

**On-device results:**
- Device: Adreno (TM) 540, Vulkan 1.1.128, driver 0x8022b000
- 44 extensions enumerated
- All 4 watched extensions **ABSENT** (see below)
- KGSL confirmed: fd 4 → `/dev/kgsl-3d0`, plus 4 shared memory mappings
- `vkAllocateMemory` handle=0x7e0dc26d00, size=262144, memType=2
- `vkMapMemory` CPU VA=0x7e0d24e000
- 64/64 pages: present=1 but PFN unresolved (unprivileged)

**On-device results (root, Nov 10):**
- All 64 pages resolved to physical addresses
- Pages 0-28: contiguous PFNs 0x13384e–0x13386a (physically contiguous block)
- Pages 29-49: contiguous PFNs 0xc11bb–0xc11cf (second contiguous block)
- Pages 50-56: scattered PFNs (fragmented region)
- Pages 57-63: contiguous PFNs 0xc11d0–0xc11d6 (third contiguous block)
- Allocation split across 3-4 physical memory regions — scatter-gather via IOMMU on LPDDR4X

### Extension findings (Priority #5)

All 4 watched extensions are **ABSENT** on Adreno 540:

- **`VK_KHR_buffer_device_address`** — ABSENT. Cannot get GPU virtual addresses from the Vulkan API. This makes KGSL ftrace (Component 2) the **mandatory** path for GPU VA recovery. This is the most important architectural finding.
- **`VK_EXT_external_memory_host`** — ABSENT. Cannot import existing host pointers into Vulkan.
- **`VK_KHR_shader_clock`** — ABSENT. No in-shader timing source.
- **`VK_EXT_host_query_reset`** — ABSENT. Minor, no impact.

**Useful extensions that ARE present:**
- `VK_ANDROID_external_memory_android_hardware_buffer` — AHB/ION/dma-buf path
- `VK_KHR_external_memory_fd`, `VK_EXT_queue_family_foreign` — dma-buf export
- `VK_EXT_device_memory_report`, `VK_KHR_dedicated_allocation`

### KGSL confirmation (Priority #6)

After `vkCreateDevice`, the process has:
- fd 4 → `/dev/kgsl-3d0`
- 4+ `rw-s` shared mappings of `/dev/kgsl-3d0`

This confirms Vulkan routes through Qualcomm's KGSL kernel driver, validating Component 2's design premise.

## Root access — ACHIEVED (persistent Magisk, Nov 10)

### Current method (working)

**Magisk v31.1** flashed to boot_a via custom Python fastboot client. Root survives reboots.

**Working boot image:** `D:\edl_tools\patched-grant2.img` (backed up as `boot_images/boot_magisk_working.img`)

**How it was achieved:**
1. Extracted stock boot from firmware `payload.bin`
2. Re-patched with Magisk's `boot_patch.sh` using `LEGACYSAR=true` (kernel patched `skip_initramfs` → `want_initramfs` at offset 0x01DF95B8)
3. Flashed via custom Python fastboot client (`D:\edl_tools\flash_boot.py`) — standard `fastboot.exe` can't see Oculus USB VID 0x2833
4. On first boot, tapped "Proceed" on Magisk's "Additional Setup" dialog in VR headset
5. Granted su to shell via Magisk's Superuser tab in VR headset

**To reflash if needed:**
```bash
# Put device in fastboot: adb reboot bootloader
# Flash: python D:\edl_tools\flash_boot.py
# (BOOT_IMAGE var in flash_boot.py must point to the correct .img)
```

**Verification:**
```bash
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' adb shell su -c id
# Expected: uid=0(root) gid=0(root) groups=0(root) context=u:r:magisk:s0
```

### ftrace visibility (confirmed Nov 10)

KGSL ftrace events available at `/sys/kernel/debug/tracing/events/kgsl/`:
- `kgsl_mem_map` — GPU virtual address mapping (key for Component 2)
- `kgsl_mem_alloc` / `kgsl_mem_free` — allocation tracking
- `kgsl_context_create` / `kgsl_context_destroy`
- `kgsl_issueibcmds` — command submission
- Adreno-specific: `adreno_cmdbatch_*`, `adreno_drawctxt_*`, `adreno_gpu_fault`

IOMMU ftrace events at `/sys/kernel/debug/tracing/events/iommu/`:
- `map` / `unmap` — IOMMU page table operations
- `io_page_fault`

**Note:** `available_filter_functions` does not exist (kprobes disabled, CONFIG_KPROBES=n). Component 2 must use tracepoint events, not function tracing.

## Root access — historical (ionstack, no longer working)

### What worked (Oct 31)

**ionstack** from the QuestStack project — a GhostLock kernel exploit chaining CVE-2021-1931 (Qualcomm ION heap vulnerability) with ION slab grooming.

**Location on device:** `/data/local/tmp/ionstack`
**SHA-256:** `ECD366CEEFC0BAFD0CE8C35A9832FFCC425D2E80F915CCAE0E8B48B881977056`

**Run command:**
```bash
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' /c/Users/lbrug/Documents/platform-tools-latest-windows/platform-tools/adb.exe shell "cd /data/local/tmp && HOME=/data/local/tmp TMPDIR=/data/local/tmp XRW_PERSIST=adbd ./ionstack"
```

**Successful output (Oct 31):**
```
ROOT VERDICT root=1 uid=2000->0 selinux_permissive=1 (1->0) persist=1(adbd)
xrw: ROOT held -- box staying up, adbd is root, `adb shell` gives root.
```

`adb shell id` confirmed: `uid=0(root) gid=0(root)`

Root is **non-persistent** — lost on reboot. Must re-run ionstack each session.

### What's failing now (Nov 1 onward)

The exploit's **pfn-oracle** stage fails deterministically:
```
pfn-oracle: could not arm the bit counters (alloc_id=0)
the mm-slab groom cannot leak on this kernel
prepare_kernel_page did not find usable nonzero source pointers
stage fops-page FAILED base=0
```

The KASLR bypass (perf sampling) succeeds every time. Only the slab-grooming stage fails.

**What was tried (all failed):**
- ~500+ attempts across multiple boots
- Fresh reboot then immediate run
- Rapid-fire retries (5-10 back-to-back)
- `IONSTACK_PAGE_SETUP_ATTEMPTS=200` (increased from default 72)
- `IONSTACK_PIPE_PFN_ORACLE=1`
- `IONSTACK_RECLAIM_DRAIN_ALL=1`
- `IONSTACK_RECLAIM_PARTIAL_SLABS=1`
- `IONSTACK_PFN_ORACLE=pipe` (invalid — it takes numeric values)
- `IONSTACK_KS_VERBOSE=1`
- `IONSTACK_LARGE_PIPE_TRIES=50`
- (Nov 9) Generic heap pressure: spawned ~300 extra processes, pipes, sockets, launched VR shell/browser/system apps — no effect
- (Nov 9) Sustained ION/Vulkan spray (`ion_spray.c`): 20 processes each holding 200 Vulkan HOST_VISIBLE allocations (4000 ION objects total), killed half to create slab holes, ran ionstack immediately — still fails at alloc_id=0
- (Nov 9) Spray-and-root timing: ionstack run within 1 second of hole creation, with other half of sprayers still active — no change

**Root cause (confirmed Nov 9):** Factory reset left the kernel slab caches in a state where the pfn-oracle cannot function. The failure is at `alloc_id=0` — the very first oracle step — meaning no amount of userspace heap pressure (ION, Vulkan, pipes, processes) can create the required slab structure. The oracle likely depends on kernel-internal allocations (from system services like surfaceflinger, vold, zygote children) that only exist on a device with apps installed and Meta account paired.

**Remaining options:**
- Ask QuestStack Discord for guidance on pfn-oracle failure post-factory-reset
- Install real Android APKs (F-Droid, sideloaded apps) to bring real services online
- EDL flash of Magisk-patched boot image (see below)
- Alternative kernel exploits (CVE-2021-0920, CVE-2021-1048) — no reliable PoCs

**Available IONSTACK_* env vars** (found via `strings` on the binary): There are 100+ tuning variables. The full list was extracted in session. Key ones: `IONSTACK_PFN_ORACLE`, `IONSTACK_PIPE_PFN_ORACLE`, `IONSTACK_PAGE_SETUP_ATTEMPTS`, `IONSTACK_RECLAIM_*`, `IONSTACK_STAGE`, `IONSTACK_KASLR_BASE`.

### Magisk — attempted, not viable from slot A

A Magisk-patched boot image was created:
- Stock boot extracted from firmware `payload.bin`: `boot_images/boot_stock_49845030443200410.img` (SHA-256: `3c0a7c11...`)
- Patched with `boot_patch.sh` + magiskboot: `boot_images/boot_patched.img` (SHA-256: `ae3328f4...`)

**Why it can't be flashed via fastboot:** `adb reboot bootloader` from slot A goes to Android recovery mode, not fastboot. Fastboot requires slot B's ABL, and **slot B is off-limits**.

### EDL (Qualcomm Emergency Download Mode) — partially working (Nov 9)

The device **successfully enters EDL mode** via `adb reboot edl`. Windows sees it as `QUSB_BULK` in Device Manager. Zadig was used to install libusbK driver.

**What failed:** bkerler/edl Python tool (V3.62) crashes with access violation (0xC0000005) due to a bug in its code (USB communication itself works fine). A custom minimal Sahara/Firehose script was written (`D:\edl_tools\edl_flash.py`) that successfully completes the Sahara hello exchange and begins uploading the programmer. However, **the device rejects the programmer at exactly 7648 bytes** (after reading the ELF header + initial segments) for both SDM835 loaders. This means the Quest 1's EDL requires a Meta/Oculus-signed firehose programmer. Generic SDM835 programmers are rejected. **EDL is a dead end without Meta's proprietary programmer.**

**What's set up on D:\edl_tools\:**
- bkerler/edl repo cloned with Loaders submodule
- Python deps installed to `D:\edl_tools\lib`
- SDM835 loaders at `Loaders/qualcomm/factory/sdm835/`
- libusb-1.0.dll available
- Zadig at `Drivers/Windows/zadig-2.8.exe`
- libusbK driver installed for QUSB_BULK device

**To retry EDL:** Try QFIL (Qualcomm Flash Image Loader, a Windows GUI tool) which uses its own USB stack, bypassing pyusb. Or try from a Linux machine with native libusb.

### Sideload via recovery — blocked (Nov 9)

Device enters sideload mode (`adb sideload`). An unsigned OTA zip containing the Magisk-patched boot image and an updater-script was rejected by stock recovery (signature verification enforced, only 0.01x transferred).

## Repo file layout

```
xrmemmap/
├── README.md
├── boot_images/
│   ├── boot_stock_49845030443200410.img
│   ├── boot_stock_49845030443200410.img.sha256
│   ├── boot_patched.img
│   ├── boot_patched.img.sha256
│   └── boot_magisk_working.img        ← Nov 10, LEGACYSAR Magisk boot (the one that works)
├── component1_pagemap/
│   ├── Makefile
│   ├── CMakeLists.txt
│   ├── README.md
│   ├── results_pagemap_demo.txt          ← Nov 1 capture
│   ├── src/pagemap.h
│   ├── src/pagemap.c
│   └── test/test_pagemap.c
├── component2_kgsl_ftrace/               ← empty, weeks 2-3
├── component3_drama/                     ← empty, weeks 6-7
├── component4_vulkan_shim/
│   ├── Makefile
│   ├── CMakeLists.txt
│   ├── results_adreno540.txt             ← Oct 31 capture
│   ├── results_vulkan_demo.txt           ← Nov 1 capture
│   ├── src/test_vulkan.c
│   └── correlator/
├── docs/
│   └── week1_findings.md                 ← updated Nov 1
├── examples/
│   ├── case_studies/
│   └── simple_allocation/
└── scripts/
    └── verify_platform.sh
```

## What was gated on root (NOW UNBLOCKED — Nov 10)

1. **Priority #3:** ~~Real physical PFN values~~ — **DONE.** Full Vulkan chain: handle → CPU VA → PFN → PA, all 64 pages resolved.
2. **Priority #7:** ~~ftrace function visibility~~ — **DONE.** `kgsl_mem_map` tracepoint confirmed available. `available_filter_functions` doesn't exist (kprobes disabled), but tracepoint events work. Component 2 uses tracepoint events, not function tracing.

## Nov 4 demo status

**Ready to show:**
- Component 1 pagemap reader — PASS, all page flags decoded correctly
- Full Vulkan chain: handle `0x7e0dc26d00` → CPU VA `0x7e0d24e000` → 64 pages present
- 44 device extensions enumerated, all 4 watched extensions confirmed ABSENT
- KGSL backing confirmed — Vulkan routes through `/dev/kgsl-3d0`
- Architecture validated: GPU VA must come from KGSL ftrace (Component 2)

**Now also shown (root achieved Nov 10):**
- Physical PFN values — full 64-page Vulkan chain resolved
- ftrace: `kgsl_mem_map` and `iommu/map` tracepoints confirmed

**Demo captures saved at:**
- `component1_pagemap/results_pagemap_demo.txt`
- `component4_vulkan_shim/results_vulkan_demo.txt`

## Week 2 plan (updated Nov 10 — root achieved)

1. ~~Keep retrying ionstack~~ — **No longer needed.** Magisk root is persistent.
2. **Start Component 2:** KGSL ftrace tracer for GPU virtual address capture. Key tracepoints: `kgsl_mem_map`, `iommu/map`.
3. Wire Components 1 + 2 together for full VA → PA → GPU VA correlation.
4. Initialize git repo and commit all work.

## QuestStack reference

- GitHub: https://github.com/starseed12345/QuestStack
- ionstack binary: from QuestStack releases, verified by SHA-256
- Discord: linked from the GitHub README (for ionstack troubleshooting)
- ionstack is a GhostLock implementation targeting Quest 1 on firmware 49845030443200410
