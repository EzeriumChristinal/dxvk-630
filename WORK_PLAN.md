# DXVK-UHD630: Execution Tasks

Base: `C:\Users\Admin\Documents\DXVK\dxvk` (DXVK 3.1 mainline)
Sarek ref: `C:\Users\Admin\Documents\DXVK\dxvk-sarek` (DXVK-Sarek v1.12.0)
Target: Intel UHD 630 (Gen9.5, vendor=0x8086, EUs≤24, shared mem)

---

## Phase 0: Workspace Copy

- [x] **P0.0: Clone DXVK 3.1 to workspace**
  Files: `dxvk-630/*` (copy of `dxvk/`)
  Action: `Copy-Item -Recurse dxvk dxvk-630`
  Status: Done. Workspace at `dxvk-630/`.

## Toolchain (Windows cross-compile setup, llvm-mingw)
- Compiler: `tools/llvm-mingw-20260616-ucrt-x86_64/bin/` (clang 22.1.8)
- Build system: `dxvk-venv/` (Python 3.14.5 venv with meson 1.11.2)
- Ninja: `tools/ninja.exe` (1.13.2)
- GLSL→SPIR-V: `tools/glslang-bin/bin/glslangValidator.exe`
- Vulkan-Headers: `tools/Vulkan-Headers/include` (main branch, junction to `dxvk-630/include/vulkan/include`)
- SPIRV-Headers: `tools/SPIRV-Headers/include` (main branch, junction to `dxvk-630/include/spirv/include`)
- Build PATH: `tools\llvm-mingw-*ucrt-x86_64\bin;tools\glslang-bin\bin;tools;dxvk-venv\Scripts`

---

## Phase 1: Capability Gating (make Vulkan 1.3 features optional)

- [x] **T1.1: Lower DxvkVulkanApiVersion from 1.3 to 1.2**
  Files: `src/dxvk/dxvk_instance.h:12`
  Change: `VK_API_VERSION_1_3` → `VK_API_VERSION_1_2`
  Why: Lets Gen9 Windows driver (which reports 1.2 on older drivers) init without hard reject. 1.3 features still available via extension queries if driver supports them.

- [x] **T1.2: Make Vulkan 1.2 features optional**
  Files: `src/dxvk/dxvk_device_info.cpp:876-903`
  Change: Set `featureRequired=false` for these vk12 entries:
  - `bufferDeviceAddress` (line 876)
  - `descriptorIndexing` (line 877)
  - `timelineSemaphore` (line 901)
  - `vulkanMemoryModel` (line 903)
  - `scalarBlockLayout` (line 896)
  - `shaderInt8` (line 898)
  - `uniformBufferStandardLayout` (line 902)
  State transition: Required→Optional, DXVK queries but does not crash if absent.

- [x] **T1.3: Make Vulkan 1.3 features optional**
  Files: `src/dxvk/dxvk_device_info.cpp:905-914`
  Change: Set `featureRequired=false` for:
  - `inlineUniformBlock` (line 905)
  - `dynamicRendering` (line 907)
  - `maintenance4` (line 908)
  - `shaderDemoteToHelperInvocation` (line 911)
  - `shaderZeroInitializeWorkgroupMemory` (line 912)
  - `subgroupSizeControl` (line 913)
  - `synchronization2` (line 914)
  Keep `computeFullSubgroups` (line 906) as false (already non-required).
  State transition: Required→Optional. Critical: dynamicRendering and sync2 are deeply wired into mainline code — ensure code handles their absence gracefully.

- [x] **T1.4: Make KHR extensions optional**
  Files: `src/dxvk/dxvk_device_info.cpp:1026-1036`
  Change: Set `featureRequired=false` for:
  - `khrLoadStoreOpNone` (line 1028) — used for tiler opt, not critical
  - `maintenance5` (line 1031)
  - `maintenance6` (line 1032)
  Keep swapchain required (line 1058).
  State transition: Required→Optional. These are deeply embedded in pipeline creation; code must handle NULL/fallback when absent.

---

## Phase 2: Device Detection & Gen9 Profile

- [x] **T2.1: Add Gen9.5 device detection flag**
  Files: `src/dxvk/dxvk_adapter.h` (add method), `src/dxvk/dxvk_adapter.cpp` (implement)
  Add to DxvkAdapter class:
  ```cpp
  bool isGen9LowPower() const;
  ```
  Implementation: match vendorID==0x8086, deviceID in Gen9/9.5 range (SKL/KBL/CFL/WHL/AML/CML GT2), deviceType==INTEGRATED.
  Device ID ranges: 0x1902-0x193D (SKL), 0x5910-0x5927 (KBL), 0x3E90-0x3E9B (CFL), 0x9BC5 (KBL-R), etc.
  Return false by default unless match.

- [x] **T2.2: Add Gen9 config defaults**
  Files: `src/dxvk/dxvk_options.h` (add fields), `src/dxvk/dxvk_options.cpp` (add config parsing)
  Add DxvkOptions fields:
  ```cpp
  bool enableGen9Profile = false;   // auto-set by device detect
  bool enableDyasync = false;       // Sarek-port flag
  int32_t numDyasyncThreads = 0;    // 0=auto
  ```
  Parse from config:
  ```
  dxvk.enableDyasync        = config.getOption<bool>("dxvk.enableDyasync", false)
  dxvk.numDyasyncThreads   = config.getOption<int32_t>("dxvk.numDyasyncThreads", 0)
  ```

- [x] **T2.3: Wire Gen9 profile into device creation**
  Files: `src/dxvk/dxvk_device.cpp` (constructor area)
  After adapter info populated, check `m_adapter->isGen9LowPower()`.
  If true AND no user override: force `enableDyasync=true`, `numDyasyncThreads=min(4,auto)`, `useRawSsbo=false`, `maxMemoryBudget=1024`.
  Log: "DXVK: Intel Gen9 low-power profile active".

---

## Phase 3: dyasync Port (core async pipeline compiler)

- [x] **T3.1: Create DxvkPipelineCompiler (header)**
  Files: `src/dxvk/dxvk_pipecompiler.h` **NEW**
  Source: Port Sarek's `dxvk_pipecompiler.h` lines 1-83.
  Adapt: DxvkPipelinePriority enum (Background=0, Live=1). DxvkPipelineEntry struct. DxvkPipelineCompiler class with queueCompilation(), dual queues (live/background), worker pool.

- [x] **T3.2: Create DxvkPipelineCompiler (implementation)**
  Files: `src/dxvk/dxvk_pipecompiler.cpp` **NEW**
  Source: Port Sarek's `dxvk_pipecompiler.cpp` lines 1-216.
  Adapt: Use mainline 3.1's `dxvk::thread` and `ThreadPriority::Lowest`. Worker count formula: `((cores-1)*5)/7`. Queues: live + background with background every 8th pick. Queue capacity 4096. `compileAndCache()` calls `pipeline->compilePipeline(state, renderPass)` then `writePipelineStateToCache()`.

- [x] **T3.3: Add meson build for pipecompiler**
  Files: `src/meson.build`
  Add `dxvk/dxvk_pipecompiler.cpp` to source list.
  Verify `dxvk_pipecompiler.h` include path is picked up.

- [x] **T3.4: Add fallback map to DxvkGraphicsPipeline (header)**
  Files: `src/dxvk/dxvk_graphics.h` (in class DxvkGraphicsPipeline private: section)
  Port Sarek's additions (around line 234-268):
  ```cpp
  struct FallbackMapEntry { atomic<uintptr_t> key; atomic<VkPipeline> pipeline; atomic<uint8_t> used; };
  static constexpr uint32_t FallbackMapSize = 2048;
  static constexpr uint32_t FallbackMapMask = FallbackMapSize - 1;
  static constexpr uint32_t FallbackProbeMax = 16;
  array<FallbackMapEntry, FallbackMapSize> m_fallbackMap;
  atomic<VkPipeline> m_basePipeline { VK_NULL_HANDLE };
  atomic<bool> m_hasBasePipeline { false };
  static uintptr_t computeFallbackKey(const DxvkRenderPass*);
  VkPipeline findFallback(const DxvkRenderPass*);
  ```
  Also add `m_queuedSet` (4096-entry atomic hash set) to prevent double-queue.

- [x] **T3.5: Implement fallback map functions**
  Files: `src/dxvk/dxvk_graphics.cpp`
  Add implementations:
  - `computeFallbackKey()`: reinterpret_cast renderPass pointer
  - `findFallback()`: open-addressing probe on fallbackMap, return VkPipeline or VK_NULL_HANDLE
  - `createInstance()`: after creating pipeline, store in fallbackMap keyed by renderPass
  - `~DxvkGraphicsPipeline()`: clean up fallback map entries

- [x] **T3.6: Modify getPipelineHandle for dyasync fallback**
  Files: `src/dxvk/dxvk_graphics.cpp` (modify `getPipelineHandle()`)
  Replace synchronous-only path with fallback logic:
  1. Try `findInstanceLockFree(state, renderPass)` → return if hit
  2. Validate pipeline state → return VK_NULL_HANDLE if invalid
  3. Try `findFallback(renderPass)` → if hit AND m_compiler exists:
     - Hash state+renderPass, attempt dedup in m_queuedSet
     - Queue: `m_compiler->queueCompilation(this, state, renderPass, Live)`
     - Return fallback pipeline
  4. No fallback → synchronous compile: `createInstance(state, renderPass)`, cache, return

- [x] **T3.7: Integrate compiler into DxvkPipelineManager**
  Files: `src/dxvk/dxvk_pipemanager.h` (add m_compiler member), `src/dxvk/dxvk_pipemanager.cpp` (constructor)
  Add `Rc<DxvkPipelineCompiler> m_compiler` to private members.
  In constructor: conditionally create `new DxvkPipelineCompiler(device)` when `config().enableDyasync && !env::getEnvVar("DXVK_DISABLE_DYASYNC")`.
  Friend class DxvkGraphicsPipeline must access m_compiler.

---

## Phase 4: SPIR-V / Shader Adaptation

- [x] **T4.1: Conditional descriptor indexing in SPIR-V**
  Status: **No code change needed.** Existing code at `dxvk_shader_ir.cpp:2254` already gates `optimizeDescriptorIndexing` behind `SupportsResourceIndexing` SPIR-V flag. Gen9 driver sets this correctly based on `descriptorIndexing` feature availability.

- [x] **T4.2: BDA-free shader path for nullDescriptor fallback**
  Status: **No code change needed.** `useRawSsbo=Tristate::False` effectively disables all BDA-dependent paths (handled by DxvkDevice at shader compile time). When raw SSBO is False, the shader compiler falls back to descriptor-based SSBO.

- [x] **T4.3: Disable raw SSBO on Gen9 profile**
  Status: **Done in T2.3.** `m_options.useRawSsbo = Tristate::False` set when Gen9 profile activates.

---

## Phase 5: Memory Tuning

- [x] **T5.1: Memory budget override for shared-mem GPUs**
  Status: **Done in T2.3.** `m_options.maxMemoryBudget = 1024 << 20` set when Gen9 profile activates. The memory allocator already respects this field.

- [x] **T5.2: Prefer cached host memory for shared systems**
  Status: **No code change needed.** Existing `DxvkMemoryAllocator::determineMemoryTypesWithPropertyFlags()` already handles cached vs uncached properly. On UMA (Gen9), the driver exposes `HOST_CACHED|HOST_COHERENT|HOST_VISIBLE|DEVICE_LOCAL` type which is selected optimally by existing logic.

---

## Phase 6: Frame Pacer (optional port)

- [ ] **T6.1: Port framepacer header**
  Files: `src/dxvk/framepacer/dxvk_framepacer.h` **NEW**
  Source: Sarek `dxvk_framepacer.h` (port, adapt to 3.1 types)
  Three modes: MaxFrameLatency (default), LowLatency, MinLatency.
  Rolling average GPU completion prediction + sleep_until.

- [ ] **T6.2: Port framepacer implementation**
  Files: `src/dxvk/framepacer/dxvk_framepacer.cpp` **NEW**
  Source: Sarek `dxvk_framepacer.cpp` (port, adapt to 3.1 types)
  Uses DxvkFramePacer::beginFrame/endFrame with CallbackFence for GPU signal.

- [ ] **T6.3: Wire framepacer into D3D11 swapchain**
  Files: `src/d3d11/d3d11_swapchain.cpp`
  Add FramePacer member. Call beginFrame() at Present() start.
  Call endFrame() with CallbackFence GPU signal for low-latency modes.
  getEffectiveFrameLatency() → clamp to 1 for low-latency modes.

- [ ] **T6.4: Wire framepacer into D3D9 swapchain**
  Files: `src/d3d9/d3d9_swapchain.cpp`
  Same as T6.3 but for D3D9 path.

---

## Phase 7: Validation & Build

- [x] **T7.1: Build debug with meson**
  Command: `meson setup build-debug ... && meson compile -C build-debug`
  Result: 186/186 targets, all linked successfully (only D3D8 override warnings).

- [x] **T7.2: Build release for win64**
  Command: `meson setup build-release ... && meson compile -C build-release`
  Result: 323/323 targets, all linked successfully.

- [ ] **T7.3: Runtime smoke test (basic D3D11 app launch)**
  Verify: DXVK init succeeds on target hardware (UHD 630).
  Check log: No "Device does not support Vulkan 1.3" error.
  Check log: "DXVK: Intel Gen9 low-power profile active" (if Gen9 detected).

---

## Execution Order

```
Phase 0: Copy repo ─┐
Phase 1: Capability gating (T1.1→T1.4) ──┐
Phase 2: Device detection (T2.1→T2.3) ───┤
Phase 3: dyasync port (T3.1→T3.7) ───────┼── sequential
Phase 4: Shader adaptation (T4.1→T4.3) ──┤
Phase 5: Memory tuning (T5.1→T5.2) ──────┘
Phase 6: Frame pacer (T6.1→T6.4) ─── optional, parallel to 3-5
Phase 7: Build & test (T7.1→T7.3) ─── after all above
```

## Reference File Map

```
Sarek source → Target for port:
  dxvk_pipecompiler.h/.cpp   → dxvk-630/src/dxvk/          (T3.1, T3.2)
  dxvk_graphics.h (fallback)  → dxvk-630/src/dxvk/          (T3.4)
  dxvk_graphics.cpp (logic)   → dxvk-630/src/dxvk/          (T3.5, T3.6)
  dxvk_pipemanager.h/.cpp     → dxvk-630/src/dxvk/          (T3.7)
  dxvk_options.h/.cpp         → dxvk-630/src/dxvk/          (T2.2)
  framepacer/*                → dxvk-630/src/dxvk/framepacer/ (T6.1, T6.2)
```
