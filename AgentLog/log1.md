Status: 17/24 tasks done, 7 remaining (T6.1-T6.4 optional, T7.1-T7.3 blocked).
Files modified/created:
src/dxvk/dxvk_instance.h              — VK_API_VERSION_1_3 → 1_2
src/dxvk/dxvk_device_info.cpp         — 17 feature flags required→false
src/dxvk/dxvk_adapter.h               — +isGen9LowPower() decl
src/dxvk/dxvk_adapter.cpp             — +isGen9LowPower() impl (ID ranges)
src/dxvk/dxvk_options.h               — +enableGen9Profile, enableDyasync, numDyasyncThreads
src/dxvk/dxvk_options.cpp             — +config parsing for dyasync fields
src/dxvk/dxvk_device.cpp              — +Gen9 profile override block
src/dxvk/dxvk_pipecompiler.h [NEW]    — DxvkPipelineCompiler class (Sarek port)
src/dxvk/dxvk_pipecompiler.cpp [NEW]  — dual-queue worker pool (adapted for mainline)
src/dxvk/dxvk_graphics.h              — +fallback map structs, m_compiler, methods
src/dxvk/dxvk_graphics.cpp            — +fallback functions, modified getPipelineHandle
src/dxvk/dxvk_pipemanager.h           — +m_compiler member, pipecompiler include
src/dxvk/dxvk_pipemanager.cpp         — +compiler creation in constructor
src/dxvk/dxvk/meson.build             — +dxvk_pipecompiler.cpp source
Key architectural decisions during port:
- entry_is_valid relaxed: null renderPass allowed (mainline uses dynamic rendering)
- compileAndCache calls compilePipeline(entry.state) only (mainline has no writePipelineStateToCache)
- Dyasync fallback returns base pipeline from getBasePipeline() instead of renderPass-based fallback
- DxvkPipelineCompiler adapted to return void from compilePipeline
Blockers for Phase 7: Need mingw-w64 cross-compiler + meson installed. Recommend WSL2 with sudo apt install g++-mingw-w64-x86-64 meson, or alternatively setup MSYS2 with pacman -S mingw-w64-x86_64-meson mingw-w64-x86_64-gcc.
Risk flag: synchronization2 made optional (T1.3) but code uses VK_PIPELINE_STAGE_2_* constants pervasively. Safe on Gen9 (2025+ Vulkan 1.3 driver), but would crash on Vulkan 1.2-only driver if it advertises 1.2 without sync2. Plan: config option dxvk.forceVulkan12 to skip sync2 usage for testing.