# dxvk-630

A DXVK fork tuned to squeeze maximum performance from **Intel UHD 630 (Gen9.5)** integrated GPUs. Targets low-power UMA systems where every cycle and byte of memory bandwidth counts.

Built on DXVK v3.0.2 with cherry-picks from the [Sarek](https://github.com/HansKristian-Work/dxvk) dyasync branch for async pipeline compilation and Gen9 low-power profile support.

## Changes from upstream DXVK

| Change | Description |
|--------|-------------|
| **Gen9 low-power profile** | Auto-detects UHD 630 hardware and applies memory budget caps (1024 MB), disables raw SSBO, enables async compilation |
| **Async pipeline compilation (dyasync)** | Compiles shader pipelines on background threads to eliminate stutter on slow iGPU hardware |
| **VkPipelineCache** | Persistent pipeline cache saved to disk, reused across runs to skip recompilation |
| **Narrowed pipeline barriers** | `ALL_COMMANDS_BIT` replaced with stage-specific barriers (`ALL_GRAPHICS_BIT`, `ALL_TRANSFER_BIT`) to reduce synchronization overhead |
| **Relaxed barriers** | `d3d11.relaxedBarriers = True` by default in config — removes redundant UAV barriers |
| **Max-perf defaults** | `dxvk.conf` shipped with aggressive settings: async compile, relaxed barriers, MSAA disabled, low anisotropy, LOD bias, capped tessellation, vsync off, UMA memory tuning |
| **Bugfix** | Removed spurious `static` on `computeFallbackKey()` that used `this` |

## Changes from Sarek

The Sarek fork contributed the dyasync infrastructure and Gen9 device detection. dxvk-630 adapts and simplifies those changes for the UHD 630 target only:

| Sarek feature | dxvk-630 status |
|---------------|-----------------|
| Async pipeline compilation (dyasync) | Kept and wired into auto-detected Gen9 profile |
| Gen9 low-power device profile | Kept and simplified — no multi-GPU branching |
| Frame pacer (Phase 6) | Ported — three modes: max-frame-latency (default), low-latency (predictive), min-latency (clamp only) |
| Multi-vendor GPU profiles (NVIDIA/AMD) | Not ported — Intel UHD 630 only |

## Building

Same as upstream DXVK:

```
git clone https://github.com/EzeriumChristinal/dxvk-630.git
cd dxvk-630

# 64-bit debug
meson setup --cross-file build-win64.txt --buildtype debug build-debug
ninja -C build-debug

# 64-bit release
meson setup --cross-file build-win64.txt --buildtype release build-release
ninja -C build-release
```

Requires llvm-mingw, glslang, and meson. See [Build instructions](https://github.com/doitsujin/dxvk#build-instructions) from upstream DXVK for details.

## Configuration

The shipped `dxvk.conf` contains all performance tweaks. Place it next to `dxvk.dll` or set `DXVK_CONFIG_FILE`. Options can also be set per-app by placing a `dxvk.conf` in the game directory.

## Credits

- [DXVK](https://github.com/doitsujin/dxvk) — the upstream project by Philip Rebohr
- [Sarek](https://github.com/HansKristian-Work/dxvk) — dyasync and Gen9 profile by Hans-Kristian Arntzen
