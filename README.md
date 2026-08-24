# dxvk-630

DXVK fork tuned for **Intel UHD 630 (Gen9.5)** iGPUs on low-power UMA systems with
constrained memory bandwidth.

Built on DXVK v3.0.2 with picks from the Sarek dyasync branch for async pipeline
compilation and a Gen9 low-power profile. Every change is audited: see
[DXVK-AUDIT.md](DXVK-AUDIT.md) - 16 rounds, every finding tracked, 0 critical or high
bugs open.

## Changes from upstream DXVK

| Change | Description |
|--------|-------------|
| Gen9 low-power profile | Auto-detects UHD 630-class hardware, caps memory budget at half the largest heap, disables raw SSBO, enables async compilation. User `dxvk.conf` overrides built-in defaults |
| Dyasync pipeline compilation | Background threads compile pipelines off the hot path - no stutter on slow iGPUs. `dxvk.enableDyasync`, `dxvk.numDyasyncThreads` |
| Frame pacer | Three modes: max-frame-latency (default), low-latency (predictive wake), min-latency. `dxvk.framePace` or `DXVK_FRAME_PACE` env, `dxvk.lowLatencyOffset` fine-tuning |
| Narrowed pipeline barriers | `ALL_COMMANDS_BIT` replaced with stage-specific masks where proven safe (SDMA transfers); relaxed barriers available |
| Pipeline cache persistence | Cache saved to disk at exit, reused across runs, teardown under proper host lock |
| ALLOW_TEARING / forceHdr | Immediate present mode for DXGI flip model; forced HDR10 option |
| Gen9 device IDs | Broxton/APL and 0x22xx Gen9 LP IDs added to detection |
| IsCurrent adapter tracking | Sorted physical-device handle cache detects hotplug without per-call property queries |
| Link-time optimization | Release builds use LTO |
| Per-type pipeline locks | Compute / graphics pipeline mutex split |
| D3D9 frame pacer wiring | FramePacer wired into the D3D9 swapchain alongside D3D11 |
| Audit fixes | Rounds 5-16: UAFs, deadlocks, data races, barrier correctness, overflow guards, memory-policy fixes. Full per-finding status in DXVK-AUDIT.md |

## Changes from Sarek

Sarek contributed the dyasync infrastructure and Gen9 detection. This fork adapts both
for the UHD 630 target only: no multi-vendor GPU profiles, simplified profile resolution,
frame pacer ported with three latency modes instead of Sarek's phase system.

## Building

Toolchain paths for both hosts are in [../AGENTS.md](../AGENTS.md). Short form:

    git clone https://github.com/EzeriumChristinal/dxvk-630.git
    cd dxvk-630
    meson setup --cross-file build-win64.txt --buildtype release build-release
    ninja -C build-release

Output: 5 DLLs under `build-release/src/{d3d8,d3d9,d3d10,d3d11,dxgi}/`. Requires
llvm-mingw (ucrt), glslangValidator, meson 1.11+, ninja. Cross-compiles to
`x86_64-w64-mingw32` from Linux or Windows.

## Configuration

Shipped `dxvk.conf` carries the tuned defaults. Place it next to `dxvk.dll`, point
`DXVK_CONFIG_FILE` at it, or drop a per-game `dxvk.conf` in the game directory. Fork-specific
keys: `dxvk.enableDyasync` (auto/on/off), `dxvk.numDyasyncThreads` (0 = auto),
`dxvk.framePace` (empty / low-latency / min-latency), `dxvk.lowLatencyOffset` (microseconds,
-10000..10000), `dxvk.maxMemoryBudget` (bytes; Gen9 auto = largest heap / 2).

## Credits

- [DXVK](https://github.com/doitsujin/dxvk) - upstream project by Philip Rebohle
- [DXVK-Sarek](https://github.com/pythonlover02/DXVK-Sarek) - dyasync and frame-pacer reference