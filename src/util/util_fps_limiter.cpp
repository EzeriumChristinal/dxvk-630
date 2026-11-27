#include <algorithm>
#include <thread>

#include "thread.h"
#include "util_env.h"
#include "util_fps_limiter.h"
#include "util_sleep.h"
#include "util_string.h"

#include "./log/log.h"

using namespace std::chrono_literals;

namespace dxvk {

  namespace {

    // Wait budgets. The slice step bounds how long any single sleep call
    // may run, so one mispredicted sleep costs at most a step rather than
    // the rest of the frame.
    constexpr int64_t SliceMarginNs = 150000;   // 0.15 ms
    constexpr int64_t SliceStepNs   = 1000000;  // 1 ms

    using TimePoint = dxvk::high_resolution_clock::time_point;
    using TimerDuration = std::chrono::nanoseconds;

    FpsLimitPacing pacingFromEnv(FpsLimitPacing fallback) {
      const std::string name = env::getEnvVar("DXVK_FRAME_RATE_PACING");

      if (name == "precise")
        return FpsLimitPacing::Precise;
      if (name == "sleep")
        return FpsLimitPacing::Sleep;
      if (name == "sliced")
        return FpsLimitPacing::Sliced;
      if (name == "spin")
        return FpsLimitPacing::Spin;
      return fallback;
    }

    FpsLimitMethod methodFromEnv(FpsLimitMethod fallback) {
      const std::string name = env::getEnvVar("DXVK_FRAME_RATE_METHOD");

      if (name == "deviation")
        return FpsLimitMethod::Deviation;
      if (name == "timeline")
        return FpsLimitMethod::Timeline;
      if (name == "reactive")
        return FpsLimitMethod::Reactive;
      return fallback;
    }

  }


  FpsLimiter::FpsLimiter() {
    m_pacing  = pacingFromEnv(FpsLimitPacing::Precise);
    m_method  = methodFromEnv(FpsLimitMethod::Deviation);
    m_hasTarget = false;
  }


  FpsLimiter::~FpsLimiter() {

  }


  void FpsLimiter::setTargetFrameRate(double frameRate, uint32_t maxLatency) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    TimerDuration interval = frameRate != 0.0
      ? TimerDuration(int64_t(double(TimerDuration::period::den) / frameRate))
      : TimerDuration::zero();

    if (m_targetInterval != interval) {
      m_targetInterval = interval;

      m_heuristicFrameTime = TimePoint();
      m_heuristicFrameCount = 0;
      m_heuristicEnable = false;

      m_maxLatency = maxLatency;

      m_hasTarget = interval != TimerDuration::zero();

      if (!std::exchange(m_heuristicEnable, false))
        Logger::warn("Built-in frame rate limiter enabled. Please enable an external"
                     " limiter instead in order to avoid poor frame pacing.");
    }
  }


  void FpsLimiter::delay() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    if (m_targetInterval == TimerDuration::zero()) {
      m_nextFrame = TimePoint();
      m_hasTarget = false;
      return;
    }

    auto t1 = dxvk::high_resolution_clock::now();

    // Snapshot frame deadline too: a concurrent delay() rewrites m_nextFrame
    // under the same mutex, so reading it unlocked is a race (R20-3).
    TimePoint nextFrame = m_nextFrame;
    lock.unlock();

    switch (m_method) {
      case FpsLimitMethod::Deviation:
        delayDeviation();
        break;

      case FpsLimitMethod::Timeline:
      case FpsLimitMethod::Reactive:
        delayTimeline();
        break;
    }

    lock.lock();
    m_nextFrame = nextTarget(t1);
    lock.unlock();
  }


  void FpsLimiter::delayDeviation() {
    auto t0 = m_nextFrame;
    auto t1 = dxvk::high_resolution_clock::now();

    auto interval = m_targetInterval;

    if (t1 < t0) {
      // Frame is ahead of schedule; sleep until the deadline.
      Sleep::sleepUntil(t1, t0);
    } else {
      // Frame is behind schedule; skip the sleep and reschedule from now.
      t0 = t1;
    }
  }


  void FpsLimiter::delayTimeline() {
    auto t1 = dxvk::high_resolution_clock::now();

    // Absolute cadence: advance by exactly one interval per frame,
    // resynchronising only after a frame that overran a whole interval.
    m_nextFrame = nextTarget(t1);
  }


  TimePoint FpsLimiter::nextTarget(TimePoint now) {
    if (!m_hasTarget)
      return TimePoint();

    return (now < m_nextFrame)
      ? m_nextFrame
      : now + m_targetInterval;
  }


  TimePoint FpsLimiter::sleepOnce(TimerDuration duration) {
    if (duration <= TimerDuration::zero())
      return dxvk::high_resolution_clock::now();

    auto t0 = dxvk::high_resolution_clock::now();
    return Sleep::sleepFor(t0, duration);
  }


  TimePoint FpsLimiter::sleepPrecise(TimePoint t0, TimerDuration duration) {
    if (duration <= TimerDuration::zero())
      return dxvk::high_resolution_clock::now();

    // Sleep coarsely for most of the duration, then busy-wait the last
    // stretch so we land as close to the deadline as possible.
    auto coarse = duration - TimerDuration(SliceMarginNs);
    if (coarse > TimerDuration::zero()) {
      auto t1 = dxvk::high_resolution_clock::now();
      if (t1 < t0 + coarse)
        Sleep::sleepUntil(t1, t0 + coarse);
    }

    auto t1 = dxvk::high_resolution_clock::now();
    auto deadline = t0 + duration;
    while (dxvk::high_resolution_clock::now() < deadline)
      ; // busy-wait the final margin

    return dxvk::high_resolution_clock::now();
  }


  TimePoint FpsLimiter::sleepSliced(TimePoint t0, TimerDuration duration) {
    if (duration <= TimerDuration::zero())
      return dxvk::high_resolution_clock::now();

    auto deadline = t0 + duration;
    auto now = dxvk::high_resolution_clock::now();

    while (now < deadline) {
      auto remaining = deadline - now;
      if (remaining <= TimerDuration(SliceStepNs)) {
        // Final slice: busy-wait the short remainder.
        while (dxvk::high_resolution_clock::now() < deadline)
          ;
        break;
      }

      auto slice = std::min(remaining - TimerDuration(SliceMarginNs), TimerDuration(SliceStepNs));
      if (slice > TimerDuration::zero()) {
        now = Sleep::sleepFor(now, slice);
      }

      now = dxvk::high_resolution_clock::now();
    }

    return now;
  }


  TimePoint FpsLimiter::spinUntil(TimePoint t0, TimerDuration duration) {
    auto deadline = t0 + duration;
    while (dxvk::high_resolution_clock::now() < deadline)
      ; // busy-wait the entire remainder
    return dxvk::high_resolution_clock::now();
  }


  bool FpsLimiter::testRefreshHeuristic(TimerDuration interval, TimePoint now, uint32_t maxLatency) {
    if (m_heuristicEnable)
      return true;

    constexpr static uint32_t MinWindowSize = 8;
    constexpr static uint32_t MaxWindowSize = 128;

    if (m_heuristicFrameCount >= MinWindowSize) {
      TimerDuration windowTotalTime = now - m_heuristicFrameTime;
      TimerDuration windowExpectedTime = m_heuristicFrameCount * interval;

      uint32_t minFrameCount = m_heuristicFrameCount - 1;
      uint32_t maxFrameCount = m_heuristicFrameCount + maxLatency;

      // Enable frame rate limiter if frames have been delivered faster than
      // the desired refresh rate even accounting for swap chain buffering.
      if ((maxFrameCount * windowTotalTime) < (m_heuristicFrameCount * windowExpectedTime)) {
        double got = (double(m_heuristicFrameCount) * double(TimerDuration::period::den))
                   / (double(windowTotalTime.count()) * double(TimerDuration::period::num));
        double refresh = double(TimerDuration::period::den) / (double(TimerDuration::period::num) * double(interval.count()));

        Logger::info(str::format("Detected frame rate (~", uint32_t(got), ") higher than selected refresh rate of ~",
          uint32_t(refresh), " Hz.\n", "Engaging frame rate limiter."));

        m_heuristicEnable = true;
        return true;
      }

      // Reset heuristics if frames have been delivered slower than the refresh rate.
      if (((minFrameCount * windowTotalTime) > (m_heuristicFrameCount * windowExpectedTime))
       || (m_heuristicFrameCount >= MaxWindowSize)) {
        m_heuristicFrameCount = 1;
        m_heuristicFrameTime = now;
        return false;
      }
    }

    if (!m_heuristicFrameCount)
      m_heuristicFrameTime = now;

    m_heuristicFrameCount += 1;
    return false;
  }

}
