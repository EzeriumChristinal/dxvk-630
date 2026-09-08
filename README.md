# dxvk-630

DXVK fork tuned for Intel UHD 630 (Gen9.5) iGPUs on low-power UMA systems with
constrained memory bandwidth.

Built on DXVK v3.0.2 with async pipeline compilation from the Sarek dyasync
branch and a Gen9 low-power profile.

## Changes from upstream DXVK

| Change | Description |
|--------|-------------|
| Gen9 low-power profile | Auto-detects UHD 630-class hardware, caps memory budget at half the largest heap, disables raw SSBO, enables async compilation. `dxvk.conf` values override the built-in defaults |
| Dyasync pipeline compilation | Background threads compile pipelines off the hot path, hiding compile stutter on slow iGPUs. `dxvk.enableDyasync`, `dxvk.numDyasyncThreads` |
| Frame pacer | Three modes: max-frame-latency, low-latency (predictive wake), min-latency. `dxvk.framePace` or `DXVK_FRAME_PACE`, `dxvk.lowLatencyOffset` fine-tuning |
| Narrowed pipeline barriers | Stage-specific masks replace `ALL_COMMANDS_BIT` on transfer-only paths; relaxed barriers available |
| Pipeline cache persistence | Cache saved to disk at exit, reused across runs |
| ALLOW_TEARING / forceHdr | Immediate present mode for the DXGI flip model; forced HDR10 option |
| Link-time optimization | Release builds use LTO |
| D3D9 frame pacer wiring | FramePacer wired into the D3D9 swapchain alongside D3D11 |

## Changes from Sarek

Sarek contributed the dyasync infrastructure and Gen9 detection. This fork
targets UHD 630 only: no multi-vendor GPU profiles, simplified profile
resolution, and a reworked frame pacer with three latency modes.

## Building

Requires llvm-mingw (ucrt), glslangValidator, meson 1.11+, ninja on PATH:

    git clone https://github.com/EzeriumChristinal/dxvk-630.git
    cd dxvk-630
    meson setup --cross-file build-win64.txt --buildtype release build-release
    ninja -C build-release

Output: 5 DLLs under `build-release/src/{d3d8,d3d9,d3d10,d3d11,dxgi}/`.
Cross-compiles to `x86_64-w64-mingw32` from Linux or Windows.

## Configuration

Shipped `dxvk.conf` carries the tuned defaults. Place it next to `dxvk.dll`, point
`DXVK_CONFIG_FILE` at it, or drop a per-game `dxvk.conf` in the game directory. Fork-specific
keys: `dxvk.enableDyasync` (auto/on/off), `dxvk.numDyasyncThreads` (0 = auto),
`dxvk.framePace` (empty / low-latency / min-latency), `dxvk.lowLatencyOffset` (microseconds,
-10000..10000), `dxvk.maxMemoryBudget` (bytes; Gen9 auto = largest heap / 2).

## Credits

- [DXVK](https://github.com/doitsujin/dxvk) - upstream project by Philip Rebohle
- [DXVK-Sarek](https://github.com/pythonlover02/DXVK-Sarek) - dyasync and frame-pacer reference