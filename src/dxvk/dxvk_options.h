#pragma once

#include "../util/config/config.h"
#include "../util/util_env.h"

#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config);

    /// Enable debug utils
    bool enableDebugUtils = false;

    /// Enable memory defragmentation
    Tristate enableMemoryDefrag = Tristate::Auto;

    /// Number of compiler threads
    /// when using the state cache
    int32_t numCompilerThreads = 0;

    /// Enable graphics pipeline library
    Tristate enableGraphicsPipelineLibrary = Tristate::Auto;

    /// Enable descriptor heap
    Tristate enableDescriptorHeap = Tristate::Auto;

    /// Enable descriptor buffer
    Tristate enableDescriptorBuffer = Tristate::Auto;

    /// Enable unified image layout path
    bool enableUnifiedImageLayout = true;

    /// Enables pipeline lifetime tracking
    Tristate trackPipelineLifetime = Tristate::Auto;

    /// Shader-related options
    Tristate useRawSsbo = Tristate::Auto;

    /// HUD elements
    std::string hud;

    /// Forces swap chain into MAILBOX (if true)
    /// or FIFO_RELAXED (if false) present mode
    Tristate tearFree = Tristate::Auto;

    /// Enables latency sleep
    Tristate latencySleep = Tristate::Auto;

    /// Latency tolerance, in microseconds
    int32_t latencyTolerance = 1000;

    /// Disable VK_NV_low_latency2. This extension
    /// appears to be all sorts of broken on 32-bit.
    Tristate disableNvLowLatency2 = Tristate::Auto;

    // Hides integrated GPUs if dedicated GPUs are
    // present. May be necessary for some games that
    // incorrectly assume monitor layouts.
    bool hideIntegratedGraphics = false;

    /// Clears all mapped memory to zero.
    bool zeroMappedMemory = false;

    /// Allows full-screen exclusive mode on Windows
    bool allowFse = false;

    /// Whether to enable tiler optimizations
    Tristate tilerMode = Tristate::Auto;

    /// Overrides memory budget for DXVK
    VkDeviceSize maxMemoryBudget = 0u;

    /// Whether to use custom sin/cos approximation
    Tristate lowerSinCos = Tristate::Auto;

    /// Enables implicit resolves that are used to
    /// deal with MSAA-related undefined behaviour.
    bool enableImplicitResolves = true;

    /// Enables NV_raw_access_chains extension on Nvidia
    bool enableNvRawAccessChains = true;

    /// Enable descriptor update templates
    bool enableDescriptorUpdateTemplates = true;

    /// Device name
    std::string deviceFilter;

    /// Enable Gen9 low-power profile (auto-set by device detect)
    bool enableGen9Profile = false;

    /// Enable async pipeline compilation (dyasync)
    bool enableDyasync = false;

    /// Number of async compiler threads (0 = auto)
    int32_t numDyasyncThreads = 0;

    /// Frame pacing mode. Supported values: "", "low-latency", "min-latency".
    std::string framePace;

    /// Fine-tuning offset for low-latency frame pacing, in microseconds.
    int32_t lowLatencyOffset = 0;
  };

}
