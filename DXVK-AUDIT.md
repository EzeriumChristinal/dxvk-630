# DXVK-630 Audit - Gen9 UHD 630

Workspace: 4 dirs

| Repo | Origin | State |
|------|--------|-------|
| `dxvk/` | doitsujin/dxvk v3.0.2 | Pristine upstream |
| `dxvk-630/` | Fork v3.0.2 + 10 commits | Active fork, tag v3.0.2-dxvk630-1 |
| `DXVK-Sarek/` | pythonlover02/DXVK-Sarek v1.12.0 | Older era (v1.10.x lineage) |
| `tools/` | llvm-mingw, meson 1.11.2, ninja, glslang, Vulkan-Headers | Build toolchain |

Latest: FIX STATUS sections below (round 5-14). Round 14 = post-migration full diff-surface rescan (all 59 fork-different files), Map-path sync-gate completion. `DXVK-AUDIT-2026-08-01.md` merged here + deleted (was the historic round-4 do-file).

## ARCHITECTURE

dxvk-630 = DXVK v3.0.2 + 10 commits + audit fixes:
1. Squash+patch (v3.0.2 base + Gen9/async)
2. README rewrite
3. Threading/DXGI/D3D11 fixes
4. Shader cache, presenter fixes
5. ALLOW_TEARING, forceHdr
6. Frame pacer (Sarek port)
7. Fallback map removal, queue warn, memory order
8. Gen9 profile user-config, device IDs, dyasync safety
9. NT handle leak, Present1 dirty rects, worker serialization, LTO
10. D3D9 framepacer wiring, IsCurrent, pipeline mutex split

Diff vs upstream: Vulkan 1.3->1.2, 17 features optional, Gen9 detect, dyasync pipecompiler, frame pacer, LTO, per-type pipeline locks.

## ARCHITECTURAL MAP

```
App -> D3D8/D3D9/D3D10/D3D11/DXGI -> DxvkInstance -> DxvkAdapter -> DxvkDevice
  -> DxvkPipelineManager (dyasync pipecompiler + pipeline workers)
  -> DxvkContext (barriers, descriptors, draw calls)
  -> DxvkCsThread (command stream: chunks, dispatch, sync)
  -> DxvkQueue (Vulkan submission, timeline semaphores)
  -> Presenter (swapchain, frame pacer, fps limiter)
```

Config layers: dxvk.conf -> env vars -> per-app profiles (~250 games).
State cache: IR shaders. Two-file (.bin + .lut). Async write-back. FNV-1a checksum.

## CRITICAL BUGS (crash/UB - fix first)

### C-1 Pipecompiler UAF on pipeline destroy [dxvk_pipecompiler.cpp:196-205]
`removePipeline()` erases `m_queuedGraphicsPipelines` but NOT `m_liveQueue`/`m_backgroundQueue`. Queue holds raw `DxvkGraphicsPipeline*`; destroyed pipeline still queued -> worker deref dangling ptr. **Fixed:** queue purge in removePipeline.

### C-2 Frame pacer UAF via CallbackFence [framepacer/dxvk_framepacer.cpp:158]
`m_signal->setCallback` captures raw `this`; swapchain destroyed before GPU complete -> callback on dangling `FramePacer*`. **Fixed:** `clearCallbacks()` in dtor.

### N-1 Present-fence UAF on swapchain recreate [dxvk_presenter.cpp:228-229, 298-303, 1241-1267]
`presentImage` stores `m_lastPresentFence` (owned by old swapchain's semaphore set); `destroySwapchain` destroys all fences but never resets it; queue thread `signalFrame` `vkWaitForFences` on stale/destroyed handle after recreate (see dxvk_queue.cpp:286). Pre-existing upstream bug, present for both trees. **Fixed round 6:** reset `m_lastPresentFence = VK_NULL_HANDLE; m_lastPresentFenceFrameId = 0` in destroySwapchain.

## HIGH BUGS

### H-1 NT handle leak on failure [d3d11_device.cpp:1574-1593]
Failed `D3DKMTOpenResourceFromNtHandle`/`createFence` -> `hResource` not closed -> kernel object leak. **Fixed:** `CloseHandle` all failure paths.

### H-2 Pipecompiler queue cap drops compilations [dxvk_pipecompiler.cpp:170-173]
4096 cap -> full queue drops silently -> permanent fallback. **Fixed:** queue grows, no cap.

### H-3 Background pipeline starvation [dxvk_pipecompiler.cpp:49-51, 232]
1/8 picks background -> bg starves under live load. **Fixed:** per-worker counter forces bg pick after 24.

### H-4 Frame pacer race on m_lastFrameStart [framepacer/dxvk_framepacer.cpp:124-141]
Written app thread, read CS thread -> UB. **Fixed:** mutex-guarded.

### H-5 sleep_until no re-check [framepacer/dxvk_framepacer.cpp:132-136]
Early return -> jitter; negative offset -> busy-wait. **Fixed:** re-check loop + minWait guard.

### H-6 endFrame deref optional unguarded [framepacer/dxvk_framepacer.cpp:152]
UB if beginFrame not called. **Fixed:** `.has_value()` guard.

### H-7 dynamicRendering optional, no fallback [dxvk_device_info.cpp:907]
`require=false` but used everywhere; driver lacks it -> crash on pipeline create. **RESOLVED:** reverted to required (clean rejection).

### H-8 synchronization2 optional, no fallback [dxvk_device_info.cpp:914]
Same as H-7. **RESOLVED:** reverted to required.

### H-9 Config float parse overflow/div-by-zero [config.cpp:1651-1682]
`fractDivisor *= 10` overflow -> div-by-zero. **Fixed:** int/fract guards.

### H-10 D3D9 CB heap overflow [d3d9_constant_copy.cpp:346-354]
`writeIntRange`/`writeBoolRange` no bounds check -> heap corruption. **Fixed:** clamp.

### H-11 Pipecompiler thundering herd [dxvk_pipecompiler.cpp:225-227]
`wait_for(16ms)` wakes all workers every 16ms. **Fixed:** `wait()` no timeout.

### H-12 idleWorkers read outside mutex [dxvk_pipemanager.cpp:74]
Torn read on ARM -> lost wake -> worker sleeps forever. **Fixed:** atomic.

### H-13 Presenter signal before GPU completion [dxvk_queue.cpp:286]
Signal may fire before present done. **Fixed:** `WaitIdle()` before signal.

### H-14 Double-signal race [dxvk_presenter.cpp:275-299]
signalFrame + runFrameThread both signal. **Fixed:** reverted - idempotent, harmless.

### H-15 FpsLimiter m_nextFrame outside mutex [util_fps_limiter.cpp:70-72]
Timing glitch on reconfigure. **Fixed:** write under lock.

### H-16 D3D9 CB alloc every dirty call [d3d9_device.cpp:6027-6032]
Per-dirty-call GPU alloc. **Fixed round 3 (ring buffer) then REVERTED round 10:** the 4-slot ring reused live slices with no completion wait -> wrong constants on FF-heavy titles (R10-1). Back to upstream per-wrap `allocateStorage()`; per-wrap alloc, not per-dirty-call - no perf issue.

### H-17 EmitCsChunk locks every chunk [d3d11_context_imm.cpp:946-949]
`FlushInitCommands` acquires device mutex per chunk. **Fixed:** atomic flag gate.

## MEDIUM BUGS

### M-1 notify_one outside lock [dxvk_pipecompiler.cpp:188-193]
Lost wake-up race. **Fixed:** notify inside lock.

### M-2 Workers start preferring background [dxvk_pipecompiler.cpp:220-221]
pick==0 -> bg on first iteration. **Fixed:** spread init.

### M-3 levelCount overflow [dxvk_barrier.cpp:506]
Bad Vulkan barriers -> GPU hang. **Fixed:** safe range.

### M-4 Bitwise OR in condition [dxvk_barrier.cpp:523, 534]
`|` not `||`. **Fixed.**

### M-5 KeyedMutex TOCTOU [dxvk_image.cpp:56-84]
Two threads both pass check -> both own. **Fixed:** compare_exchange.

### M-6 Config section header OOB [config.cpp:1511-1513]
Malformed `[section` -> index npos. **Fixed.**

### M-7 IsCurrent always TRUE [dxgi_factory.cpp:437-439]
No hotplug notify. **Fixed:** re-enumerates, compares LUID.

### M-8 Present1 null params [dxgi_swapchain.cpp:371]
Null `pPresentParameters` -> crash. **Fixed:** empty params.

### M-9 WaitForVBlank drift [dxgi_output.cpp:463-493]
Estimated timing drifts. **Deferred:** needs Vulkan timing ext.

### M-10 Config overflow audit [d3d11_options.cpp]
`maxTessFactor` clamped. `maxAvailableMemory` uint32 wrap possible (manual misconfig). **Deferred.**

### M-11 avgFrameDurationUs non-atomic RMW [framepacer/dxvk_framepacer.cpp:166]
**Fixed:** CAS loop.

### M-12 No monotonic clock guard [framepacer/dxvk_framepacer.cpp:134]
Clock regress -> long hang. **Fixed:** minWait guard (full detection deferred).

### M-13 TOCTOU m_lastFrameStart [framepacer/dxvk_framepacer.cpp:124-141]
Stale read window. **Deferred:** wide critical section, low risk.

### M-14 notifyWorkers wake-one [dxvk_pipemanager.cpp:67-79]
One wake under load. **Fixed:** notify_all when queue depth > 1.

### M-15 O(n) removePipeline scan [dxvk_pipecompiler.cpp:179-200]
4096-entry scan locks workers. **Deferred:** teardown only.

### M-16 startWorkers exception hole [dxvk_pipemanager.cpp:82-126]
OOM -> join deadlock on never-started threads. **Fixed:** WorkerGuard RAII.

### M-17 Worker Lowest priority [dxvk_pipemanager.cpp:121]
Async compile starved under render. **RESOLVED (round 4):** dyasync now Lowest too.

### M-18 Chunk pool unbounded [dxvk_cs.cpp:90-95]
**Fixed:** cap 128.

### M-19 Callbacks under mutex [sync_signal.h:148-161]
Reentrancy deadlock. **Fixed:** callbacks outside mutex.

### M-20 Frame duration double-counts idle [d3d11_swapchain.cpp:462-465]
Measures wall time from frame start. **Deferred:** measurement inaccuracy.

### M-21 ApplyDirtyNullBindings overhead [d3d11_context_imm.cpp:985-1069]
2304 bit checks per flush. **Deferred:** perf.

### M-22 FindMapEntry linear scan [d3d11_context_def.cpp:427-442]
O(n) deferred context. **Deferred.**

### M-23 D3D9 staging double-copy [d3d9_device.cpp:5582-5616]
CPU memcpy + GPU copy for mappable buffers. **Deferred:** perf.

### M-24 Frontbuffer blit every frame [d3d9_swapchain.cpp:165-182]
~8ms GPU per frame wasted. **Fixed:** lazy on GetFrontBufferData.

### M-25 LockBuffer ignores DISCARD [d3d9_device.cpp:5511-5534]
Torn content on staging path. **Deferred:** spec-correct.

### M-26 m_deviceFeatures race [d3d11_device.cpp:1348-1353]
**Fixed:** reads under lock.

### M-27 UpdateSubresource always discards [d3d11_context.cpp:5780-5783]
Alloc churn. **Fixed:** in-place write when GPU free.

### M-28 UnmapTextures LRU no size priority [d3d9_device.cpp:9043-9065]
**Deferred:** perf.

### M-29 ExecuteFlush hEvent empty [d3d11_context_imm.cpp:1096-1097]
**Fixed:** early return, direct signal.

### M-30 MapBuffer uncached GPU sync [d3d11_context_imm.cpp:389-395]
**Deferred:** Gen9 specific.

### M-31 Budget tracking stale [dxvk_memory.cpp:2444-2448]
500ms refresh. **Deferred.**

### M-32 tryAcquire UAF race [dxvk_sparse.h:535-544]
**Deferred:** mitigated by calling conventions.

### M-33 UMA separate pools [dxvk_memory.cpp:833-835]
**Deferred:** minor waste.

### M-34 Defrag 12.5% tolerance [dxvk_memory.cpp:2553-2555]
**Deferred:** perf tradeoff.

### M-35 m_mode not atomic [framepacer/dxvk_framepacer.h:42]
**Fixed:** atomic.

### M-36 FpsLimiter m_nextFrame [util_fps_limiter.cpp:68-72]
**Fixed:** inside lock (folded H-15).

## LOW BUGS / CODE QUALITY

- **L-1** dead `line.empty()` after `[` check [config.cpp:1505-1518] - **Fixed** removed
- **L-2** Release ~0u on null dispatch [dxgi_swapchain_dispatcher.h:41] - **Fixed** return 0
- **L-3** int32 duration blend overflow [framepacer/dxvk_framepacer.cpp:56-62] - **Fixed** int64
- **L-4** missing Gen9 IDs (Broxton/APL 0x0A84/0x1A84/0x5A84/0x5A85/0x5A8A, 0x22B0/0x22B1) [dxvk_adapter.cpp:386-405] - **Fixed** added
- **L-5** dyasync GPL-only fallback [dxvk_graphics.cpp] - deferred, Gen9 has GPL
- **L-6** fixed 16KB chunk size [dxvk_cs.h:15] - deferred, no adaptive growth
- **L-7** pushData unaligned offset [dxvk_cs.h:284-295] - deferred, x86 OK / ARM crash
- **L-8** SetLight unbounded resize [d3d9_stateblock.cpp:190-192] - cap at 8
- **L-9** NotifyWindowActivated deadlock [d3d9_device.cpp:9088-9103] - try_lock
- **L-5** dyasync placeholder empty attachment mask [dxvk_graphics.cpp:1114] - GPL early-return means mask NEVER upgraded; wrong load/store + feedback tracking. **Fixed round 6.** (was: "deferred, Gen9 has GPL")
- **L-10** UpdateTextureFromBuffer staging always [d3d9_device.cpp:5340-5365] - direct CPU write when host-visible
- **L-11** UpdateClipPlanes uploads 6 always [d3d9_device.cpp:6095-6112] - upload only enabled
- **L-12** UMA = FALSE [d3d11_features.cpp:77] - report TRUE when `isUnifiedMemoryArchitecture()`
- **L-13** FF dirty flags every change [d3d9_device.cpp:139-146, 7353-7535] - group by stage
- **L-14** spec function dup per stage [d3d9_shader.cpp:653-701] - share across stages
- **L-15** StandardSwizzle = FALSE [d3d11_features.cpp:76] - implement if needed
- **L-16** small dynamic buffers not throttled [d3d11_context_imm.cpp:363-364] - count toward budget
- **L-17** UpdateMappedBuffer dup entries [d3d11_context_def.cpp:366-371] - remove redundant AddMapEntry
- **L-18** UpdateTexture staging always [d3d11_context.cpp:5654-5662] - direct upload
- **L-19** state cache overhead [d3d11_device.cpp:1041-1103] - skip one-shot
- **L-20** barrier merge no array layers [dxvk_barrier.cpp:483-517] - extend
- **L-21** barrier hash 32 buckets [dxvk_barrier.h:199] - adaptive
- **L-22** chunk growth policy [dxvk_memory.cpp:1518-1520] - shrink idle
- **L-23** maxBudget device-local only [dxvk_memory.cpp:2457-2459] - apply to system heap
- **L-24** no defrag mapped pools [dxvk_memory.cpp:2767-2768] - periodic compaction
- **L-25** recycler mutex not lock-free [dxvk_recycler.h:57] - rename/document

## OPTIMISATIONS

- **O-1** pipecompiler growable/ring queue - **Done** (dynamic deque)
- **O-2** aging bg entries -> live - **Done** (starvation counter)
- **O-3** per-thread queues - not done, shared mutex
- **O-4** per-type pipeline locks [dxvk_pipemanager.cpp:120] - **Done** compute/graphics split
- **O-5** NV_vulkan_exp visibility [d3d9_device.cpp] - low impact on Intel Gen9
- **O-6** skip frontbuffer blit unless GetFrontBufferData - = M-24, **Done**
- **O-7** D3D9 CB ring buffer - = H-16, **Done**
- **O-8** skip FlushInitCommands lock if empty - = H-17, **Done**
- **O-9** batch null binding updates - not done
- **O-10** D3D9 direct CPU upload - not done
- **O-11** remove 16ms periodic wake - = H-11, **Done**
- **O-12** unified UMA pool - not done
- **O-13** barrier merge array layers - **Done round 13**

## BUILD & CI

- **B-1** stale `build/` dir - removed
- **B-2** no CI pipelines - workflows manual-only
- **B-3** no 32-bit builds - build-win32.txt exists, not set up
- **B-4** libdisplay-info pnp-id generator refs Linux `/usr/share/hwdata/pnp.ids` - verify on Windows
- **B-5** no tests (`meson test` empty)
- **B-6** version v3.0.2 tag via git describe - acceptable
- **B-7** upstream divergence - fork at v3.0.2, upstream moved on

## FIX STATUS (2026-08-04, round 5)

### FIXED (61)
- **C-1** queue purge in removePipeline
- **C-2** FramePacer dtor clearCallbacks
- **H-1** CloseHandle failure paths
- **H-2** queue grows, no cap
- **H-3** starvation counter (24)
- **H-4** mutex-guarded m_lastFrameStart
- **H-5** sleep_until re-check + minWait
- **H-6** has_value guard
- **H-9** int/fract overflow guards
- **H-10** bounds clamp
- **H-11** wait() no herd
- **H-12** idleWorkers atomic
- **H-13** WaitIdle before signal
- **H-14** reverted (idempotent)
- **H-15** m_nextFrame under lock
- **H-16** ring buffer 4-slot
- **H-17** atomic init gate
- **M-1** notify inside lock
- **M-2** spread initial pick
- **M-3** safe levelCount
- **M-4** || not |
- **M-5** compare_exchange
- **M-7** IsCurrent re-enumerate
- **M-8** empty params
- **M-11** CAS loop
- **M-12** minWait guard
- **M-14** notify_all depth>1
- **M-16** WorkerGuard RAII
- **M-18** chunk cap 128
- **M-19** callbacks outside mutex
- **M-24** lazy frontbuffer blit
- **M-26** feature reads under lock
- **M-27** in-place write
- **M-29** early return + direct signal
- **M-35** m_mode atomic
- **M-36** folded into H-15
- **A-1** D3D9 framepacer wired (`ctx->signal` + endFrame pre-flush, firstFrameId=0)
- **A-2** dyasync stop+join before maps destruct
- **A-3** workers halved when dyasync (4-core Gen9: 4+2 -> 1+2)
- **A-4** dyasync Lowest priority
- **A-5** dedupe (pipeline, state hash)
- **A-6** Gen9 options resolved in init list - dyasync now spawns on Gen9
- **L-1..L-4** dead code, Release 0, int64 blend, Gen9 IDs
- **T6.4** FramePacer wired into D3D9
- **O-4** pipeline mutex split
- **O-7** CB ring buffer (=H-16)
- **B-1** build/ removed

### FIX STATUS (2026-08-08, round 6)

- **N-1** present-fence UAF on recreate - reset fence on destroy
- **L-5** dyasync attachment mask placeholder - compute mask

### FIX STATUS (2026-08-10, round 7)

- **R7-1** pipecompiler double-join -> std::terminate on every dyasync teardown (A-2 fix regression: manager dtor stop()+join then ~DxvkPipelineCompiler joins again; join() detaches -> second join throws). Guard `worker.joinable()` in `joinAll()`. [dxvk_pipecompiler.cpp:155]
- **R7-2** D3D9+D3D11 swapchain present CS lambda captured raw `m_framePacer.get()`; swapchain destroy frees pacer while chunk queued -> UAF. Capture `shared_ptr` instead. [d3d9_swapchain.cpp:885, d3d11_swapchain.cpp:438]
- **R7-3** FramePacer GPU-completion callback captured raw `this`; queue thread fires after pacer destroyed -> UAF in recordFrameDuration. Timing state moved to shared `FramePacerTimings`, callback captures shared_ptr. [dxvk_framepacer.cpp:170, .h]
- **R7-4** M-29 regression: Flush1(hEvent) with no pending chunks SetEvent'd immediately -> event fired before in-flight GPU work done. Restored upstream `if (!GetPendingCsChunks() && !hEvent) return;`. [d3d11_context_imm.cpp:1100]
- **R7-5** mergeRanges merged barriers ignoring baseArrayLayer/layerCount + src/dstQueueFamilyIndex -> over-broad transitions, dropped QFO. Added to merge predicate; levelCount sum now uint64 (VK_REMAINING_MIP_LEVELS wrap). [dxvk_barrier.cpp:483-517]
- **R7-6** d3d9 constant_copy writeIntRange/writeBoolRange: `bufferSize - dstIndex*sizeof` uint32 underflow on bad range -> huge count. Early return when dstIndex out of range. [d3d9_constant_copy.cpp:345,376]

Verified non-issues this round: dyasync fast-path unref'd base handle (dxvk_graphics.cpp:1108) benign on 64-bit Gen9 - `mustTrackPipelineLifetime` Auto=false; SetLight unbounded resize matches upstream; compute async path sync (dxvk_compute.cpp:64) deferred; A-1..A-6, N-1, L-5 all confirmed in source.

### ROUND 8 (2026-08-11) - over-engineering review (ponytail)

Reviewed HEAD~2..HEAD diff (rounds 6-7), over-engineering only. One finding, ~5 lines removable:
- **Y-1** [framepacer/dxvk_framepacer.h:24-26, dxvk_framepacer.cpp:65-76] - `FramePacerTimings` struct wraps a single atomic purely to give the GPU-completion callback a shared_ptr target. Replace with `std::shared_ptr<std::atomic<int32_t>> m_avgFrameDurationUs`; callback captures that. **Delete struct + shrink free fn.**

Everything else in rounds 6-7 is minimal correctness: joinable() guard (R7-1), shared_ptr captures (R7-2/3), barrier merge predicate + uint64 sum (R7-5), constant_copy guards (R7-6), presenter fence reset (N-1), attachment mask (L-5), Flush1 restore (R7-4). No dead code, no speculative features.

### ROUND 9 (2026-08-11) - fresh scan + round-8 followup (ponytail)

- **R9-1** = Y-1 fixed: `FramePacerTimings` struct deleted; `std::shared_ptr<std::atomic<int32_t>> m_avgFrameDurationUs` [framepacer .h/.cpp]
- **R9-2** submit thread self-deadlock [dxvk_queue.cpp:201-218] - error branch called `m_device->waitForIdle()` while holding `m_mutex` with the failing entry still queued; `waitForIdle` re-locks and waits for `m_submitQueue.empty()` -> can never drain. Upstream-identical. **Fixed:** pop+notify first, waitForIdle outside lock.
- **R9-3** dyasync fast path ungated [dxvk_graphics.cpp:1106-1118, fork-only] - `getBasePipeline` called without `canCreateBasePipeline` gate: patched states (dual-source, flat shading, GS/tess, VS input promotion, depth-clip) linked from UNPATCHED shader libs -> wrong render ~1 frame; and instance never created -> every draw re-ran lock + vi/fo library lookup + queue push (worker bails at compilePipeline:1159) -> permanent churn, no FastPipeline variant ever. **Fixed:** gate on `canCreateBasePipeline`, `createInstance(state, true)` before unlock (base recached, cheap), so later draws hit findInstance and the worker compiles the optimized variant.
- **R9-4** present-fence race [dxvk_presenter.cpp:298-303 vs 1232-1262] - signalFrame (finish thread) `vkWaitForFences` on `m_lastPresentFence` while destroySwapchain (recreate) destroys that fence under `m_frameMutex`; reset at 1261 only covers sequential case. **Fixed:** check+wait under `m_frameMutex`; presentImage write also under `m_frameMutex`.
- **R9-5** D3D11 SyncFrameLatency callback UAF [d3d11_swapchain.cpp:649-664, upstream-identical] - callback captured raw `this`, fires on queue finish thread; swapchain dtor can free stats + event before queued present's signal. Same class as R7-3. **Fixed:** stats moved into `FrameStatistics` struct, callback captures shared_ptr.
- **R9-6** dead `DYASYNC_QUEUE_CAPACITY = 4096` const (growable-queue fix removed its use). **Deleted.**

Verified non-issues: stopWorkers join/restart (upstream-identical, unreachable at teardown); DxvkPipelineEntry dual-pointer (compute path sync, no perf case); writeBoolRange rounding (count always 4-aligned today); LockBuffer OffsetToLock clamp (API contract violation only); framepacer frame-identity TOCTOU (nit).

### ROUND 10 (2026-08-14) - fresh scan, 3 agents + verify (ponytail)

- **R10-1** [HIGH] D3D9 CB ring buffer regression [d3d9_constant_buffer.cpp/.h] - round-3 commit f82ad3af replaced allocator-backed `allocateStorage()` per wrap with a 4-slot ring reusing the SAME slice with no GPU/CS completion wait (the Rc in `m_ringSlots` keeps the allocation alive, bypassing the chunk-refcount fence tracking that makes upstream's per-wrap alloc safe). App ahead of CS thread by 4+ wraps (chunk-pool cap 128 allows this) -> slot clobbered while pending draws reference it -> wrong constants rendered; 64KB misc ring wraps every ~32 FF updates, so FF-heavy titles hit it regularly. **Fixed: reverted to upstream per-wrap `allocateStorage()`; both files byte-identical to upstream again.** H-16's "ring buffer exists, NOT A BUG" note was wrong - the ring WAS the bug.
- **R10-2** [MEDIUM] finish-thread self-deadlock [dxvk_queue.cpp:283-288] - R9-2 sibling: error branch called `m_device->waitForIdle()` with the failing entry still in `m_finishQueue`; waitForIdle waits on `finishQueue.empty()` -> never drains -> hang (non-DEVICE_LOST vkWaitSemaphores failure). **Fixed: waitForIdle flag, pop+notify first, wait outside lock.**
- **R10-3** [MEDIUM] pipeline cache destroy race [dxvk_device.cpp:985-1010] - `destroyPipelineCache()` (fork-only, saves cache to disk at exit) ran `vkGetPipelineCacheData`/`vkDestroyPipelineCache` without `lockPipelineCache()` while pipeline workers/dyasync threads can be mid-`vkCreateGraphicsPipelines` with the same cache -> Vulkan host-sync violation at teardown. **Fixed: whole body under lock.**
- **R10-4** [LOW] config parseInteger accepts +2147483648 [config.cpp:1618] - positive 2^31 wraps to INT32_MIN. **Fixed: limit 2147483647 for positive, 2147483648 only with '-' sign.**
- **R10-5** [LOW] dead `findInstanceLockFree` [dxvk_graphics.cpp:1142, .h:660] - never called. **Deleted.**

Deferred/benign this round:
- D3D11 SyncFrameLatency callback still captures raw event HANDLE (dtor CloseHandle, R9-5 fixed stats only) - upstream-identical; ReleaseSemaphore on closed handle benign unless value reused; event lives until dtor, callbacks fire on frame completion.
- Present-fence wait heuristic (R9-4) best-effort when frame latency > 1; infinite wait if a fence never signals (WSI bug) - no corruption either way, stall only.
- M-26 partial: 11 other `m_deviceFeatures` read sites outside lock - creation-time window only.
- FlushCsChunk TOCTOU [d3d11_initializer.h:44] - needs cross-thread create+use.
- ResizeBuffers `m_preferredBufferCount`/`m_allowTearing` race vs present thread - stale value, harmless.

AUDIT DOC CORRECTIONS:
- **L-8/L-9/L-10/L-11 NOT implemented** - d3d9_stateblock.cpp and d3d9_device.cpp are byte-identical to pristine upstream. SetLight cap, NotifyWindowActivated try_lock, UpdateTextureFromBuffer direct CPU write, UpdateClipPlanes enabled-only upload are still open TODOs, not fixes.
- dxgi_monitor.h bpp->format mapping and d3d9_options textureMemory INT32_MAX clamp are fork improvements over upstream (upstream hardcodes SRGB; upstream raw `<<20` can overflow) - correct, not bugs.

### ROUND 11 (2026-08-19) - fresh scan, 3 agents + verify (ponytail)

- **R11-1** [LOW/opt] barrier under-merge [dxvk_barrier.cpp:505] - `mergeRanges` contiguity tested each candidate against the run HEAD `v[i]` instead of the previous merged element, so a run of 3+ equal-levelCount mip ranges `[0,L),[L,2L),[2L,3L)` only ever merged the first pair and left the tail unmerged. Correct (each pair is exactly adjacent, uint64 range-end safe) but lost merging. **Fixed: compare `v[j]` to `v[j-1]`** so the whole contiguous run folds into one barrier.
- **R11-2** [LOW] eviction disabled on ALL UMA, not just Gen9 [dxvk_memory.cpp:2639] - fork added `|| m_device->isUnifiedMemoryArchitecture()` to `evictResources` early-return, so on any integrated/APU (AMD APU, Apple, non-Gen9 Intel) the allocator never returns memory to the driver under pressure, while `enableDefrag` (line 2787) is gated only on `isGen9LowPower()` - inconsistent. Upstream evicts on UMA. **Fixed: gate on `isGen9LowPower()`** so non-Gen9 UMA devices match upstream and only the Gen9 low-power target keeps eviction off.

Verified non-issues / documented:
- dyasync fast-path `queueCompilation(this)` with no `acquirePipeline()` [dxvk_graphics.cpp:1116] - latent-only: pipeline never destroyed mid-life (only in ~DxvkPipelineManager after `m_compiler->stop()`), AND acquire/release are no-ops on 64-bit Gen9 (`mustTrackPipelineLifetime`=false, round-7). Speculative - skipped (YAGNI).
- FpsLimiter `delay()` double-advance on concurrent callers = known O-14 (double-sleep), re-verified, skip.
- Compute async path [dxvk_compute.cpp:58-70] compiles synchronously (`createInstance`->`vkCreateComputePipelines`) then queues a redundant no-op entry (worker finds instance already created). Correct, no async benefit for compute, wasted queue slot only - design limitation, no placeholder mechanism. Deferred.
- pipecompiler dedup keyed on 64-bit state hash [dxvk_pipecompiler.cpp:179-186] - hash collision would permanently suppress a variant compile; very low probability, silent but benign (base pipeline still used). Deferred.
- d3d9_swapchain pacer `beginFrame()` sleep holds device lock [d3d9_swapchain.cpp:171] - only LowLatency/MinLatency modes; default MaxFrameLatency returns nullopt, never sleeps. Low usage, skip.
- d3d9_constant_copy bool SIMD writes up to 3 past clamped bound on `count % 4 != 0` - buffer is 64-byte allocator-padded, bools <= 16, not reachable. INFO only.

### ROUND 12 (2026-08-19) - fresh scan, 3 agents + verify (ponytail)

- **R12-1** [MEDIUM] maintenance4 NULL fn-ptr on 1.2 device [dxvk_memory.cpp:2275,2288] - fork downgraded `vk13 maintenance4` to `require=false` (dxvk_device_info.cpp:908) but the allocator still called `vkGetDeviceBufferMemoryRequirements`/`vkGetDeviceImageMemoryRequirements` unconditionally (O-16 incompleteness). On a true Vulkan-1.2 device without `VK_KHR_maintenance4` those pointers are NULL -> startup crash in `determineBufferMemoryTypes`/`determineBufferUsageFlagsPerMemoryType`. All callers already tolerate `return false` (they skip the memory-type refinement and fall back to defaults). **Fixed: guard both fns on `features().vk13.maintenance4`, return false when absent.**
- **R12-2** [MEDIUM] present-fence infinite wait holds frame mutex [dxvk_presenter.cpp:307] - `signalFrame` non-presentWait path did `vkWaitForFences(..., ~0ull)` while holding `m_frameMutex` (held to protect against `destroySwapchain` freeing the fence). A fence that never signals (WSI bug, or the fork's own `m_hasGamescopeFenceSignalBug` path) would block the finish thread forever AND, via the same mutex, hang any swapchain recreate. Round-10 deferred it as "stall only, no corruption", but a permanent hang on recreate is worse. **Fixed: bounded 1s timeout instead of infinite.**
- **R12-3** [LOW/opt] O-14 double-sleep resolved for explicit limits [d3d11_swapchain.cpp:416, d3d9_swapchain.cpp:171] - when an explicit frame-rate limit is set (`m_targetFrameRate != 0`), the FpsLimiter already paces on the present/finish thread, and the FramePacer LowLatency `beginFrame` sleep added on top on the app thread -> additive sleeps -> sustained FPS under target. **Fixed: skip `m_framePacer->beginFrame()` when a limit is active.** Positive explicit limits sleep immediately in `delay()`; the auto-refresh heuristic (negative) self-corrects within ~8 frames. `getEffectiveFrameLatency` (latency-1 forcing) is independent of `beginFrame` and unaffected. First-present one-frame ordering artifact in D3D9 (beginFrame before UpdateTargetFrameRate) is negligible.

Verified non-issues / documented this round:
- dyasync fast path "double-compile" (dxvk_graphics.cpp:1114) - NOT a bug: `isCompiling.exchange(VK_TRUE)` (graphics.cpp:1170) is atomic-exclusive, so only one of the dyasync worker or the state-cache worker wins; no duplicate optimized compile.
- m_deviceFeatures unlocked reads (d3d11_device.cpp ~10 sites) - known M-26 partial, deferred rounds 10-11 (creation-time window, needs concurrent D3D11CreateDevice).
- RegisterDeviceRemovedEvent fires only at registration if already lost (d3d11_device.cpp:1977-1996) - fork strictly better than upstream's log-only stub; later-loss signaling needs a monitor thread, not worth it. Deferred.
- ApplyDirtyNullBindings "2304 checks" (M-21) - false: `bit::BitMask` iterates only set bits, cost scales with dirty bindings not total slots. Not an optimization target.
- UpdateClipPlanes fixed-size MaxClipPlanes (L-11) - 96 vs 16 bytes negligible, and GPL shader lib expects all 6 slots zero-filled; shrinking would break GPL linking. Skip.
- d3d11_context.cpp / d3d9_device.cpp byte-identical to upstream, so L-10/L-18 direct-CPU-upload are upstream behavior, not fork regressions. Skip.
- dxgi_swapchain dirty-rect forwarding to Presenter::Present (dxgi_swapchain.cpp:375) is inert (D3D11 Present ignores pPresentParameters) - harmless dead code, no perf impact. Skip.

### ROUND 14 (2026-08-24) - full diff-surface rescan, post-migration

Coverage note: background subagent infra failed 5/5 attempts (silent deaths), so this round
ran as a single-agent exhaustive pass: every one of the 59 files where fork differs from
pristine `dxvk/` (CR-stripped compare) reviewed hunk-by-hunk, including the four fork-only
files (pipecompiler .cpp/.h, framepacer .cpp/.h). CRLF note: pristine tree still has CRLF -
byte-compare against it MUST use `--strip-trailing-cr` or everything reports DIFF.

- **R14-1** [LOW/perf] Map() early CS-sync gate incomplete [d3d11_context_imm.cpp:385-387] -
  the P-5-class fast path skipped `SynchronizeCsThread` on `!isInUse()` alone, but access refs
  attach at chunk EXECUTION, so a dispatched-but-unexecuted chunk is invisible. Correctness is
  backstopped by the unconditional `WaitForResource` fallthrough (its own gate syncs when
  `lastSeq < seq` and rechecks), so no corruption. Real cost: with a queued READ pending,
  `hasRwAccess` reads false -> `doInvalidatePreserve` promotion lost -> full GPU wait that
  upstream's early drain would have avoided. **Fixed: sync when `!csIdle || isInUse(W|R)`;
  skip only when every chunk up to the resource's sequence number has executed.**

Verified non-issues / documented this round (all personally traced):
- `computeFullSubgroups` true->false flip [dxvk_device_info.cpp] - zero code dependencies;
  wave/subgroup ops cannot occur in D3D9/D3D11 shaders (D3D12-only feature); defrag site
  guards `subgroupSizeControl` separately. Safe.
- `notifyWorkers` reads `queue.size()` - all three call sites hold `m_lock`. Safe.
- shader-cache `freeInstance` rewrite [dxvk_shader_cache.cpp:684] - revive-guard + identity
  check is strictly SAFER than upstream (upstream deletes even when revived between fetch_sub
  and lock). Fork improvement, keep.
- TBDR architecture flag TRUE for vendor 0x1010 [d3d11_features.cpp] - correctness improvement
  over upstream always-FALSE; Gen9 unaffected.
- EnumAdapterByGpuPreference if/else refactor - behavior-equivalent to upstream reverse-index.
- P-2 GetFrontBufferData hoist - blit idempotent across repeated calls; validation order sound.
- P-1 IsCurrent sorted-handle cache - `m_adapters` written only in ctor; enumerate+sort+equal
  detects hotplug identically to property-query approach.
- Presenter fence lifecycle complete: presentImage store + signalFrame bounded wait +
  destroySwapchain reset ALL under `m_frameMutex`; frameId match guard prevents stale waits.
- mergeRanges (R11-1/O-13) re-derived edge cases: run-dimension lock-in, REMAINING_* excluded
  both sides, uint64 ends, writeIdx compaction never overtakes scan index. Clean.
- pipecompiler requestStop notify-outside-lock: state change under same mutex as wait predicate
  -> lost wake impossible regardless of notify placement.
- dyasync fast path (R9-3) brace/fallthrough re-verified verbatim; lock order m_mutex ->
  pipelineCacheLock consistent both paths; getBasePipeline under m_mutex matches worker side.
- compute getPipelineHandle P-4 restructure - post-unlock `instance->handle` read is
  upstream-parity pattern; no fork regression.
- d3d9/d3d11 beginFrame gating self-consistent when fps-limit active from frame 0:
  m_lastFrameStart stays nullopt -> endFrame no-ops; mid-run activation degrades one blend
  sample, smoothed 3:1.

Perf verdict: no new perf lands justified this round. Round-13 batch (P-1..P-5, O-13)
re-verified sound end-to-end. Remaining deferred items re-assessed: M-23 fix would diverge
from now-byte-identical upstream d3d9_device.cpp; O-9 remains debunked by M-21 analysis;
O-12 speculative without profiling. Kept deferred.

### ROUND 15 (2026-08-24) - fresh scan, 3 agents + verify (ponytail)

Coverage: A=concurrency hot spots, B=d3d9+dxgi, C=d3d11+dxvk core (all 59-file diff surface).

- **R15-1** [MEDIUM/deadlock] startWorkers exception path self-deadlock [dxvk_pipemanager.cpp] -
  the M-16 "WorkerGuard RAII" fix (round 5) was itself a regression. Both call sites
  (`compilePipelineLibrary`/`compileGraphicsPipeline`) hold `m_lock` across `startWorkers()`;
  its catch block joined partially-spawned workers inline. A thread-spawn throw at iteration >=1
  left new workers blocked acquiring `m_lock` (first action of `runWorker`) that the joining
  thread still held -> permanent device-wide pipeline-creation hang. Upstream has no try/catch
  and unwinds cleanly on the same throw. Reachable whenever workerCount >= 2 (8-core Gen9,
  non-dyasync configs); dyasync 4-core default spawns 1 worker so misses it. **Fixed: try/catch
  deleted, upstream spawn loop restored** - already-started workers keep idling and are joined
  by stopWorkers() at teardown. M-16's original rationale ("join deadlock on never-started
  threads") was wrong analysis; see corrections table.
- **R15-2** [LOW] pushEntry dedupe-key tombstone leak [dxvk_pipecompiler.cpp] -
  key inserted into `m_queuedGraphicsPipelines`/`m_queuedComputePipelines` before
  `queue.push_back`; a throwing push_back (deque node bad_alloc) left the key in the set with no
  queued entry, silently suppressing that state's optimized dyasync compile for the pipeline's
  lifetime (H-2 consequence class via exception safety instead of queue cap). **Fixed: rollback
  erase of the just-inserted key on throw; deque::push_back strong guarantee keeps entry intact
  in catch.**

Verified non-issues / documented this round (all agent claims personally verified before edit):
- Full d3d9+dxgi diff surface clean (Agent B). INFO: dxgi_options.cpp `<<20` hunk is a
  behavioral no-op (VkDeviceSize cast precedes shift upstream too - never credit it as an
  overflow fix); GetFrontBufferData P-2 hoist empty-vector exposure pre-exists at the
  upstream-identical `GetFrontBuffer()->back()` deref - any future guard must cover line 258 too.
- Full d3d11+dxvk core diff surface clean (Agent C). Extra verifications PASS:
  m_compiler declared-before-maps lifetime chain; seqDispatch atomization strictly removes
  upstream's inherent race; initializer gate worst case one-chunk delay, no lost command;
  shader-cache writer call_once respawn path unreachable; WriteToSubresource depth-pitch is a
  fork FIX of a real upstream bug; config.cpp regex catch widened to std::exception fixes
  upstream terminate-on-malformed-regex; numDyasyncThreads negative falls through to auto.
- destroyPipelineCache `std::vector<char> data(dataSize)` under manual lock pair: bad_alloc
  would skip unlock, but only caller is ~DxvkDevice -> std::terminate either way. Below bar,
  untouched.
- Perf verdict: no new perf lands justified; deferred list unchanged (rounds 12-14 assessments stand).

## BUILD VERIFIED
Release clean on Linux host (llvm-mingw linux-x86_64 + ninja + glslang), exit 0, 5 DLLs.
Round 14 rebuild after R14-1: exit 0. Round 15 rebuild after R15-1/R15-2: exit 0.
Warnings pre-existing only.
NOTE: shell PATH must include tools/llvm-mingw-*-linux-x86_64/bin AND tools/ninja-linux
before ninja, else `ninja: command not found` / `x86_64-w64-mingw32-g++: not found`
(no-op rebuilds mask this).


## AUDIT HISTORY

### NOT FIXED (deferred)
- **M-9** WaitForVBlank drift - needs Vulkan timing ext
- **M-10** config overflow - maxAvailableMemory wrap (manual misconfig only)
- **M-13** TOCTOU m_lastFrameStart - low risk
- **M-15** O(n) queue scan - teardown only
- **M-20** duration double-counts idle - measurement
- **M-21** ApplyDirtyNullBindings - perf
- **M-22** FindMapEntry O(n) - deferred ctx
- **M-23** D3D9 staging double-copy - perf
- **M-25** LockBuffer DISCARD - spec-correct
- **M-28** UnmapTextures LRU - perf
- **M-30** uncached GPU sync - Gen9
- **M-31** budget stale - 500ms
- **M-32** tryAcquire UAF - mitigated
- **M-33** UMA pools - minor
- **M-34** defrag 12.5% - tradeoff
- **L-5..L-25** code-quality/perf (above)
- **O-3, O-9, O-10, O-12** not done (**O-13 done round 13**)
- **B-2..B-7** CI/32-bit/tests/upstream

### OPEN ITEMS (merged from old round-4 do-file, still valid)

- **O-14** FramePacer + FpsLimiter double-sleep: presenter `m_fpsLimiter.delay()` (fps-limit heuristic) + FramePacer beginFrame sleep (LowLatency) are additive -> under-target FPS. **RESOLVED round 12 for explicit limits (R12-3): swapchain skips `beginFrame()` when a limit is active.** Auto-refresh (negative) heuristic still self-corrects within ~8 frames.
- **O-15** Cold state cache = synchronous compile: dyasync fallback needs shader-library handles; cold cache `VK_NULL_HANDLE` (graphics.cpp:1388-1392) -> first draw sync-compiles. dyasync helps warm cache only; state-cache tuning > pipecompiler tweaks on Gen9.
- **O-16** Vulkan 1.2 downgrade incomplete: RESOLVED round 13 (R13-2) - every feature with unguarded use restored to required; genuinely-unused ones stay optional with verified guard status. Fork now rejects unsupported devices cleanly at creation instead of crashing/corrupting later.
- **O-17** Runtime-verify dyasync spawns on Gen9: check log for "Using N dyasync compiler threads" + "dxvk-shader" thread names.
- **O-18** RegisterDeviceRemovedEvent never signals after the fact: round 16 reduced state to an atomic cookie counter; SetEvent fires only at registration when the device is already dead. Proper fix needs a device-loss callback from DxvkDevice/queue-error paths - feature work, belongs in a bug-audit round with runtime testing.

### ROUND 13 (2026-08-24) - Linux migration + fresh scan, 3 agents + verify

Workspace migrated Windows -> Linux. Toolchain rebuilt under `tools/`:
llvm-mingw-20260616 **ucrt-linux-x86_64** (same clang 22.1.8), ninja 1.13.2 linux,
glslang 16.5.0 linux. Meson runs from `tools/meson-1.11.2/meson.py` via system python3.
Build verified: `ninja -C build-release` exit 0, all 5 DLLs (PE32+ x86-64).

- **R13-1** [BLOCKER] CRLF contamination from migration - 840 files (`build-win64.txt`,
  `package-release.sh`, headers, sources) had `\r\n`; broke shebang execution
  (`gen-search-table.py` -> `env: python3\r`) and polluted diffs. Fixed: LF-normalized
  whole tree (`.git*` excluded).
- **R13-2** [CRASH class] O-16 RESOLVED - 10 Vulkan features were flipped optional by the
  1.2 downgrade while their use sites stayed upstream-unconditional. Restored to REQUIRED
  (upstream parity), each verified against concrete use:
  - `khrMaintenance5`: vkCmdBindIndexBuffer2KHR (every indexed draw) +
    vkGetDeviceImageSubresourceLayoutKHR + module-less pipeline creation ->
    NULL fn ptr on first indexed draw without it.
  - `khrMaintenance6`: vkCmdBindDescriptorSets2KHR / vkCmdPushConstants2KHR /
    vkCmdSetDescriptorBufferOffsets2EXT on every descriptor bind/push.
  - `vulkanMemoryModel`: emitMemoryModel() declares MemoryModelVulkan for EVERY shader.
  - `maintenance4`: OpExecutionModeId LocalSizeId in every CS (R12-1 allocator guards stay as defense).
  - `shaderDemoteToHelperInvocation`: OpDemote emitted for every PS discard/texkill,
    meta copy/resolve stencil path; no fallback lowering in dxbc-spirv.
  - `khrLoadStoreOpNone`: adjustAttachmentLoadStoreOps sets LOAD/STORE_OP_NONE unconditionally.
  - `inlineUniformBlock`: createSpecDataSetLayout uses INLINE_UNIFORM_BLOCK when descriptor buffer active.
  - `scalarBlockLayout` + `uniformBufferStandardLayout` + `shaderInt8`: SPIR-V offset
    decoration scheme + sub-dword push-data lowering assume them.
  Net effect: devices lacking these now fail device creation cleanly instead of
  NULL-derefing / miscompiling mid-session. Remaining truly-optional features have no
  unguarded use (verified: bDA unused, descriptorIndexing keyed via SupportsResourceIndexing,
  subgroupSizeControl guarded at defrag site, zeroInit never emitted, timelineSemaphore still true).
- **R13-3** [MEDIUM/corruption] M-27 regression: `UpdateMappedBuffer` in-place write raced
  dispatched-but-unexecuted CS chunks - isInUse() refs attach only at chunk EXECUTION, so a
  queued CopyResource/draw let memcpy hit live storage (lost update / torn reads) on DEFAULT
  constant buffers. Fixed: in-place only when `lastSequenceNumber() >= GetCurrentSequenceNumber()`
  AND both isInUse bits clear; else upstream rename path. [d3d11_context_imm.cpp]
- **R13-4** [MEDIUM/sync] Unattributed stage-mask narrowings reverted to upstream ALL_COMMANDS:
  - swapchain blitter post-present barrier dstStageMask (backbuffer not access-tracked;
    later transfer/compute consumers relied on this barrier).
  - acquire/releaseExternalResource (D3D11on12 interop): external API may use ANY stage;
    ALL_GRAPHICS skipped waits for compute/transfer writers and dropped visibility of ours.
- **R13-5** [LOW/hygiene] dxgi factory: dead `m_windowAssociationFlags` store+member deleted;
  `m_windowAssociation` made `std::atomic<HWND>` (formal race MakeWindowAssociation vs Get).

Perf (all verified, built):
- **P-1** IsCurrent(): sorted physical-device handle-set cache replaces per-call
  enumerate+Nx Properties2 queries; count+sorted-handle compare detects hotplug identically.
- **P-2** GetFrontBufferData: extra-frontbuffer StretchRect hoisted behind null/pool/
  devicelost validation (was executed then discarded on rejected calls).
- **P-3** FramePacer::beginFrame gated on needsGpuSignal() (+ existing fps-limit gate) in
  d3d9+d3d11 swapchains: default MaxFrameLatency mode skips 2 locks + 2 clock reads/frame.
- **P-4** Compute spec-constant path: removed always-redundant queueCompilation after sync
  createInstance (worker would find instance and bail; wasted slot+wakeup per unique state).
- **P-5** WaitForResource: skip SynchronizeCsThread when already drained past SequenceNumber
  (exact no-op fast path; saves locked condvar roundtrip per idle Map).
- **O-13 DONE** mergeRanges extended: adjacent array-layer runs fold like mips
  (single-dimension runs only - mixing dimensions would over-cover); VK_REMAINING_*
  counts excluded from adjacency on BOTH sides; layer sums in uint64. Cubemap/layer-array
  batches now collapse to one barrier.

Documented deltas (verified intentional/benign, NOT bugs):
- SDMA transition srcStages ALL_COMMANDS->ALL_TRANSFER kept: transfer-only command buffer,
  cross-queue ordering via semaphores; unreachable beyond transfer stages on Gen9.
- D3D9/D3D11 UAV-unbind refactor (mask test vs uavStages): equivalent - non-CS stages map
  to same graphics UAV slots (nulling idempotent) and masks only dirty for pixel/compute.
- EnumOutputs drops upstream's isLinkedToDGPU early-return but enumerates own+linked iGPU
  LUIDs: intentional so muxless-laptop iGPU (the fork's target) sees displays. Duplicate
  HMONITORs across adapters possible; accepted.
- config parser treats `#`/`;` as inline comments (upstream does not) - behavioral delta,
  keep; parsing hardening itself correct.
- dxgi_output BitsPerColor 10->8: Gen9 SDR tuning, accepted.
- isGen9LowPower includes 0x2200-0x22FF (Gen8 BSW/CHV) and 0x0A84/0x1A84 (Haswell-era?):
  profile-gating only, type+vendor filtered; verify intent if those targets matter. INFO.
- L-12 UMA=TRUE never implemented (both trees report FALSE) - stays open, doc claim removed.
- "1024MB budget" corrected: default is heapSize/2; 1024MB only via dxvk.maxMemoryBudget.

Verified non-issues this round: mergeRanges REMAINING wrap previously unreachable (view
ranges absolute; context emits no REMAINING mips) - now also guarded; pipeline-cache lock
invariant re-verified at all 8 cache sites + destroy; dyasync lifetime chain, shared_ptr
captures, presenter fence bounded-wait, queue pop-before-waitForIdle, constant_buffer
byte-identity all re-checked PASS. MultiDrawIndirectCount/drawIndirectCount optional-feature
exposure is upstream-identical private-extension behavior (INFO, not touched).

## AUDIT HISTORY

| Date | Coverage | Analyzer |
|------|----------|----------|
| 2026-07-27 | Initial audit | opencode explore |
| 2026-07-28 | Deep-dive: framepacer, pipecompiler, memory, barrier, D3D11, D3D9 | 3 agents |
| 2026-07-28 | Verify + merge | opencode |
| 2026-07-28 | Round 2: H-11..H-17, M-14/16/18/19/24 | opencode |
| 2026-07-29 | Round 3: H-16, M-11/12/26/27/29/35 | opencode |
| 2026-08-01 | Round 4: A-1..A-5 | opencode |
| 2026-08-04 | Round 5: A-6 + compress | opencode |
| 2026-08-08 | Round 6: N-1 present-fence UAF, L-5 mask | opencode + explore |
| 2026-08-10 | Round 7: R7-1..R7-6 (double-join, pacer/swapchain UAFs, Flush1, barrier merge) | opencode + 3 explore agents |
| 2026-08-11 | Round 8: over-engineering review (ponytail) - Y-1 | opencode |
| 2026-08-11 | Round 9: Y-1 fix, queue deadlock, dyasync fast-path gate, present-fence race, SyncFrameLatency UAF, dead const | opencode + 3 agents |
| 2026-08-14 | Round 10: D3D9 ring regression revert, finish-thread deadlock, pipeline-cache destroy race, config overflow, dead code; L-8..L-11 corrections | opencode + 3 agents |
| 2026-08-19 | Round 11: barrier under-merge, UMA eviction gate narrow | opencode + 3 agents |
| 2026-08-19 | Round 12: maintenance4 NULL fn-ptr guard, present-fence bounded wait, O-14 double-sleep gate | opencode + 3 agents |
| 2026-08-24 | Round 13: Linux migration recovery, O-16 feature-gate sweep resolved, M-27 race fix, sync-narrowing reverts, perf batch (P-1..P-5, O-13) | 3 agents + verify |
| 2026-08-24 | Round 14: full 59-file diff-surface rescan, Map-path sync-gate completion (R14-1); perf batch re-verified | single agent (subagent infra down) |
| 2026-08-24 | Round 15: startWorkers exception deadlock regression reverted (M-16 correction), pipecompiler tombstone leak | 3 agents + verify |

Total: 104 found. After round 7: 69 fixes. Round 10: 80 total. Round 12: 85 total, 0 high, 14 deferred.
Round 13: +6 fixes (R13-1..R13-5 incl. O-16 resolution) + 6 perf lands (P-1..P-5, O-13), 0 high open, build restored on Linux host.
Round 14: +1 fix (R14-1, LOW/perf), full diff surface re-verified, 0 high open, 14 deferred unchanged.
Round 15: +2 fixes (R15-1 revert of M-16 regression, R15-2), full diff surface re-scanned by 3 agents, 0 high open.

## WHAT AUDIT GOT WRONG (corrected)

| Claim | Correct |
|-------|---------|
| `memory_order_seq_cst` in shader.h | `acquire`/`acq_rel` - correct |
| Fallback map dead code | Removed in 5e5ab75b |
| WaitForVBlank spin-loop | `Sleep::sleepUntil` - kernel sleep |
| m_compiledOnce set before compile | AFTER compile (line 299); m_needsCompile cleared early (line 280) - diff flag |
| Unbounded queues | Bounded 4096 - missed const |
| M-16 "join deadlock on never-started threads" | No upstream bug; the WorkerGuard fix itself deadlocked (join under caller's m_lock) - reverted round 15 (R15-1) |
| Queue cap in header | Was Sarek header; dxvk-630 moved to cpp:16 |
| H-5 busy-wait | while+sleep_until blocks; loop guards early return - correct |

| H-5 busy-wait | while+sleep_until blocks; loop guards early return - correct |

## ROUND 16 - CLEANUP PASS (over-engineering audit, applied)

3-agent ponytail-audit + manual verification of every finding. Build: ninja -C build-release exit 0 after edits.

Applied (code):
- pipecompiler: removed dead queueCompilation(DxvkComputePipeline*,...) overload (0 callers); merged Graphics/ComputeQueueKey into template QueueKey + shared hash; inlined single-caller helpers (TakeLane/select_lane/work_available/entry_is_valid/processEntry); collapsed 4-fn worker-count chain; shared dyasyncBaseWorkers() with pipemanager npWorkerCount formula.
- framepacer: hand-rolled retry-sleep loop replaced by Sleep::sleepFor (NT timer tuning); identical Low/Min latency cases merged; predict_wake_time switch collapsed to one guard.
- options: deleted write-only enableGen9Profile (0 readers).
- d3d11: m_removedEvents map+mutex -> atomic cookie counter (was write-only; removal-signaling still unimplemented = separate item); dropped HasPendingInitCommands() wrapper (FlushCsChunk self-guards); restored upstream EmitCsChunk text; removed cs-idle guard dup in context_imm Map path (synchronize() early-outs internally on m_seqOrdered).
- dxgi: EnumAdapterByGpuPreference HIGH_PERFORMANCE/default branches merged.

Rejected during verification:
- DXVK_FRAME_PACE env removal - documented feature (dxvk.conf:469), NOT YAGNI.
- Factory ctor/IsCurrent enumerate dedupe - preserving IsCurrent failure semantics eats the savings.

Repo hygiene (same pass): deleted build-debug (stale Windows config), tools/{zipped,dl,glslang-src,mingw-w64-src}, dxvk-venv, AgentLog, WORK_PLAN.md, root AUDIT duplicate, .github CI/templates. Parent repo: junk unstaged from index. .git-backup/ renamed back to live dxvk-630/.git.

Follow-ups: fork git index has pre-existing autocrlf false-modified state + symlink-vs-tracked header trees (migration artifact) - needs one-time renormalization commit before next real commit. AGENTS.md now documents dual-host toolchain paths.
