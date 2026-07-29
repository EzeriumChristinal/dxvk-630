# dxvk-630

DXVK fork tuned for **Intel UHD 630 (Gen9.5)** iGPUs on low-power UMA systems with constrained memory bandwidth.

Built on DXVK v3.0.2 with cherry-picks from the [Sarek](https://github.com/HansKristian-Work/dxvk) dyasync branch for async pipeline compilation and Gen9 low-power profile support.

## Changes from upstream DXVK

| Change | Description |
|--------|-------------|
| **Gen9 low-power profile** | Auto-detects UHD 630 hardware, applies memory budget caps (1024 MB), disables raw SSBO, enables async compilation |
| **Async pipeline compilation (dyasync)** | Background thread pipeline compilation eliminates stutter on slow iGPU |
| **VkPipelineCache** | Persistent pipeline cache saved to disk, reused across runs |
| **Narrowed pipeline barriers** | `ALL_COMMANDS_BIT` replaced with stage-specific barriers (`ALL_GRAPHICS_BIT`, `ALL_TRANSFER_BIT`) |
| **Relaxed barriers** | `d3d11.relaxedBarriers = True` by default |
| **Max-perf defaults** | Aggressive `dxvk.conf`: async compile, relaxed barriers, MSAA disabled, low anisotropy, LOD bias, capped tessellation, vsync off, UMA memory tuning |
| **Frame pacer (Phase 6, Sarek port)** | Three modes: max-frame-latency (default), low-latency (predictive), min-latency (clamp only) |
| **ALLOW_TEARING flag forwarding** | `VK_PRESENT_MODE_IMMEDIATE_KHR` support for DXGI flip model |
| **forceHdr option** | Forces HDR10 output on supported displays |
| **Gen9 profile respects user config** | User `dxvk.conf` overrides built-in Gen9 profile defaults |
| **Missing Gen9 device IDs** | Added Broxton/APL/0x22xx Gen9 LP device IDs |
| **IsCurrent adapter tracking** | `IDXGIOutput::IsCurrent` re-enumerates Vulkan physical devices, detects hotplug |
| **Threading/fix batch** | Shader cache race fix, DXGI options shift overflow, `RegisterDeviceRemovedEvent`, `MakeWindowAssociation`, sync interval fix, `BufferCount` pick fix |
| **NT handle leak fix** | `CloseHandle` on failure paths in D3D11 device NT handle open |
| **Present1 dirty rects** | Null `pPresentParameters` converted to empty params |
| **LTO enabled** | Link-time optimization for release builds |
| **Per-type pipeline locks** | Single `m_pipelineMutex` split into compute + graphics + pipeline mutexes |
| **D3D9 frame pacer wiring** | FramePacer wired into D3D9 swapchain |
| **Pipecompiler bugfixes** | Removed spurious `static` on `computeFallbackKey()` using `this` |
| **Audit fixes (round 1-3)** | 55 bugfixes applied: UAF in pipecompiler/framepacer, data races, queue starvation, thundering herd, OOB reads, chunk pool leak, callback reentrancy, lazy frontbuffer blit, monitor deadlock, round 3 fixes (H-16, M-11, M-12, M-26, M-27, M-29, M-35), and 36+ more |

## Changes from Sarek

Sarek contributed dyasync infrastructure and Gen9 device detection. dxvk-630 adapts and simplifies for UHD 630 target only:

| Sarek feature | dxvk-630 status |
|---------------|-----------------|
| Async pipeline compilation (dyasync) | Kept, wired into auto-detected Gen9 profile |
| Gen9 low-power device profile | Kept, simplified — no multi-GPU branching |
| Frame pacer (Phase 6) | Ported with three latency modes |
| Multi-vendor GPU profiles (NVIDIA/AMD) | Not ported — Intel UHD 630 only |

## Building

```
git clone https://github.com/EzeriumChristinal/dxvk-630.git
cd dxvk-630

meson setup --cross-file build-win64.txt --buildtype debug build-debug
ninja -C build-debug

meson setup --cross-file build-win64.txt --buildtype release build-release
ninja -C build-release
```

Requires llvm-mingw (ucrt), glslangValidator, meson 1.11+, and ninja. See [Build instructions](https://github.com/doitsujin/dxvk#build-instructions) from upstream DXVK for details.

**Windows toolchain setup:**
- llvm-mingw-20260616-ucrt-x86_64 (or newer), glslang, meson, ninja
- Add to PATH or use absolute paths in cross-file
- Run meson with `--cross-file build-win64.txt` (supplied)

## Configuration

Shipped `dxvk.conf` contains all performance tweaks. Place next to `dxvk.dll` or set `DXVK_CONFIG_FILE`. Per-app overrides via `dxvk.conf` in game directory.

## Credits

- [DXVK](https://github.com/doitsujin/dxvk) — upstream project by Philip Rebohr
- [Sarek](https://github.com/HansKristian-Work/dxvk) — dyasync and Gen9 profile by Hans-Kristian Arntzen
