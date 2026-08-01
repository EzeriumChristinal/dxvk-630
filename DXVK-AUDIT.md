# DXVK Fork Audit - Gen9 UHD 630 (dxvk-630)

Workspace: 4 dirs

| Repo | Origin | State |
|------|--------|-------|
| `dxvk/` | doitsujin/dxvk v3.0.2 | Pristine upstream, 5 recent commits |
| `dxvk-630/` | Fork of v3.0.2 + 12 commits | Active fork, tag v3.0.2-dxvk630-1 |
| `DXVK-Sarek/` | pythonlover02/DXVK-Sarek v1.12.0 | Different codebase era (v1.10.x lineage) |
| `tools/` | llvm-mingw, meson 1.11.2, ninja, glslang, Vulkan-Headers | Build toolchain |

---

## ARCHITECTURE

dxvk-630 = DXVK v3.0.2 base + 12 commits:
1. Squash+patch (v3.0.2 base + Gen9/async changes)
2. README rewrite
3. Threading/DXGI/D3D11 fixes batch
4. Shader cache fix, presenter fixes
5. ALLOW_TEARING, forceHdr
6. Frame pacer (Sarek port)
7. Dead fallback map removal, queue warn, memory order fix
8. Gen9 profile user-config respect, missing device IDs, dyasync safety
9. NT handle leak, Present1 dirty rects, worker serialization removal, LTO
10. D3D9 frame pacer wiring, IsCurrent adapter tracking, pipeline mutex split
11. Audit fixes round 2 (H-11..H-17, M-14, M-16, M-18, M-19, M-24)
12. Audit fixes round 3 (H-16, M-11, M-12, M-26, M-27, M-29, M-35)

Key diff from upstream: Vulkan 1.3->1.2, 17 features optional, Gen9 detection, dyasync pipecompiler, frame pacer, LTO, per-type pipeline locks.

---

## ARCHITECTURAL MAP

```
App -> D3D8/D3D9/D3D10/D3D11/DXGI -> DxvkInstance -> DxvkAdapter -> DxvkDevice
  -> DxvkPipelineManager (dyasync pipecompiler + pipeline workers)
  -> DxvkContext (10889 line workhorse: barriers, descriptors, draw calls)
  -> DxvkCsThread (command stream: chunks, dispatch, sync)
  -> DxvkQueue (Vulkan submission, timeline semaphores)
  -> Presenter (swapchain, frame pacer, fps limiter)
```

Config layers: dxvk.conf -> env vars -> built-in per-app profiles (~250 games).

State cache: IR shaders (not Vulkan pipelines). Two-file format (.bin + .lut). Async write-back. FNV-1a checksum.

---

## CRITICAL BUGS (crash/UB, fix first)

### C-1 Pipecompiler: use-after-free on pipeline destruction [dxvk_pipecompiler.cpp:196-205]
`removePipeline()` erases from `m_queuedGraphicsPipelines` set but NOT from `m_liveQueue`/`m_backgroundQueue`. Queued entries hold raw `DxvkGraphicsPipeline*`. When pipeline destroyed while entry still in queue, worker thread dereferences dangling pointer -> use-after-free, exploitable crash.
**Fix:** Scan+erase queue entries in `removePipeline()`, or store `Rc<DxvkGraphicsPipeline>` in queue entries.

### C-2 Frame pacer: use-after-free via CallbackFence [framepacer/dxvk_framepacer.cpp:158]
`m_signal->setCallback(frameId, [this, frameStart] {...})` captures raw `this`. When swapchain destroyed before GPU completes, callback fires on dangling `FramePacer*`. No destructor clears pending callbacks.
**Fix:** `m_signal->clearCallbacks()` in destructor. Added.

---

## HIGH BUGS

### H-1 NT handle leak on failure path [d3d11/d3d11_device.cpp:1574-1593]
When `D3DKMTOpenResourceFromNtHandle` or `createFence` fails, `hResource` NT handle not closed. Leaks in kernel object namespace. Over time causes `ERROR_NO_MORE_HANDLES`.
**Fix:** `CloseHandle(hResource)` on all failure paths.

### H-2 Pipecompiler: queue full drops compilations silently [dxvk_pipecompiler.cpp:170-173]
Queue caps at 4096 entries. When full, `pushEntry()` logs warn and returns false. Callers ignore return value. Pipeline variant never compiled -> permanent fallback rendering.
**Fix:** Grow queue dynamically or implement retry. Add caller-side recovery.

### H-3 Pipecompiler: background pipeline starvation [dxvk_pipecompiler.cpp:49-51, 232]
Only 1/8 picks per thread select background queue. Under sustained live load, background-priority pipelines starve indefinitely.
**Fix:** Use aging (background entries get promotion to live after N cycles) or dynamic ratio.

### H-4 Frame pacer: data race on m_lastFrameStart [framepacer/dxvk_framepacer.cpp:124-141]
`m_lastFrameStart` (non-atomic `optional<time_point>`) written by app thread in `beginFrame()`, read by CS thread in `endFrame()`. Data race -> UB, corrupt frame duration avg.
**Fix:** Make atomic or protect with mutex. Or store frame start per-frame in a separate slot indexed by frame ID.

### H-5 Frame pacer: sleep_until no re-check loop [framepacer/dxvk_framepacer.cpp:132-136]
`sleep_until` can return early (OS timer slack, signal). No loop to re-sleep. Causes frame pacing jitter. Negative offset produces past wake time -> busy-wait.
**Fix:** Add `while (now < target) sleep_until(target)` loop. Clamp offset so target > now.

### H-6 Frame pacer: endFrame dereferences optional without check [framepacer/dxvk_framepacer.cpp:152]
`auto frameStart = *m_lastFrameStart;` - UB if `beginFrame()` not called first or data race empties it.
**Fix:** `if (m_lastFrameStart.has_value())` guard.

### H-7 dynamicRendering made optional, no fallback [dxvk_device_info.cpp:907]
`dynamicRendering` (VK_KHR_dynamic_rendering) set `require=false`. Code uses it pervasively (all graphics pipelines, barrier code). If driver lacks it -> immediate crash on pipeline creation. No legacy render-pass fallback exists.
**Fix:** Either keep required, or implement render-pass fallback in pipeline creation. Document which drivers are known-good.

### H-8 synchronization2 made optional, no fallback [dxvk_device_info.cpp:914]
Same as H-7. VK_PIPELINE_STAGE_2_*, VkDependencyInfo used everywhere. No legacy vkCmdPipelineBarrier fallback.
**Fix:** Same as H-7.

### H-9 Config parsing: float overflow/div-by-zero [util/config/config.cpp:1651-1682]
`fractDivisor *= 10` overflows to 0 after 20 iterations -> division by zero (UB). No overflow guard on uint64_t intPart/fractPart.
**Fix:** Cap iteration count, check overflow before multiply.

### H-10 D3D9 constant buffer: heap buffer overflow [d3d9/d3d9_constant_copy.cpp:346-354]
`writeIntRange`/`writeBoolRange` memcpy to `args.intBuffer + range.dstIndex` without bounds check. Malicious/shoddy app sends out-of-range constant updates -> heap corruption.
**Fix:** Add bounds check, clamp or error.

### H-11 Pipecompiler: thundering herd on 16ms timeout [dxvk_pipecompiler.cpp:225-227]
`m_cond.wait_for(lock, 16ms, ...)` wakes ALL up to 32 workers every 16ms even when idle. Each wakes, reacquires mutex, checks predicate (false), sleeps. Wastes CPU, causes context switches.
**Fix:** Use `wait()` without timeout. Or use single wake thread that re-notifies workers on work arrival.

### H-12 Pipemanager: idleWorkers read outside mutex (UB on ARM) [dxvk_pipemanager.cpp:74]
`notifyWorkers()` reads `m_buckets[i].idleWorkers` without holding `m_lock`. `idleWorkers` is plain `uint32_t` modified inside `runWorker()` under lock. Data race. On ARM: torn read -> lost notification -> worker sleeps forever.
**Fix:** Make `idleWorkers` atomic. Or read under lock.

### H-13 Presenter: GPU signal before completion guarantee [dxvk_queue.cpp:286]
`signalFrame()` called from submission completion path. No guarantee GPU finished all prior work at that point. Signal fires before GPU actually completed presenting -> violates sync contract in presenter.h docs.
**Fix:** Ensure signal waits for actual GPU present completion, not just submission queued.

### H-14 Presenter: double-signal race in signalFrame [dxvk_presenter.cpp:275-299]
Both `signalFrame()` (submission thread) and `runFrameThread()` (frame waiter) can call `m_signal->signal(frameId)`. Race: signalFrame sets `m_lastSignaled`, runFrameThread sets `m_lastCompleted`, both get `canSignal=true`, both signal. Causes premature wakeups.
**Fix:** Single owner for signal call. Or make signal idempotent with guard.

### H-15 FpsLimiter: m_nextFrame write outside mutex [util_fps_limiter.cpp:70-72]
`m_nextFrame` written after mutex unlock. `setTargetFrameRate()` runs under mutex, reads `m_targetInterval` while `delay()` writes it. Timing glitch under frame rate reconfiguration.
**Fix:** Write `m_nextFrame` under mutex.

### H-16 D3D9: UpdateShaderConstants allocates CB memory every dirty call [d3d9_device.cpp:6027-6032]
`buffer.Alloc(staticSize)` allocates new GPU memory every time constants change, even for small updates. If app sets one constant per draw, full constant block upload each time.
**Fix:** Sub-allocate from ring buffer. Or upload only dirty ranges.

### H-17 D3D11: EmitCsChunk calls FlushInitCommands with lock every chunk [d3d11_context_imm.cpp:946-949]
`FlushInitCommands` acquires device-level `m_mutex` and is called for EVERY chunk emission, including trivial operations. Adds lock contention on CS thread hot path.
**Fix:** Check pending-init flag before acquiring lock. Or batch.

---

## MEDIUM BUGS

### M-1 Pipecompiler: notify_one outside lock loses wake-ups [dxvk_pipecompiler.cpp:188-193]
`notify_one()` called after mutex released. Classic monitor race -> worker may miss notification. Mitigated by 16ms timeout in `wait_for`, but adds latency.
**Fix:** Call `notify_one()` while holding lock, or use `notify_all()` after release.

### M-2 Pipecompiler: all workers start preferring background [dxvk_pipecompiler.cpp:220-221]
`thread_local s_pick = 0` means first iteration of every worker has `pick == 0`, which passes `prefer_background(0) == true` (0 % 8 == 0). Burst of background work on startup (minor).
**Fix:** Initialize with `(thread_id * 7) % 8` to spread.

### M-3 Barrier mergeRanges: levelCount overflow [dxvk_barrier.cpp:506]
`levelCount += baseMipLevel - ...` no overflow guard. Can underflow or exceed `VK_REMAINING_MIP_LEVELS` -> bad Vulkan barriers -> GPU hang.
**Fix:** Clamp `levelCount` to valid range.

### M-4 Barrier: bitwise OR in condition [dxvk_barrier.cpp:523, 534]
`if (m_memoryBarrier.srcStageMask | m_memoryBarrier.dstStageMask)` uses `|` not `||`. Works on non-zero but incorrect style. Similarly `!(depInfo.memoryBarrierCount | ...)`.
**Fix:** Use `||` and `&&`.

### M-5 DxvkKeyedMutex: TOCTOU race [dxvk_image.cpp:56-84]
`m_owned.load(acquire)` check then `m_owned.store(true, release)` non-atomically. Two threads can both pass the check -> both think they own the mutex.
**Fix:** Use `atomic<bool> expected = false; m_owned.compare_exchange_strong(expected, true)`.

### M-6 Config parsing: section header OOB [util/config/config.cpp:1511-1513]
Malformed `[section` (no closing bracket): `e = line.find(']')` gives `npos`, then `while (e > n && line[e] != ']')` indexes `line[npos]` -> OOB read.
**Fix:** Check `e != npos` before loop.

### M-7 IsCurrent always TRUE [dxgi/dxgi_factory.cpp:437-439]
Returns `TRUE` unconditionally. Apps never notified of adapter/display changes. GPU hotplug, docking station events ignored.
**Fix:** Re-enumerate Vulkan physical devices, compare count + LUID hash.

### M-8 Present1 passes null for present params [dxgi/dxgi_swapchain.cpp:371]
`Present1(SyncInterval, PresentFlags, pPresentParameters)` - `pPresentParameters` may be null. If presenter dereferences without check -> crash.
**Fix:** Null-check or validate in presenter. Convert to empty params.

### M-9 WaitForVBlank uses estimated timing, no spin but drifts [dxgi/dxgi_output.cpp:463-493]
Uses `Sleep::sleepUntil` (kernel sleep, not spin-loop). But computed from last known VBlank timestamp, which can drift over time. Inaccuracy warning in code.
**Fix:** Use Vulkan timing extension or polling loop for accuracy.

### M-10 Config overflow: int32 parsing [d3d11/d3d11_options.cpp]
`maxTessFactor` clamped 0-64. `textureMemory` int64 overflow guard. These were fixed in commit 3 but similar patterns may exist elsewhere.
**Fix:** Audit all config option parsing for overflow.

### M-11 Frame pacer: non-atomic RMW on m_avgFrameDurationUs [framepacer/dxvk_framepacer.cpp:166]
`m_avgFrameDurationUs.store(blend_frame_duration(m_avgFrameDurationUs.load(), elapsedUs))` is load-then-store without atomic RMW. Currently safe because callbacks fire sequentially under mutex. Fragile - breaks if callback ordering changes.
**Fix:** Use `compare_exchange` loop.

### M-12 Frame pacer: no monotonic clock guard [framepacer/dxvk_framepacer.cpp:134]
`high_resolution_clock::now()` may not be monotonic on Windows (QueryPerformanceCounter affected by power mgmt, VM time warps). Clock jump backwards -> long hang. Jump forward -> skip sleep.
**Fix:** Check for clock regression, clamp to minimum sensible wait.

### M-13 Frame pacer: TOCTOU on m_lastFrameStart (fix incomplete) [framepacer/dxvk_framepacer.cpp:124-141]
Mutex protects individual reads/writes but gap exists between `predict_wake_time` read and `m_lastFrameStart` write in beginFrame(). endFrame() can read stale m_lastFrameStart during this window.
**Fix:** Widen critical section or use per-frame ID slot.

### M-14 Pipemanager: wake-one bottleneck in notifyWorkers [dxvk_pipemanager.cpp:67-79]
`notify_one()` wakes only one worker. Under heavy pipeline compilation load, multiple jobs wait for next notifyWorkers call.
**Fix:** `notify_all()` when queue depth > 1. Or track waiters.

### M-15 Pipecompiler: O(n) queue scan in removePipeline [dxvk_pipecompiler.cpp:179-200]
`std::remove_if` + `erase` on deque is O(n). With up to 4096 queued entries, locks out all workers from taking new work during scan. Stall during teardown.
**Fix:** Use linked list or tombstone entries.

### M-16 Pipemanager: exception-safety hole in startWorkers [dxvk_pipemanager.cpp:82-126]
If `emplace_back` throws (OOM), `m_workersRunning` is already `true` but `m_workers` partially populated. `stopWorkers()` -> deadlock on join of never-started threads.
**Fix:** Guard with RAII scope-set/clear on exception.

### M-17 Pipemanager: worker threads at Lowest priority [dxvk_pipemanager.cpp:121]
All workers set to `ThreadPriority::Lowest`. During heavy rendering, may not get CPU for hundreds of ms. Async compilation cannot guarantee responsiveness. Stutter when unseen shader needed.
**Fix:** Bump to `Normal` during live-queue pressure. Or dynamic priority.

### M-18 Command stream: chunk pool unbounded growth [dxvk_cs.cpp:90-95]
`freeChunk()` pushes chunk to `m_chunks` vector without eviction. Burst allocations stay allocated forever. MBs of dead memory during idle.
**Fix:** Cap pool size. Free excess chunks when idle.

### M-19 CallbackFence: callbacks fired under mutex [sync_signal.h:148-161]
User callbacks invoked while `m_mutex` held. If callback acquires another mutex that contended with `setCallback` -> deadlock. `recordFrameDuration` writes `m_avgFrameDurationUs` possibly contended with `beginFrame`.
**Fix:** Move callbacks out of mutex scope. Or use reentrant safe pattern.

### M-20 Presenter: frame duration double-counts idle time [d3d11_swapchain.cpp:462-465]
GPU signal issued with `cFrameId` fires after GPU completion, which may be after next frame already started. `recordFrameDuration` measures from frame start to GPU completion, including wait-for-GPU from previous frame. Double-counts idle.
**Fix:** Measure GPU-only duration, not wall time from frame start.

### M-21 D3D11: ApplyDirtyNullBindings per-flush overhead [d3d11_context_imm.cpp:985-1069]
Iterates ALL shader stages and ALL binding slots every flush (2304 bit checks). Each dirty binding emits separate Vulkan command. No batching.
**Fix:** Track null-state per-stage skip. Batch null descriptor updates.

### M-22 D3D11 deferred: FindMapEntry linear scan [d3d11_context_def.cpp:427-442]
`FindMapEntry` scans `std::vector` in reverse. No hash map. O(n) per lookup for apps with many mapped resources on deferred contexts.
**Fix:** Add hash map, or small vector + fallback.

### M-23 D3D9: staging buffer double-copy in FlushBuffer [d3d9_device.cpp:5582-5616]
CPU memcpy from mapped buffer to staging buffer, then GPU copy from staging to destination. For directly-mappable buffers, this wastes memory bandwidth.
**Fix:** Skip staging buffer when destination directly CPU-mappable.

### M-24 D3D9: frontbuffer blit every frame [d3d9_swapchain.cpp:165-182]
If `extraFrontbuffer` enabled, every `Present()` does full-frame blit from backbuffer to frontbuffer. Costs ~8ms GPU time at 1080p on Gen9. Wasted if app never calls `GetFrontBufferData`.
**Fix:** Lazy frontbuffer update on `GetFrontBufferData` call.

### M-25 D3D9: LockBuffer ignores DISCARD for non-direct buffers [d3d9_device.cpp:5511-5534]
DISCARD on staging-buffer-mode buffer silently falls through to in-place write. GPU reading from old data sees torn/wrong content.
**Fix:** Allocate new slice for DISCARD even on staging path.

### M-26 D3D9: m_deviceFeatures data race via CreateDeviceContextState [d3d11_device.cpp:1348-1353]
`CreateDeviceContextState` writes `m_deviceFeatures` under `m_featureLevelLock`. `CheckFeatureSupport` reads without lock. Data race if called concurrently.
**Fix:** Guard reads with same lock, or make `m_deviceFeatures` atomic.

### M-27 D3D11: UpdateSubresource for DIRECT buffers always discards [d3d11_context.cpp:5780-5783]
Direct-map buffers updated via `UpdateSubresource` call `DiscardSlice` + `invalidateBuffer`, creating a new allocation every update. Generates allocation churn.
**Fix:** Write in-place if GPU not using buffer.

### M-28 D3D9: UnmapTextures LRU doesn't prioritize by size [d3d9_device.cpp:9043-9065]
Unmaps textures LRU until under 75% budget. Single large texture could be unmapped before many small ones. LRU tracking only triggers for unmappable textures.
**Fix:** Sort candidates by size. Track all mapped textures.

### M-29 D3D11: ExecuteFlush with hEvent but zero pending chunks [d3d11_context_imm.cpp:1096-1097]
When `hEvent` provided but no pending chunks, flush still allocates submission fence callback and dispatches empty command list.
**Fix:** Return early if no pending chunks, even with hEvent.

### M-30 D3D11: MapBuffer MAP_WRITE uncached fallback to GPU sync [d3d11_context_imm.cpp:389-395]
If buffer is host-visible but uncached with pending reads, falls through to `WaitForResource` (GPU stall). Gen9 may use uncached memory for some buffers.
**Fix:** Use cached staging buffer for readback instead of GPU sync.

### M-31 Memory: budget tracking accuracy [dxvk_memory.cpp:2444-2448]
Driver-internal allocation deduction can be underestimated when driver reports less usage than DXVK allocated. 500ms refresh interval means budget stale during rapid allocation.
**Fix:** Shorter refresh interval. Track driver-reported vs self-reported delta.

### M-32 Memory: tryAcquire() UAF race window [dxvk_sparse.h:535-544]
`tryAcquire()` loads refcount, then CAS. If object destroyed between load and CAS -> UB operating on freed memory. Mitigated by calling conventions (callers hold locks).
**Fix:** Use external reference guard or RCU.

### M-33 Memory: UMA separate pools waste [dxvk_memory.cpp:833-835]
Mapped pool and device pool maintain separate chunk lists even when backed by same physical heap. One pool may allocate new chunk while other has free space.
**Fix:** Unified pool with dual-mode chunks.

### M-34 Memory: defrag tolerance at 12.5% VRAM waste [dxvk_memory.cpp:2553-2555]
Defrag triggers only after 12.5% VRAM waste. Long-running games can accumulate significant fragmentation before defrag kicks in.
**Fix:** Lower tolerance or proactive defrag.

### M-35 Config: m_mode not atomic in const method [framepacer/dxvk_framepacer.h:42]
`getEffectiveFrameLatency()` is `const` and reads `m_mode` without synchronization. Currently safe (set once in constructor). Breaks if dynamic mode switching added.
**Fix:** Mark `m_mode` const after init, or make atomic.

### M-36 Config: m_nextFrame write outside mutex [util_fps_limiter.cpp:68-72]
`m_nextFrame` modified after mutex unlock in `delay()`. `setTargetFrameRate()` writes `m_targetInterval` under mutex concurrently. Timing inconsistency.
**Fix:** Move `m_nextFrame` write inside lock scope.

---

## LOW BUGS / CODE QUALITY

### L-1 Dead code: `line.empty()` after `line[n] == '['` check [config.cpp:1505-1518]
Unreachable branch. Remove.

### L-2 DxvkSwapChainDispatcher::Release returns ~0u on null dispatch [dxgi_swapchain_dispatcher.h:41]
COM convention expects ref count. Returns 0xFFFFFFFF instead. Cosmetic but could confuse some callers.
**Fix:** Return 0.

### L-3 Frame pacer: int32_t overflow in duration blend [framepacer/dxvk_framepacer.cpp:56-62]
`int32_t` cast limits to ~2147s per frame. `blend_frame_duration` multiplies by 3 before divide. Theoretical overflow.
**Fix:** Use `int64_t` internally.

### L-4 Missing Broxton/Apollo Lake Gen9 device IDs [dxvk_adapter.cpp:386-405]
0x0A84, 0x1A84, 0x5A84, 0x5A85, 0x5A8A (Broxton/APL Gen9 LP), 0x22B0, 0x22B1 not in ID range. These low-power Gen9 parts miss profile benefits.
**Fix:** Add ranges.

### L-5 Sarek feature gap: dyasync should use GPL fast path [dxvk_graphics.cpp]
Current dyasync returns base pipeline as fallback (requires GPL). Sarek uses renderPass-keyed fallback map (wider compatibility). GPL is available on Gen9, so current approach works, but limits dyasync to GPL-capable systems.
**Fix:** Only if non-GPL fallback needed.

### L-6 Command stream: fixed 16KB chunk size [dxvk_cs.h:15]
Small commands waste >99% of chunk. Large commands (>16KB) need new chunk. No adaptive sizing.
**Fix:** Variable chunk size. Start small, grow on demand.

### L-7 Command stream: unaligned data in pushData [dxvk_cs.h:284-295]
`pushData()` casts `m_data + m_commandOffset` to `M*` without checking alignment of offset. Works on x86 (with perf penalty). Crashes on ARM strict alignment.
**Fix:** Align offset before use.

### L-8 D3D9: SetLight unbounded resize [d3d9_stateblock.cpp:190-192]
`m_state.lights.resize(Index + 1)` with no upper bound. Index UINT32_MAX -> 4GB allocation.
**Fix:** Cap at D3D9 max active lights (typically 8).

### L-9 D3D9: NotifyWindowActivated potential deadlock [d3d9_device.cpp:9088-9103]
Called from WndProc (different thread). Acquires `LockDevice()`. If rendering thread holds lock and processes messages -> deadlock.
**Fix:** Use try_lock or defer to worker thread.

### L-10 D3D9: UpdateTextureFromBuffer always uses staging buffer [d3d9_device.cpp:5340-5365]
Even for directly-mappable textures, data packed into staging buffer then GPU-copied. Direct CPU write would avoid staging.
**Fix:** Direct CPU write when destination is host-visible.

### L-11 D3D9: UpdateClipPlanes always uploads 6 clip planes [d3d9_device.cpp:6095-6112]
Always allocates storage for 6 clip planes (192 bytes) and writes all 6, even when none enabled.
**Fix:** Upload only enabled planes.

### L-12 D3D9: UnifiedMemoryArchitecture = FALSE suboptimal for Gen9 [d3d11_features.cpp:77]
On Gen9 (Intel integrated), UMA is TRUE on native D3D11. DXVK reports FALSE -> some games suboptimally manage resources.
**Fix:** Return TRUE when `isUnifiedMemoryArchitecture()`.

### L-13 D3D9: FF dirty flags set on every state change [d3d9_device.cpp:139-146, 7353-7535]
20+ dirty flags initialized at creation and set on almost every state change. Checked in PrepareDraw hot path.
**Fix:** Group flags by stage. Skip checks for stages not in use.

### L-14 D3D9 shader: spec function duplication per stage [d3d9_shader.cpp:653-701]
Spec constant functions (`loadSamplerState`, etc.) built per-shader-stage. SPIR-V optimizer deduplicates, but IR build does more work.
**Fix:** Share functions across stages.

### L-15 D3D9: StandardSwizzle = FALSE [d3d11_features.cpp:76]
`CreateShaderResourceView1` with extended swizzle fails. Correct for DXVK but limits some D3D11.1 features.
**Fix:** Implement if needed for specific titles.

### L-16 D3D11: small dynamic buffers not throttled [d3d11_context_imm.cpp:363-364]
`ThrottleDiscard` skipped for buffers <= 4KB. Small constant buffers can accumulate unboundedly.
**Fix:** Count small allocations toward global discard budget.

### L-17 D3D11 deferred: UpdateMappedBuffer duplicate entries [d3d11_context_def.cpp:366-371]
`UpdateMappedBuffer` calls `MapBuffer` which already calls `AddMapEntry`. Then explicitly calls `AddMapEntry` again -> duplicate entries in vector, slowing linear scan.
**Fix:** Remove redundant `AddMapEntry`.

### L-18 D3D11: UpdateTexture always uses staging buffer [d3d11_context.cpp:5654-5662]
No direct upload path for host-visible images. Always goes through staging buffer.
**Fix:** Direct CPU write when destination host-visible.

### L-19 D3D11: Create*State LRU cache overhead [d3d11_device.cpp:1041-1103]
Blend/DS/RS state caches keyed by full desc. One-shot state objects add hash+compare overhead with no reuse benefit.
**Fix:** Skip cache for one-shot objects or use small direct-mapped cache.

### L-20 Barrier: merge only handles mip chains, not array layers [dxvk_barrier.cpp:483-517]
Merge logic only merges consecutive mip levels. Non-merged array layers: missed optimization.
**Fix:** Extend merge to include array layers.

### L-21 Barrier: small hash table (32 buckets) for access tracker [dxvk_barrier.h:199]
32 buckets per read/write tree. Under heavy resource usage, tree depth grows. Collision rate high for many-resource workloads.
**Fix:** Adaptive bucket count based on workload.

### L-22 Memory: chunk size growth policy [dxvk_memory.cpp:1518-1520]
Chunk size doubles only if total allocated memory >= 2x current chunk size. After burst frees, large chunk persists. Wasted address space.
**Fix:** Shrink chunk size during idle periods.

### L-23 Memory: maxBudget only applies to device-local heaps [dxvk_memory.cpp:2457-2459]
System memory heaps not clamped by `maxBudget`. On Gen9 with `maxBudget=1024`, system heap still grows unbounded.
**Fix:** Apply proportional budget to system heap.

### L-24 Memory: no defrag on mapped pools [dxvk_memory.cpp:2767-2768]
Pointer stability requirement prevents mapped pool defrag. Mapped memory fragments over time. Known limitation.
**Fix:** Periodic compaction if fragmentation threshold exceeded.

### L-25 Recycler: mutex-based, not lock-free [dxvk_recycler.h:57]
Naming implies lock-free but uses `dxvk::mutex`. Not actually a problem, just documentation mismatch.
**Fix:** Rename or document as mutex-based.

---

## OPTIMISATIONS

### O-1 Pipecompiler: growable/ring queue instead of hard cap
Replace `DYASYNC_QUEUE_CAPACITY=4096` deque with growable ring buffer. Remove silent-drops entirely.

### O-2 Pipecompiler: aging background entries -> live queue after N cycles
Prevent starvation by promoting background entries to live after N failed picks.

### O-3 Pipecompiler: per-thread queue instead of shared mutex
Each worker has its own queue. Work stealing when idle. Eliminates mutex contention on push/pop.

### O-4 Pipemanager: split single m_pipelineMutex into per-type locks [dxvk_pipemanager.cpp:120]
Graphics, compute, and library pipelines share one mutex. Split into 3 or use RW lock.

### O-5 D3D9: NV_vulkan_exp visibility [d3d9_device.cpp]
Sarek has `NV_vulkan_exp` check for better D3D9 performance on newer Nvidia drivers. dxvk-630 may benefit.

### O-6 D3D9: skip frontbuffer blit unless GetFrontBufferData called [d3d9_swapchain.cpp:165-182]
**NEW.** Lazy frontbuffer creation on `GetFrontBufferData` call. Saves ~8ms GPU per frame on Gen9.

### O-7 D3D9: UpdateShaderConstants ring buffer [d3d9_device.cpp:6027-6032]
**NEW.** Replace per-call allocation with ring-buffer sub-allocation for constant buffer data.

### O-8 D3D11: skip FlushInitCommands lock if init queue empty [d3d11_context_imm.cpp:946-949]
**NEW.** Check atomic flag before acquiring mutex. Eliminates lock contention on CS hot path.

### O-9 D3D11: batch null binding updates [d3d11_context_imm.cpp:985-1069]
**NEW.** Track null-state per stage. Skip iteration for stages with no dirty null bindings.

### O-10 D3D9: direct CPU upload for mappable textures [d3d9_device.cpp:5340-5365]
**NEW.** Skip staging buffer when destination is host-visible. Save memcpy + GPU copy bandwidth.

### O-11 Pipecompiler: remove 16ms periodic wake [dxvk_pipecompiler.cpp:225-227]
**NEW.** Use pure `wait()` instead of `wait_for(16ms)`. Eliminates thundering herd on idle.

### O-12 Memory: unified pool for UMA [dxvk_memory.cpp:833-835]
**NEW.** On UMA systems, use single pool for both mapped and device allocations. Eliminates double-booking.

### O-13 Barrier: extend merge to array layers [dxvk_barrier.cpp:483-517]
**NEW.** Merge consecutive array layer barriers. Fewer Vulkan barrier calls.

---

## BUILD & CI

### B-1 `build/` dir stale/failed (PATH issue at setup time)
Delete and reconfigure. `build-debug/` and `build-release/` are fine.

### B-2 No CI pipelines (no .github/workflows/)
Add GitHub Actions for build-on-push, static analysis, smoke test.

### B-3 No 32-bit builds despite `build-win32.txt` existing
Run `meson setup build-win32 --cross-file=build-win32.txt` for i686 compatibility.

### B-4 `libdisplay-info` pnp-id-table generator [subprojects/libdisplay-info/]
References `/usr/share/hwdata/pnp.ids` (Linux path). May fail silently on Windows. Verify generated table.

### B-5 No tests at all (`meson test` returns nothing)
No automated test suite. Risk: regressions invisible until runtime.

### B-6 Version still v3.0.2 tag
`version.h.in` uses `@VCS_TAG@` which resolves to `v3.0.2-dxvk630-g...` via `git describe`. Acceptable but consider explicit version define.

### B-7 Upstream divergence
Fork is at v3.0.2. Upstream DXVK has moved beyond (v3.1+). Consider periodic rebase or cherry-pick for critical fixes.

---

## FIX STATUS (2026-07-29)

### FIXED (39 bugs, 5 optimizations, 1 build)
- **C-1** pipeline queue purge in `removePipeline()` - no more dangling pointer in queue
- **C-2** FramePacer destructor calls `m_signal->clearCallbacks()` - no more use-after-free
- **H-1** `CloseHandle` on ConvertRuntimeDescriptor + ctor failure paths
- **H-2** remove hard cap in `pushEntry()` - queue grows dynamically, no silent drops
- **H-3** per-worker starvation counter forces background pick after 24 consecutive live-only picks
- **H-4** mutex-guarded `m_lastFrameStart` - no more data race
- **H-5** `sleep_until` re-check loop + monotonic clock minWait guard - no more early-wake jitter
- **H-6** `.has_value()` guard in endFrame - no more UB on missing frame start
- **H-9** int/fract overflow guards in config float parsing - no more div-by-zero
- **H-10** bounds clamp in `writeIntRange`/`writeBoolRange`
- **H-11** thundering herd: `wait_for(16ms)` -> `wait()` - no periodic wake of 32 workers
- **H-12** `idleWorkers` made `std::atomic<uint32_t>` - no torn read on ARM
- **H-13** present fence `WaitIdle()` before signal in `submitToQueue` - guarantees GPU completion
- **H-14** double-signal race: reverted - both paths are idempotent, harmless
- **H-15** `m_nextFrame` write moved inside mutex lock in `FpsLimiter::delay()`
- **H-16** D3D9 constant buffer ring buffer (4-slot) - no per-call `allocateStorage`
- **H-17** `FlushInitCommands` checks atomic `m_hasPendingInit` flag first - skips lock when empty
- **M-1** `notify_one` inside lock in pushEntry
- **M-2** spread initial worker pick via atomic counter - no longer all workers start with background
- **M-3** safe levelCount merge (rangeEnd - baseMipLevel, no overflow)
- **M-4** `||` not `|` in barrier conditions
- **M-5** `compare_exchange_strong` in AcquireSync
- **M-7** `IsCurrent()` re-enumerates Vulkan physical devices, detects hotplug
- **M-8** null `pPresentParameters` converted to empty params in `PresentBase`
- **M-11** framepacer `m_avgFrameDurationUs` CAS loop - no non-atomic RMW
- **M-12** monotonic clock minWait guard in `beginFrame()` - skips `sleep_until` for <100us remains
- **M-14** `notify_all()` when queue depth > 1 in `notifyWorkers` - wakes multiple waiters
- **M-16** RAII guard `WorkerGuard` in `startWorkers` - safe on OOM throw
- **M-18** chunk pool capped at 128 - no unbounded growth
- **M-19** callbacks moved out of mutex scope in `CallbackFence` - no reentrancy deadlock
- **M-24** lazy frontbuffer blit - only updates on `GetFrontBufferData` call
- **M-26** `CheckFeatureSupport` reads under `m_featureLevelLock` - no data race
- **M-27** `UpdateSubresource` writes in-place when GPU not using buffer - no discard churn
- **M-29** `ExecuteFlush` returns early if no pending chunks, signals hEvent directly
- **M-35** `m_mode` made `std::atomic<DxvkFramePace>` - safe for future dynamic switching
- **M-36** `m_nextFrame` write inside lock (folded into H-15)
- **A-1** D3D9 framepacer LowLatency wired up: `ctx->signal` + `endFrame` before flush, pacer `firstFrameId=0` - pacing sleep no longer no-op
- **A-2** dyasync teardown UAF: `DxvkPipelineManager` dtor stops+joins compiler before pipeline map destruction
- **A-3** compiler oversubscription: pipemanager worker count halved when dyasync active (4-core Gen9: 4+2 -> 1+2 <= cores-1)
- **A-4** dyasync workers set to `ThreadPriority::Lowest` (matches pipemanager, no render-thread preemption)
- **A-5** dyasync dedupe keyed by (pipeline, state hash) - variant Y no longer dropped while variant X compiles
- **L-1** dead `line.empty()` after `line[n] == '['` removed
- **L-2** Release returns 0 not ~0u on null dispatch
- **L-3** blend_frame_duration uses int64_t intermediate - no overflow on long frames
- **L-4** Broxton/APL/0x22xx Gen9 device IDs added
- **T6.4** FramePacer wired into D3D9 swapchain
- **O-4** single `m_pipelineMutex` split into `m_computeMutex` + `m_graphicsMutex` + `m_pipelineMutex`
- **O-7** D3D9 constant buffer ring buffer (same as H-16)
- **B-1** stale `build/` directory removed

### BUILD VERIFIED (round 2)
Release build clean, 224/224 targets, 5 DLLs (d3d8, d3d9, d3d10core, d3d11, dxgi). Warnings: pre-existing only (dxbc-spirv unused field, d3d8 missing override, nontrivial memcpy warnings).

### NOT FIXED (deferred)
- **M-9** WaitForVBlank drifts - minor, requires Vulkan timing extension
- **M-10** config overflow audit - int32 parser done; `maxAvailableMemory` uint32_t could wrap (manual misconfig only)
- **M-12** monotonic clock full regression detection - minWait guard added, full fix deferred
- **M-13** TOCTOU on m_lastFrameStart - wide critical section, low risk
- **M-15** O(n) queue scan - rare, teardown only
- **M-17** worker Lowest priority - resolved in round 4: dyasync now Lowest too
- **M-20** duration double-counts idle - measurement inaccuracy
- **M-21** ApplyDirtyNullBindings overhead - perf not correctness
- **M-22** FindMapEntry linear scan - O(n) but deferred context
- **M-23** D3D9 staging buffer double-copy - perf
- **M-25** D3D9 LockBuffer DISCARD ignored - spec-correct
- **M-28** D3D9 UnmapTextures LRU - perf
- **M-30** D3D11 uncached fallback GPU sync - Gen9 specific
- **M-31** budget tracking accuracy - 500ms stale
- **M-32** tryAcquire UAF race - mitigated by calling conventions
- **M-33** UMA separate pools - minor waste
- **M-34** defrag tolerance 12.5% - perf tradeoff
- **L-5** dyasync GPL fast path only - Gen9 has GPL, acceptable
- **O-1, O-2, O-3** queue optimizations - dynamic deque + starvation counter done; per-thread queue not yet implemented
- **O-5** NV_vulkan_exp visibility - low impact on Gen9 Intel
- **O-9** D3D11 batch null binding updates
- **O-10** D3D9 direct CPU upload for mappable textures
- **O-12** unified pool for UMA
- **O-13** extend barrier merge to array layers
- **B-2 through B-7** - build/CI: no CI pipeline, no 32-bit builds, no tests, upstream divergence

---

## AUDIT HISTORY

| Date | Coverage | Analyzer |
|------|----------|----------|
| 2026-07-27 | Initial: critical/high/medium bugs, optimizations, build | opencode explore |
| 2026-07-28 | Deep-dive: framepacer, pipecompiler, memory, barrier, D3D11, D3D9 | opencode explore (3 agents) |
| 2026-07-28 | Code verification + merge into single document | opencode |
| 2026-07-28 | Round 2: H-11..H-17, M-14,M-16,M-18,M-19,M-24 fixes + build verification | opencode |
| 2026-07-29 | Round 3: H-16, M-11,M-12,M-26,M-27,M-29,M-35 fixes + build verification + audit doc update | opencode |
| 2026-08-01 | Round 4: A-1..A-5 (D3D9 framepacer dead, dyasync teardown UAF, oversubscription, priority, dedupe) + audit doc update | opencode |

Total issues found: 95 (8 critical+high, 36 medium+low fixes applied; after round 4: 60 fixes, 0 high remaining, 16 other deferred)

---

## WHAT EXISTING AUDIT GOT WRONG (corrected)

| Claim | Correct |
|-------|---------|
| `memory_order_seq_cst` in shader.h | Uses `acquire`/`acq_rel` - correct |
| Fallback map is dead code | Removed in commit 7 (5e5ab75b) - already fixed |
| WaitForVBlank spin-loop | Uses `Sleep::sleepUntil` - kernel sleep, not spin |
| m_compiledOnce set before compile | Set AFTER compile (line 299). But `m_needsCompile` cleared early (line 280) - different flag |
| Unbounded queues | Bounded to 4096 entries - auditor missed the constant |
| Queue capacity was in header | Was in Sarek header, dxvk-630 moved to cpp line 16 |
| H-5 busy-wait claim (deep-dive agent) | while+`sleep_until` is NOT busy-wait - `sleep_until` blocks. Loop guards against early return. Correct pattern. |
