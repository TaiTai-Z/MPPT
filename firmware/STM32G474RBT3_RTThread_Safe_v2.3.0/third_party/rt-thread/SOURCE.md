# Source provenance

- Upstream: https://github.com/RT-Thread/rtthread-nano
- Commit: `8afd041631ec44653c2de45581bcdae15fb2ae06`
- Kernel version macros: 4.1.1
- Imported: 2026-08-22
- License: Apache-2.0, see `LICENSE`

Imported directories are `include`, `src`, and `libcpu/arm/cortex-m4`.

Local compatibility change: `src/thread.c` initializes the local close-result
variable to `RT_ERROR`. This removes an ARMClang uninitialized-return warning
for the no-heap build without changing the successful static-thread path.
