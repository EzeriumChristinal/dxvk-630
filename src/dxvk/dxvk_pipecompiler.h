#pragma once

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

  class DxvkPipelineCompiler : public RcObject {

  public:

    struct GraphicsQueueKey {
      DxvkGraphicsPipeline* pipeline = nullptr;
      size_t                stateHash = 0;

      bool operator==(const GraphicsQueueKey& other) const {
        return pipeline == other.pipeline && stateHash == other.stateHash;
      }
    };

    struct GraphicsQueueKeyHash {
      size_t operator()(const GraphicsQueueKey& key) const {
        return size_t(uintptr_t(key.pipeline)) ^ key.stateHash;
      }
    };

    struct ComputeQueueKey {
      DxvkComputePipeline* pipeline = nullptr;
      size_t               stateHash = 0;

      bool operator==(const ComputeQueueKey& other) const {
        return pipeline == other.pipeline && stateHash == other.stateHash;
      }
    };

    struct ComputeQueueKeyHash {
      size_t operator()(const ComputeQueueKey& key) const {
        return size_t(uintptr_t(key.pipeline)) ^ key.stateHash;
      }
    };

    explicit DxvkPipelineCompiler(const DxvkDevice* device);

    ~DxvkPipelineCompiler();

  bool queueCompilation(
          DxvkGraphicsPipeline*            pipeline,
    const DxvkGraphicsPipelineStateInfo& state,
          DxvkPipelinePriority           priority);

  bool queueCompilation(
          DxvkComputePipeline*             pipeline,
    const DxvkComputePipelineStateInfo&  state,
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

    std::unordered_set<GraphicsQueueKey, GraphicsQueueKeyHash> m_queuedGraphicsPipelines;
    std::unordered_set<ComputeQueueKey,  ComputeQueueKeyHash>  m_queuedComputePipelines;

    std::vector<dxvk::thread>     m_workers;

    void spawnWorkers(uint32_t count);

    void requestStop();

    void joinAll();

    std::deque<DxvkPipelineEntry>& laneFor(DxvkPipelinePriority priority);

    bool pushEntry(DxvkPipelineEntry&& entry);

    bool takeEntry(DxvkPipelineEntry& entry, bool preferBackground);

    bool waitForWork(DxvkPipelineEntry& entry);

    void compileAndCache(const DxvkPipelineEntry& entry);

    void processEntry(const DxvkPipelineEntry& entry);

    void runCompilerThread();

  };

}
