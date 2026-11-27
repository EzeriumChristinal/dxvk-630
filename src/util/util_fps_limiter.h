#pragma once

#include <array>
#include <optional>

#include "thread.h"
#include "util_time.h"

namespace dxvk {

  /**
   * \brief Frame pacing mode
   *
   * How the limiter waits out the remainder of a frame.
   *
   * \li \c Precise sleeps coarsely, then busy-waits the last stretch.
   *     Most accurate, and keeps a core hot every frame.
   * \li \c Sleep issues one sleep for the whole remainder. Cheapest, and
   *     only as accurate as the platform timer.
   * \li \c Sliced sleeps in bounded steps, re-measuring after each, then
   *     busy-waits a short final margin.
   * \li \c Spin busy-waits the entire remainder.
   *
   * Where the game is CPU-bound the busy-wait competes with it for a core
   * it needs, which is why the cheaper modes are worth having even though
   * they pace less tightly.
   */
  enum class FpsLimitPacing : uint32_t {
    Precise = 0,
    Sleep   = 1,
    Sliced  = 2,
    Spin    = 3,
  };

  /**
   * \brief Frame rate limiter method
   *
   * How the deadline for the next frame is derived.
   *
   * \li \c Deviation carries a correction term across frames so that
   *     sleep inaccuracy averages out.
   * \li \c Timeline holds an absolute cadence, advancing by exactly one
   *     interval per frame and resynchronising only after a frame that
   *     overran a whole interval.
   * \li \c Reactive measures each interval from the frame just presented
   *     and never tries to catch up.
   */
  enum class FpsLimitMethod : uint32_t {
    Deviation = 0,
    Timeline  = 1,
    Reactive  = 2,
  };

  /**
   * \brief Frame rate limiter
   *
   * Provides functionality to stall an application
   * thread in order to maintain a given frame rate.
   */
  class FpsLimiter {

  public:

    /**
     * \brief Creates frame rate limiter
     */
    FpsLimiter();

    ~FpsLimiter();

    /**
     * \brief Sets target frame rate
     * \param [in] frameRate Target frame rate
     */
    void setTargetFrameRate(double frameRate, uint32_t maxLatency);

    /**
     * \brief Stalls calling thread as necessary
     *
     * Blocks the calling thread if the limiter is enabled
     * and the time since the last call to \ref delay is
     * shorter than the target interval.
     */
    void delay();

  private:

    using TimePoint = dxvk::high_resolution_clock::time_point;
    using TimerDuration = std::chrono::nanoseconds;

    dxvk::mutex     m_mutex;

    TimerDuration   m_targetInterval  = TimerDuration::zero();
    TimePoint       m_nextFrame       = TimePoint();
    uint32_t        m_maxLatency      = 0;

    FpsLimitPacing  m_pacing  = FpsLimitPacing::Precise;
    FpsLimitMethod  m_method  = FpsLimitMethod::Deviation;
    bool            m_hasTarget = false;

    uint32_t        m_heuristicFrameCount = 0;
    TimePoint       m_heuristicFrameTime  = TimePoint();
    bool            m_heuristicEnable     = false;

    bool testRefreshHeuristic(TimerDuration interval, TimePoint now, uint32_t maxLatency);

    void delayDeviation();

    void delayTimeline();

    TimePoint nextTarget(TimePoint now);

    TimePoint sleepOnce(TimerDuration duration);

    TimePoint sleepPrecise(TimePoint t0, TimerDuration duration);

    TimePoint sleepSliced(TimePoint t0, TimerDuration duration);

    TimePoint spinUntil(TimePoint t0, TimerDuration duration);

  };

}
