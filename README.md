# xrmemmap

A measurement tool that resolves the full memory address chain for Vulkan
allocations on a Meta Quest 1 (Snapdragon 835, Adreno 540, LPDDR4X). For any
Vulkan allocation it reports:

```
Vulkan handle → CPU virtual address → GPU virtual address → physical pages → DRAM bank/row
```

This is research instrumentation for characterizing GPU/XR memory layout,
applying the empirical driver-measurement methodology of GPUBreach (Lin et al.,
IEEE S&P 2026, arXiv:2605.03812 §4.1) to Qualcomm Adreno on XR hardware for the
first time. It runs on a single bootloader-unlocked, end-of-life device the
author owns.

## Components

| # | Name | Resolves | Status |
|---|------|----------|--------|
| 1 | `component1_pagemap` | CPU virtual → physical (`/proc/self/pagemap`) | code complete |
| 2 | `component2_kgsl_ftrace` | GPU virtual → physical (KGSL ftrace) | weeks 2–3 |
| 4 | `component4_vulkan_shim` | end-to-end tool (LD_PRELOAD shim + correlator) | weeks 4–5 |
| 3 | `component3_drama` | physical → DRAM bank/row (DRAMA timing) | weeks 6–7 |

(Build order is 1 → 2 → 4 → 3, as numbered in the project plan.)

## Platform

- Meta Quest 1, Horizon OS (Android 10 base), bootloader unlocked
- ftrace available at `/sys/kernel/debug/tracing/`; kprobes disabled in the
  shipped kernel (`CONFIG_KPROBES=n`), so Component 2 uses ftrace, not kprobes
- Root is via Magisk and not persistent across the toolchain until installed;
  real PFNs need root (see `component1_pagemap/README.md`)

## Getting started

```sh
# With the Quest awake and authorized over adb:
bash scripts/verify_platform.sh      # read-only Week 1 platform checks

# Build + run Component 1 (needs the Android NDK):
cd component1_pagemap
make run NDK=/path/to/android-ndk
```

## References

- GPUBreach — Lin et al., IEEE S&P 2026, arXiv:2605.03812 (primary methodology)
- GPUHammer — USENIX Security 2025 (predecessor)
- DRAMA — Pessl et al., USENIX Security 2016 (Component 3 methodology)
- Linux pagemap — `Documentation/admin-guide/mm/pagemap.rst`
- Linux ftrace — `Documentation/trace/ftrace.txt`

## License

See `LICENSE` (MIT or Apache-2.0, TBD). Open-source release planned for Week 8.
