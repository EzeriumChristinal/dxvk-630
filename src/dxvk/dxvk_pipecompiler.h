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

  void removePipeline(DxvkGraphicsPipeline* pipeline);

  void removePipeline(DxvkComputePipeline* pipeline);

  private:

    std::mutex                    m_mutex;
    std::condition_variable       m_cond;
    bool                          m_stop = false;

    std::deque<DxvkPipelineEntry> m_liveQueue;
    std::deque<DxvkPipelineEntry> m_backgroundQueue;

    std::unordered_set<DxvkGraphicsPipeline*> m_queuedGraphicsPipelines;
    std::unordered_set<DxvkComputePipeline*>  m_queuedComputePipelines;

    std::vector<dxvk::thread>     m_workers;

    void spawnWorkers(uint32_t count);

    void requestStop();

    void joinAll();

    std::deque<DxvkPipelineEntry>& laneFor(DxvkPipelinePriority priority);

    bool pushEntry(DxvkPipelineEntry&& entry);

    void notifyIfPushed(bool pushed);

    bool takeEntry(DxvkPipelineEntry& entry, bool preferBackground);

    bool waitForWork(DxvkPipelineEntry& entry);

    void compileAndCache(const DxvkPipelineEntry& entry);

    void processEntry(const DxvkPipelineEntry& entry);

    void runCompilerThread();

  };

}
