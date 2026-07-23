#pragma once

#include <atomic>

#include "../util_likely.h"

namespace dxvk {
  
  /**
   * \brief Reference-counted object
   */
  class RcObject {
    
  public:
    
    /**
     * \brief Increments reference count
     * \returns New reference count
     */
    force_inline uint32_t incRef() {
      return m_refCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    
    /**
     * \brief Decrements reference count
     * \returns New reference count
     */
    force_inline uint32_t decRef() {
      return m_refCount.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    }
    
  private:
    
    std::atomic<uint32_t> m_refCount = { 0u };
    
  };
  
}