#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

#include "../../util/sync/sync_signal.h"

namespace dxvk {

  struct DxvkOptions;

  enum class DxvkFramePace : uint32_t {
    MaxFrameLatency = 0,
    LowLatency      = 1,
    MinLatency      = 2,
  };

  class FramePacer {

  public:

    FramePacer(const DxvkOptions& options, uint64_t firstFrameId);

    uint32_t getEffectiveFrameLatency(uint32_t configuredLatency) const;

    bool needsGpuSignal() const { return m_mode == DxvkFramePace::LowLatency; }

    const Rc<sync::CallbackFence>& signal() const { return m_signal; }

    void beginFrame();

    void endFrame(uint64_t frameId);

  private:

    void recordFrameDuration(std::chrono::high_resolution_clock::time_point frameStart);

    DxvkFramePace m_mode               = DxvkFramePace::MaxFrameLatency;
    int32_t       m_lowLatencyOffsetUs = 0;

    std::optional<std::chrono::high_resolution_clock::time_point> m_lastFrameStart     = std::nullopt;
    std::atomic<int32_t>                                          m_avgFrameDurationUs = { 0 };

    Rc<sync::CallbackFence> m_signal;

  };

}
