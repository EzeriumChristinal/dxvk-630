#include <algorithm>
#include <atomic>
#include <utility>

#include "dxvk_compute.h"
#include "dxvk_device.h"
#include "dxvk_graphics.h"
#include "dxvk_pipecompiler.h"

namespace dxvk {

  namespace {

    constexpr uint32_t DYASYNC_MIN_WORKERS         = 1;
    constexpr uint32_t DYASYNC_MAX_WORKERS         = 32;
    constexpr uint32_t DYASYNC_MAX_WORKERS_32BIT   = 16;
    constexpr uint32_t DYASYNC_BACKGROUND_INTERVAL = 8;

    uint32_t resolve_worker_count(
            uint32_t cpu_cores,
            int32_t  config_thread_count,
            bool     is_32bit,
            bool     use_all_cores) {
      if (use_all_cores)
        return cpu_cores;

      if (config_thread_count > 0)
        return uint32_t(config_thread_count);

      uint32_t count = std::clamp(dyasyncBaseWorkers(cpu_cores),
        DYASYNC_MIN_WORKERS, DYASYNC_MAX_WORKERS);
      return is_32bit ? std::min(count, DYASYNC_MAX_WORKERS_32BIT) : count;
    }

    bool prefer_background(uint32_t pick) {
      return (pick % DYASYNC_BACKGROUND_INTERVAL) == 0;
    }

    DxvkPipelineEntry pop_front(std::deque<DxvkPipelineEntry>& queue) {
      DxvkPipelineEntry entry = std::move(queue.front());
      queue.pop_front();
      return entry;
    }

  }


  DxvkPipelineCompiler::DxvkPipelineCompiler(const DxvkDevice* device) {
    uint32_t numWorkers = resolve_worker_count(
      dxvk::thread::hardware_concurrency(),
      device->config().numDyasyncThreads,
      env::is32BitHostPlatform(),
      env::getEnvVar("DXVK_ALL_CORES") == "1");

    Logger::info(str::format("DXVK: Using ", numWorkers, " dyasync compiler threads"));

    this->spawnWorkers(numWorkers);
  }


  DxvkPipelineCompiler::~DxvkPipelineCompiler() {
    this->requestStop();
    this->joinAll();
  }


  bool DxvkPipelineCompiler::queueCompilation(
          DxvkGraphicsPipeline*          pipeline,
    const DxvkGraphicsPipelineStateInfo& state,
          DxvkPipelinePriority           priority) {
    return this->pushEntry(DxvkPipelineEntry {
      pipeline,
      state,
      nullptr,
      DxvkComputePipelineStateInfo(),
      priority,
    });
  }


  void DxvkPipelineCompiler::stop() {
    this->requestStop();
    this->joinAll();
  }


  void DxvkPipelineCompiler::spawnWorkers(uint32_t count) {
    m_workers.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
      auto& worker = m_workers.emplace_back([this] { this->runCompilerThread(); });
      worker.set_priority(ThreadPriority::Lowest);
    }
  }


  void DxvkPipelineCompiler::requestStop() {
    { std::lock_guard<std::mutex> lock(m_mutex);
      m_stop = true; }

    m_cond.notify_all();
  }


  void DxvkPipelineCompiler::joinAll() {
    // join() blocks until the thread exits and marks it unjoinable, so a
    // second pass (e.g. stop() followed by the compiler destructor running
    // joinAll again) must not re-join already-joined threads - that would
    // throw std::system_error and terminate.
    for (dxvk::thread& worker : m_workers) {
      if (worker.joinable())
        worker.join();
    }
  }


  std::deque<DxvkPipelineEntry>& DxvkPipelineCompiler::laneFor(DxvkPipelinePriority priority) {
    return priority == DxvkPipelinePriority::Low
      ? m_backgroundQueue
      : m_liveQueue;
  }


  bool DxvkPipelineCompiler::pushEntry(DxvkPipelineEntry&& entry) {
    std::deque<DxvkPipelineEntry>& queue = this->laneFor(entry.priority);

    std::lock_guard<std::mutex> lock(m_mutex);

    if (entry.pipeline) {
      if (!m_queuedGraphicsPipelines.insert(
            QueueKey<DxvkGraphicsPipeline> { entry.pipeline, entry.state.hash() }).second)
        return false;
    } else if (entry.computePipeline) {
      if (!m_queuedComputePipelines.insert(
            QueueKey<DxvkComputePipeline> { entry.computePipeline, entry.computeState.hash() }).second)
        return false;
    }

    // Roll back the dedupe key if the push throws, else a leaked key
    // would permanently suppress this state's optimized compile.
    // deque::push_back has a strong guarantee: entry is intact on throw.
    try {
      queue.push_back(std::move(entry));
    } catch (...) {
      if (entry.pipeline)
        m_queuedGraphicsPipelines.erase(
          QueueKey<DxvkGraphicsPipeline> { entry.pipeline, entry.state.hash() });
      else if (entry.computePipeline)
        m_queuedComputePipelines.erase(
          QueueKey<DxvkComputePipeline> { entry.computePipeline, entry.computeState.hash() });
      throw;
    }

    m_cond.notify_one();
    return true;
  }


  void DxvkPipelineCompiler::removePipeline(DxvkGraphicsPipeline* pipeline) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto i = m_queuedGraphicsPipelines.begin(); i != m_queuedGraphicsPipelines.end(); ) {
      if (i->pipeline == pipeline)
        i = m_queuedGraphicsPipelines.erase(i);
      else
        i++;
    }

    auto pred = [pipeline](const DxvkPipelineEntry& e) { return e.pipeline == pipeline; };
    m_liveQueue.erase(std::remove_if(m_liveQueue.begin(), m_liveQueue.end(), pred),
      m_liveQueue.end());
    m_backgroundQueue.erase(std::remove_if(m_backgroundQueue.begin(), m_backgroundQueue.end(), pred),
      m_backgroundQueue.end());
  }


  void DxvkPipelineCompiler::removePipeline(DxvkComputePipeline* pipeline) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto i = m_queuedComputePipelines.begin(); i != m_queuedComputePipelines.end(); ) {
      if (i->pipeline == pipeline)
        i = m_queuedComputePipelines.erase(i);
      else
        i++;
    }

    auto pred = [pipeline](const DxvkPipelineEntry& e) { return e.computePipeline == pipeline; };
    m_liveQueue.erase(std::remove_if(m_liveQueue.begin(), m_liveQueue.end(), pred),
      m_liveQueue.end());
    m_backgroundQueue.erase(std::remove_if(m_backgroundQueue.begin(), m_backgroundQueue.end(), pred),
      m_backgroundQueue.end());
  }


  bool DxvkPipelineCompiler::takeEntry(DxvkPipelineEntry& entry, bool preferBackground) {
    if (!(preferBackground && !m_backgroundQueue.empty()) && !m_liveQueue.empty()) {
      entry = pop_front(m_liveQueue);
    } else if (!m_backgroundQueue.empty()) {
      entry = pop_front(m_backgroundQueue);
    } else {
      return false;
    }

    return true;
  }


  bool DxvkPipelineCompiler::waitForWork(DxvkPipelineEntry& entry) {
    static std::atomic<uint32_t> s_next_pick = 1;
    thread_local uint32_t s_pick = s_next_pick.fetch_add(1, std::memory_order_relaxed);
    thread_local uint32_t s_starvation_counter = 0;
    uint32_t pick = s_pick++;

    bool preferBg = prefer_background(pick)
                 || s_starvation_counter >= DYASYNC_BACKGROUND_INTERVAL * 3;

    std::unique_lock<std::mutex> lock(m_mutex);

    m_cond.wait(lock, [this] {
      return m_stop || !m_liveQueue.empty() || !m_backgroundQueue.empty();
    });

    if (m_stop)
      return false;

    if (!this->takeEntry(entry, preferBg))
      return false;

    if (entry.priority != DxvkPipelinePriority::Low && !m_backgroundQueue.empty())
      s_starvation_counter++;
    else
      s_starvation_counter = 0;

    return true;
  }


  void DxvkPipelineCompiler::compileAndCache(const DxvkPipelineEntry& entry) {
    if (entry.pipeline)
      entry.pipeline->compilePipeline(entry.state);
    else if (entry.computePipeline)
      entry.computePipeline->compilePipeline(entry.computeState);

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (entry.pipeline)
        m_queuedGraphicsPipelines.erase(
          QueueKey<DxvkGraphicsPipeline> { entry.pipeline, entry.state.hash() });
      else if (entry.computePipeline)
        m_queuedComputePipelines.erase(
          QueueKey<DxvkComputePipeline> { entry.computePipeline, entry.computeState.hash() });
    }
  }


  void DxvkPipelineCompiler::runCompilerThread() {
    env::setThreadName("dxvk-dyasync");

    for (DxvkPipelineEntry entry; this->waitForWork(entry); ) {
      if (entry.pipeline || entry.computePipeline)
        this->compileAndCache(entry);
    }
  }

}
