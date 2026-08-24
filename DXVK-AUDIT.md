# DXVK-630 Audit - Gen9 UHD 630

## STATUS (round 16, 2026-08-24)

Fork = DXVK v3.0.2 + Gen9 changes. Diff surface vs pristine `dxvk/`: 56 changed files +
4 fork-only files (`dxvk_pipecompiler`, `framepacer/dxvk_framepacer`); everything else
byte-identical modulo EOL. Compare against upstream MUST use `diff --strip-trailing-cr`
(fork is LF-normalized, upstream CRLF).

| Metric | Value |
|--------|-------|
| Audit rounds | 16 (1-4 folded into initial squash; log below starts at 5) |
| Findings filed | C-1..2, N-1, H-1..17, M-1..36, L-1..25, A-1..6, O-1..13, B-1..7, R7-1..R15-2 |
| Open critical/high | 0 |
| Deferred | see DEFERRED below |
| Build | ninja -C build-release exit 0 on Linux host, 5 DLLs, warnings pre-existing only |

Build PATH needs `tools/llvm-mingw-*-linux-x86_64/bin` AND `tools/ninja-linux` before
running ninja, else silent tool-not-found (no-op rebuilds mask it). AGENTS.md lists both hosts.

## INVARIANTS (breaking any of these reopens a fixed bug)

- `d3d9_constant_buffer.cpp/.h` stays byte-identical to upstream. The round-3 ring buffer
  reused live slices with no GPU wait -> wrong constants on FF-heavy titles (R10-1, reverted).
- Every vkCreate*Pipelines call AND destroyPipelineCache() holds lockPipelineCache() (R10-3).
- Async callbacks capture shared_ptr, never raw this / raw handles (R7-2/3, R9-5).
- Queue threads pop the failing entry BEFORE waitForIdle(); waiting while it is still queued
  self-deadlocks (R9-2 submit thread, R10-2 finish thread sibling).
- Barrier merge predicate includes baseArrayLayer/layerCount + queue-family; layer sums uint64;
  adjacency tested against previous merged element; REMAINING_* excluded both sides (R7-5, R11-1).
- Dyasync teardown order: stop()+join workers before pipeline maps destruct (A-2, R7-1 joinable guard).
- In-place buffer writes require lastSequenceNumber() >= current seq AND !isInUse (R13-3).

## ARCHITECTURE

    App -> D3D8/D3D9/D3D10/D3D11/DXGI -> DxvkInstance -> DxvkAdapter -> DxvkDevice
      -> DxvkPipelineManager (dyasync pipecompiler + pipeline workers, per-type mutexes)
      -> DxvkContext (barriers, descriptors, draws) -> DxvkCsThread -> DxvkQueue (timeline semaphores)
      -> Presenter (swapchain, frame pacer, fps limiter)

Config layers: dxvk.conf -> env vars -> per-app profiles. State cache: IR shaders,
two-file .bin/.lut, async write-back, FNV-1a checksum.

## OPEN ITEMS

- **O-14** FramePacer+FpsLimiter double-sleep. RESOLVED for explicit limits (R12-3):
  beginFrame skipped when limit active. Negative auto-refresh heuristic self-corrects ~8 frames.
- **O-15** Cold state cache compiles synchronously (dyasync needs shader-library handles,
  cold cache VK_NULL_HANDLE). Warm-cache-only benefit.
- **O-16** Vulkan 1.2 downgrade. RESOLVED round 13 (R13-2): every feature with unguarded use
  restored to REQUIRED; devices lacking them fail creation cleanly.
- **O-17** Runtime-verify dyasync spawns on Gen9 (log line "Using N dyasync compiler threads").
- **O-18** RegisterDeviceRemovedEvent signals only at registration if already lost. Post-loss
  signaling needs a device-loss callback from DxvkDevice/queue-error paths - feature work for
  a bug-audit round with runtime testing.

## DEFERRED (not fixed, by choice)

| ID | Where | Why deferred |
|----|-------|--------------|
| M-9 | dxgi_output WaitForVBlank | needs Vulkan timing ext |
| M-10 | config maxAvailableMemory wrap | manual misconfig only |
| M-13/M-20 | framepacer TOCTOU / idle double-count | low risk / measurement |
| M-15 | pipecompiler O(n) scan | teardown only |
| M-21/M-22 | dirty-binding scan / FindMapEntry O(n) | perf; M-21 debunked (BitMask iterates set bits) |
| M-23/M-25/M-28 | d3d9 staging copy / DISCARD / LRU | perf or spec-correct; d3d9_device.cpp upstream-identical again |
| M-30..M-34 | memory subsystem tuning | Gen9 tradeoffs, detail in git history of this file |
| L-5..L-25 | code-quality/perf backlog | individual notes in git history of this file |
| O-3/O-9/O-10/O-12 | optimisation ideas | not justified (O-9 debunked via M-21 analysis) |
| B-2..B-7 | CI / 32-bit / tests / upstream drift | local-build fork |

## BUG CATALOG (final status per finding)

Critical/high - all fixed. C-1 queue purge (r5), C-2 dtor clearCallbacks (r5), N-1 present-fence
reset on recreate (r6; bounded wait r12-2; full m_frameMutex coverage r9-4). H-1..H-17 fixed r5
except: H-7/H-8 resolved properly by R13-2 feature restore (not merely guarded); H-14 fix reverted
same round (upstream idempotent); H-16 ring buffer itself later proved to BE the bug (R10-1 revert).

Medium - fixed r5 unless noted: M-1..M-5, M-7, M-8, M-11, M-12, M-14, M-18, M-19, M-24, M-26
(partial, creation-time window documented), M-27 (redone correctly R13-3), M-29 (redone R7-4),
M-35, M-36. Special cases:
- **M-16** WorkerGuard RAII (r5) was itself a deadlock regression; reverted R15-1, upstream spawn
  loop restored. See GOTCHAS.
- **M-17** Lowest worker priority is intentional (A-4), not a bug.
- **R13-4** two stage-mask narrowings reverted to ALL_COMMANDS (blitter post-present, D3D11on12
  external acquire/release).

Low: L-1..L-4 fixed r5. L-5 attachment-mask fix r6. L-8/L-9/L-10/L-11 are OPEN TODOs (audit once
wrongly listed them as implemented; corrected R10). L-12 open (both trees report UMA FALSE).
L-6/L-7/L-13..L-25 open backlog. R10-4 config INT32_MAX clamp fixed r10. R10-5 dead
findInstanceLockFree deleted r10. R15-2 pushEntry tombstone rollback fixed r15.

Optimisations done: O-1, O-2, O-4, O-6(=M-24), O-7(=H-16), O-8(=H-17), O-11(=H-11), O-13 (r13).
Not done: O-3, O-9, O-10, O-12. Build items: B-1 done; B-2..B-7 open.

## FORK-ROUND FIX INDEX (regressions and subtleties worth knowing)

| ID | Class | One-liner |
|----|-------|-----------|
| R7-1 | crash | joinable() guard in joinAll; second join threw std::terminate |
| R7-2/3 | UAF | swapchain present lambdas + pacer callback capture shared_ptr |
| R7-4 | logic | Flush1(hEvent) empty-chunk early return restored (upstream text) |
| R7-5 | corruption | barrier merge predicate + uint64 levelCount sum |
| R7-6 | overflow | constant_copy dstIndex bounds early return |
| R9-2/R10-2 | deadlock | submit/finish threads pop before waitForIdle |
| R9-3 | wrong-render | dyasync fast path gated on canCreateBasePipeline, instance seeded |
| R9-5 | UAF | SyncFrameLatency stats into shared FrameStatistics |
| R9-6 | dead code | DYASYNC_QUEUE_CAPACITY removed |
| R10-1 | corruption | D3D9 CB ring buffer reverted to upstream per-wrap alloc |
| R10-3 | host-sync | destroyPipelineCache under lockPipelineCache |
| R11-1 | perf-correctness | mergeRanges compares v[j] to v[j-1], whole runs fold |
| R11-2 | policy | eviction off only on Gen9, not all UMA |
| R12-1 | crash | maintenance4 allocator fns guarded, absent -> return false |
| R12-2 | hang | present-fence wait bounded at 1s instead of infinite |
| R12-3 | perf | beginFrame skipped when explicit fps limit active |
| R13-2 | crash class | 10 optional-downgraded features restored to REQUIRED |
| R13-3 | corruption | UpdateMappedBuffer in-place gated on seq + isInUse |
| R13-4 | sync | two stage-mask narrowings reverted to ALL_COMMANDS |
| R14-1 | perf | Map CS-sync gate completed (sync when !csIdle || isInUse) |
| R15-1 | deadlock | startWorkers try/catch deleted (was self-deadlock), upstream loop |
| R15-2 | leak | dedupe-key rollback on throwing push_back |
| R16 | cleanup | dead overload/option/map removed, template keys, Sleep::sleepFor, cs-idle dup guard gone (commit 2ad83181) |

Perf lands: P-1 IsCurrent handle-set cache, P-2 GetFrontBufferData blit hoist, P-3 beginFrame
gating, P-4 compute redundant queue drop, P-5 idle Map sync skip (all round 13).

## VERIFIED NON-ISSUES (do not re-flag)

- dyasync fast-path double-compile: isCompiling exchange makes worker vs state-cache mutually exclusive.
- dyasync fast-path unacquired lifetime: pipeline destroyed only after compiler stop; refcount no-ops on 64-bit Gen9.
- compute async path queues a no-op after sync createInstance: design limit, no placeholder mechanism (P-4 removed the waste).
- notifyWorkers reads queue.size() under m_lock at all sites.
- requestStop notify placement safe: state change under wait-predicate mutex, lost wake impossible.
- ApplyDirtyNullBindings cost scales with set bits, not slots.
- UpdateClipPlanes uploads all 6 planes: GPL lib expects zero-filled slots.
- dxgi_options <<20 hunk is behavioral no-op (VkDeviceSize cast precedes shift upstream too).
- dxgi_swapchain dirty-rect forwarding inert (D3D11 Present ignores pPresentParameters).
- RegisterDeviceRemovedEvent arg validation strictly improves upstream stub; post-loss gap = O-18.
- shader_cache freeInstance revive-guard strictly safer than upstream.
- WriteToSubresource depth-pitch is a fix of a real upstream bug.
- config regex catch widened to std::exception (upstream terminates on malformed regex).
- numDyasyncThreads negative falls through to auto.
- EnumAdapterByGpuPreference forward-order branch shared by HIGH_PERFORMANCE/UNSPECIFIED: equivalent.
- isGen9LowPower includes 0x2200-0x22FF and 0x0A84/0x1A84: profile-gating only, INFO.
- "1024MB budget": default is heapSize/2; 1024MB only via dxvk.maxMemoryBudget.
- bool SIMD writes past clamped bound unreachable (64-byte padded buffer, <=16 bools).
- destroyPipelineCache vector bad_alloc skips unlock: sole caller is ~DxvkDevice, terminate either way.

## GOTCHAS (claims the audit got wrong, corrected)

| Wrong claim | Correct |
|-------------|---------|
| memory_order_seq_cst in shader.h | acquire/acq_rel correct |
| fallback map dead code | removed in 5e5ab75b |
| WaitForVBlank spin-loop | Sleep::sleepUntil kernel sleep |
| m_compiledOnce set before compile | AFTER compile; m_needsCompile cleared early |
| unbounded queues | bounded, cap const moved to cpp |
| M-16 join deadlock on never-started threads | no such bug; the fix deadlocked (R15-1 revert) |
| H-5 busy-wait | blocks in sleep_until with guards; correct |
| L-8..L-11 implemented | were not; d3d9 files byte-identical to upstream (corrected R10) |
| H-16 ring buffer not a bug | the ring WAS the bug (R10-1) |

## ROUND LOG

| Round | Date | Scope | Result |
|-------|------|-------|--------|
| 1-4 | 08-01..04 | initial squash + do-file scan | 61 fixes by r5 checkpoint; do-file merged away |
| 5 | 08-04 | catalog sweep | FIXED(61) checkpoint, 15 deferred |
| 6 | 08-08 | fresh scan | N-1, L-5 |
| 7 | 08-10 | fresh scan | R7-1..R7-6 |
| 8 | 08-11 | ponytail diff review | Y-1 found (FramePacerTimings wrapper) |
| 9 | 08-11 | followup + scan | Y-1 done, R9-1..R9-6 |
| 10 | 08-14 | 3 agents + verify | R10-1..R10-5 incl. ring-buffer revert; L-8..L-11 correction |
| 11 | 08-19 | 3 agents + verify | R11-1, R11-2 |
| 12 | 08-19 | 3 agents + verify | R12-1..R12-3, O-14 explicit-limit resolution |
| 13 | 08-24 | Linux migration + scan | LF normalize, R13-2 feature restore, R13-3..R13-5, P-1..P-5, O-13 |
| 14 | 08-24 | single-agent exhaustive rescan (59-file surface) | R14-1 |
| 15 | 08-24 | 3 agents + verify | R15-1 (M-16 revert), R15-2 |
| 16 | 08-24 | ponytail cleanup pass, applied | dead code out, hygiene batch, VCS restored (commit 2ad83181) |

This file is tracked in git since round 16 (a8becc21). Earlier revisions live only in the fork
repository history.