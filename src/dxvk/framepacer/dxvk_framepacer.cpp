#include "dxvk_framepacer.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <thread>

#include "../../util/log/log.h"
#include "../../util/util_env.h"
#include "../../util/util_string.h"

#include "../dxvk_options.h"

namespace dxvk {

  namespace {

    constexpr const char* FRAME_PACE_ENV_VAR = "DXVK_FRAME_PACE";

    constexpr uint32_t MIN_FRAME_LATENCY = 1u;

    constexpr int32_t LOW_LATENCY_OFFSET_MIN_US = -10000;
    constexpr int32_t LOW_LATENCY_OFFSET_MAX_US = 10000;

    constexpr int32_t FRAME_DURATION_SMOOTHING_OLD_WEIGHT   = 3;
    constexpr int32_t FRAME_DURATION_SMOOTHING_NEW_WEIGHT   = 1;
    constexpr int32_t FRAME_DURATION_SMOOTHING_TOTAL_WEIGHT =
      FRAME_DURATION_SMOOTHING_OLD_WEIGHT + FRAME_DURATION_SMOOTHING_NEW_WEIGHT;

    struct FramePaceName {
      std::string_view name;
      DxvkFramePace    mode;
    };

    constexpr std::array<FramePaceName, 2> FRAME_PACE_NAMES = {{
      { "min-latency", DxvkFramePace::MinLatency },
      { "low-latency", DxvkFramePace::LowLatency },
    }};


    DxvkFramePace parse_frame_pace(std::string_view configured) {
      auto entry = std::find_if(FRAME_PACE_NAMES.begin(), FRAME_PACE_NAMES.end(),
        [configured] (FramePaceName candidate) {
          return configured.find(candidate.name) != std::string_view::npos;
        });

      return entry != FRAME_PACE_NAMES.end() ? entry->mode : DxvkFramePace::MaxFrameLatency;
    }


    int32_t clamp_low_latency_offset(int32_t offsetUs) {
      return std::clamp(offsetUs, LOW_LATENCY_OFFSET_MIN_US, LOW_LATENCY_OFFSET_MAX_US);
    }


    int32_t blend_frame_duration(int32_t previousAvgUs, int32_t latestDurationUs) {
      return previousAvgUs == 0
        ? latestDurationUs
        : (previousAvgUs * FRAME_DURATION_SMOOTHING_OLD_WEIGHT
             + latestDurationUs * FRAME_DURATION_SMOOTHING_NEW_WEIGHT)
          / FRAME_DURATION_SMOOTHING_TOTAL_WEIGHT;
    }


    std::optional<std::chrono::high_resolution_clock::time_point> predict_wake_time(
            DxvkFramePace                                     mode,
            std::optional<std::chrono::high_resolution_clock::time_point>  lastFrameStart,
            int32_t                                            avgFrameDurationUs,
            int32_t                                            offsetUs) {
      switch (mode) {
        case DxvkFramePace::MaxFrameLatency: return std::nullopt;
        case DxvkFramePace::MinLatency:      return std::nullopt;
        case DxvkFramePace::LowLatency:                                break;
      }

      return lastFrameStart.has_value() && avgFrameDurationUs > 0
        ? std::optional(*lastFrameStart + std::chrono::microseconds(avgFrameDurationUs + offsetUs))
        : std::nullopt;
    }

  }


  FramePacer::FramePacer(const DxvkOptions& options, uint64_t firstFrameId)
  : m_lowLatencyOffsetUs(clamp_low_latency_offset(options.lowLatencyOffset)) {
    auto envValue = env::getEnvVar(FRAME_PACE_ENV_VAR);

    m_mode = parse_frame_pace(envValue.empty() ? options.framePace : envValue);

    switch (m_mode) {
      case DxvkFramePace::MaxFrameLatency:
        break;

      case DxvkFramePace::MinLatency:
        Logger::info("Frame pace: min-latency (frame latency forced to 1)");
        break;

      case DxvkFramePace::LowLatency:
        Logger::info(str::format("Frame pace: low-latency (frame latency forced to 1, offset ",
          m_lowLatencyOffsetUs, " us)"));
        m_signal = new sync::CallbackFence(firstFrameId);
        break;
    }
  }


  uint32_t FramePacer::getEffectiveFrameLatency(uint32_t configuredLatency) const {
    switch (m_mode) {
      case DxvkFramePace::MaxFrameLatency: return configuredLatency;
      case DxvkFramePace::LowLatency:      return std::min(configuredLatency, MIN_FRAME_LATENCY);
      case DxvkFramePace::MinLatency:      return std::min(configuredLatency, MIN_FRAME_LATENCY);
    }

    return configuredLatency;
  }


  void FramePacer::beginFrame() {
    auto wakeTime = predict_wake_time(m_mode, m_lastFrameStart,
      m_avgFrameDurationUs.load(), m_lowLatencyOffsetUs);

    if (wakeTime.has_value())
      std::this_thread::sleep_until(*wakeTime);

    m_lastFrameStart = std::chrono::high_resolution_clock::now();
  }


  void FramePacer::endFrame(uint64_t frameId) {
    if (m_mode != DxvkFramePace::LowLatency)
      return;

    auto frameStart = *m_lastFrameStart;

    m_signal->setCallback(frameId, [this, frameStart] () { recordFrameDuration(frameStart); });
  }


  void FramePacer::recordFrameDuration(std::chrono::high_resolution_clock::time_point frameStart) {
    auto elapsedUs = int32_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::high_resolution_clock::now() - frameStart).count());

    m_avgFrameDurationUs.store(blend_frame_duration(m_avgFrameDurationUs.load(), elapsedUs));
  }

}
