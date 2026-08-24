#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "../util/thread.h"

#include "dxvk_include.h"

namespace dxvk {

  class DxvkComputePipeline;
  class DxvkDevice;
  class DxvkGraphicsPipeline;
  struct DxvkComputePipelineStateInfo;
  struct DxvkGraphicsPipelineStateInfo;
  enum class DxvkPipelinePriority : uint32_t;

  struct DxvkPipelineEntry {
    DxvkGraphicsPipeline*             pipeline;
    DxvkGraphicsPipelineStateInfo     state;
    DxvkComputePipeline*              computePipeline = nullptr;
    DxvkComputePipelineStateInfo      computeState;
    DxvkPipelinePriority              priority;
  };

    // Dyasync worker scaling: (cores - 1) * 5 / 7. Shared with the
    // pipeline manager's normal-priority worker split.
  inline uint32_t dyasyncBaseWorkers(uint32_t cpuCores) {
    return ((std::max(1u, cpuCores) - 1u) * 5u) / 7u;
  }

  class DxvkPipelineCompiler : public RcObject {

  public:

    template<typename P>
    struct QueueKey {
      P*     pipeline  = nullptr;
      size_t stateHash = 0;

      bool operator==(const QueueKey& other) const {
        return pipeline == other.pipeline && stateHash == other.stateHash;
      }
    };

    struct QueueKeyHash {
      template<typename P>
      size_t operator()(const QueueKey<P>& key) const {
        return size_t(uintptr_t(key.pipeline)) ^ key.stateHash;
      }
    };

    explicit DxvkPipelineCompiler(const DxvkDevice* device);

    ~DxvkPipelineCompiler();

  bool queueCompilation(
          DxvkGraphicsPipeline*            pipeline,
    const DxvkGraphicsPipelineStateInfo& state,
          DxvkPipelinePriority           priority);

  void stop();

  void removePipeline(DxvkGraphicsPipeline* pipeline);

  void removePipeline(DxvkComputePipeline* pipeline);

  private:

    std::mutex                    m_mutex;
    std::condition_variable       m_cond;
    bool                          m_stop = false;

    std::deque<DxvkPipelineEntry> m_liveQueue;
    std::deque<DxvkPipelineEntry> m_backgroundQueue;

    std::unordered_set<QueueKey<DxvkGraphicsPipeline>, QueueKeyHash> m_queuedGraphicsPipelines;
    std::unordered_set<QueueKey<DxvkComputePipeline>,  QueueKeyHash> m_queuedComputePipelines;

    std::vector<dxvk::thread>     m_workers;

    void spawnWorkers(uint32_t count);

    void requestStop();

    void joinAll();

    std::deque<DxvkPipelineEntry>& laneFor(DxvkPipelinePriority priority);

    bool pushEntry(DxvkPipelineEntry&& entry);

    bool takeEntry(DxvkPipelineEntry& entry, bool preferBackground);

    bool waitForWork(DxvkPipelineEntry& entry);

    void compileAndCache(const DxvkPipelineEntry& entry);

    void runCompilerThread();

  };

}
