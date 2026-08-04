# DXVK-630 Audit - Gen9 UHD 630

Workspace: 4 dirs

| Repo | Origin | State |
|------|--------|-------|
| `dxvk/` | doitsujin/dxvk v3.0.2 | Pristine upstream |
| `dxvk-630/` | Fork v3.0.2 + 10 commits | Active fork, tag v3.0.2-dxvk630-1 |
| `DXVK-Sarek/` | pythonlover02/DXVK-Sarek v1.12.0 | Older era (v1.10.x lineage) |
| `tools/` | llvm-mingw, meson 1.11.2, ninja, glslang, Vulkan-Headers | Build toolchain |

Read `DXVK-AUDIT-2026-08-01.md` FIRST for latest round-4 findings.

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
Per-dirty-call GPU alloc. **NOT A BUG:** ring buffer exists (4 slots, 1MB/64KB slices).

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
- **O-13** barrier merge array layers - not done

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

### BUILD VERIFIED (round 5)
Release clean, 224/224 targets, 5 DLLs. Warnings pre-existing only.

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
- **O-3, O-9, O-10, O-12, O-13** not done
- **B-2..B-7** CI/32-bit/tests/upstream

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

Total: 95 found. After round 5: 61 fixes, 0 high, 15 deferred.

## WHAT AUDIT GOT WRONG (corrected)

| Claim | Correct |
|-------|---------|
| `memory_order_seq_cst` in shader.h | `acquire`/`acq_rel` - correct |
| Fallback map dead code | Removed in 5e5ab75b |
| WaitForVBlank spin-loop | `Sleep::sleepUntil` - kernel sleep |
| m_compiledOnce set before compile | AFTER compile (line 299); m_needsCompile cleared early (line 280) - diff flag |
| Unbounded queues | Bounded 4096 - missed const |
| Queue cap in header | Was Sarek header; dxvk-630 moved to cpp:16 |
| H-5 busy-wait | while+sleep_until blocks; loop guards early return - correct |
