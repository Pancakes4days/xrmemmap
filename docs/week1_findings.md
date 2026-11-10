# Week 1 findings (updated 2026-11-01)

Measured on the target: Meta Quest 1, serial 1PASH9AMUN9435, firmware
49845030443200410, running on **slot A**. All results below are from a single
device; nothing is generalized beyond it.

## Platform (verified)

| Fact | Value | How |
|------|-------|-----|
| Bootloader | unlocked (`ro.boot.flash.locked=0`) | getprop |
| Active slot | `_a` | `ro.boot.slot_suffix` |
| GPU / API | Adreno 540, Vulkan 1.1.128, driver `0x8022b000` | probe |
| KGSL node | `/dev/kgsl-3d0` present, world-rw | `ls -l` |
| ftrace | mounted at `/sys/kernel/debug/tracing` (tracefs) but **root-only** | mount + EACCES as shell |
| Root | non-persistent via ionstack (achieved Oct 31; currently blocked by pfn-oracle) | ionstack + `adb shell id` |

## Component 1 — pagemap reader: validated on-device

Self-test passes unprivileged. Confirmed it correctly: decodes present/swap/
exclusive/soft-dirty bits; reports a touched anon page present; reports an
untouched page not-present (`ENODATA`); and hits the `EACCES` "need root" path
for PFNs (unprivileged reads return a zeroed PFN, as expected since Linux 4.0).
Physical PFN **values** are the only thing gated on root.

## Priority #5 — Vulkan device extensions (44 total)

The four extensions we needed to confirm/rule out are **all ABSENT** on Adreno
540 / this driver:

- [ ] `VK_KHR_buffer_device_address`
- [ ] `VK_EXT_external_memory_host`
- [ ] `VK_KHR_shader_clock`
- [ ] `VK_EXT_host_query_reset`

### What this means for the architecture

- **No `buffer_device_address`** → we cannot read a GPU virtual address through
  the Vulkan API. This *confirms* the project's plan to recover GPU VAs from
  **KGSL ftrace (Component 2)** rather than the API — that path is now mandatory,
  not optional.
- **No `external_memory_host`** → we cannot import an existing host pointer into
  Vulkan. The shim must therefore work in the direction we already built:
  allocate with `vkAllocateMemory`, `vkMapMemory`, then resolve the resulting
  host VA with Component 1. Confirmed viable.
- **No `shader_clock`** → no in-shader timing source. DRAMA (Component 3) timing
  is CPU-side anyway, so this is not a blocker; note it as a limitation for any
  GPU-side timing ideas.
- **No `host_query_reset`** → minor; no impact on the current design.

### Potentially useful extensions that ARE present

- `VK_ANDROID_external_memory_android_hardware_buffer` — AHB import/export; ties
  into ION / dma-buf (cf. QuestStack ionstack reference).
- `VK_KHR_external_memory_fd`, `VK_EXT_queue_family_foreign` — dma-buf export.
- `VK_EXT_device_memory_report`, `VK_KHR_dedicated_allocation`.

Full capture: `component4_vulkan_shim/results_adreno540.txt`.

## Priority #6 — Vulkan goes through KGSL: confirmed

The probe inspects its own `/proc/self/{fd,maps}` after `vkCreateDevice`:
- fd 4 → `/dev/kgsl-3d0`
- four `rw-s` shared mappings of `/dev/kgsl-3d0` (the GPU memory regions)

So the Adreno Vulkan driver does route through `/dev/kgsl-3d0`, validating
Component 2's premise.

## Root access status

Non-persistent root was achieved on Oct 31 via ionstack (QuestStack's GhostLock
exploit, CVE-2021-1931 + ION heap). Confirmed `uid=0(root)` and
`selinux_permissive=1`. Root is lost on reboot and must be re-triggered.

As of Nov 1, ionstack's pfn-oracle stage is failing deterministically ("could
not arm the bit counters") across multiple boots and ~500 attempts with various
`IONSTACK_*` tuning variables. The KASLR bypass (perf sampling) succeeds every
time; only the slab-grooming stage fails. Same kernel, same firmware — the
failure appears to depend on per-boot KASLR memory layout affecting the
oracle's physical-page leak. Will continue retrying on future boots.

## Gated on root — blocked by ionstack reliability

- **Priority #3 (values):** real physical PFNs from the pagemap reader.
- **Priority #7:** confirm `kgsl_iommu_map` appears in
  `available_filter_functions` (all of tracefs is root-only here).

## Nov 4 demo status

**Demo-ready without root:**
- Component 1 pagemap reader — PASS, all flags decoded correctly
- Vulkan handle → CPU VA chain — working (handle `0x7e0dc26d00` → VA `0x7e0d24e000`)
- 44 device extensions enumerated, all 4 watched extensions confirmed ABSENT
- KGSL backing confirmed — Vulkan routes through `/dev/kgsl-3d0` (5 references)
- 64/64 Vulkan-allocated pages show `present=1` — mechanism proven, PFN gated on root

**Demo captures:** `component1_pagemap/results_pagemap_demo.txt`,
`component4_vulkan_shim/results_vulkan_demo.txt` (both Nov 1).

The physical-PFN column fills in as soon as ionstack cooperates on a future boot.
