#pragma once
#include "dxvk_include.h"
#include "helios_producer_abi.h"
#include "dxvk_fence.h"
#include <atomic>

namespace dxvk {
  class HeliosProducerBinding : public RcObject {
  public:
    HeliosProducerBinding(VkDevice device, PFN_vkGetSemaphoreCounterValue deviceEntryPoint,
      uint32_t allocation);
    ~HeliosProducerBinding();
    VkResult sample(helios_producer_snapshot& snapshot) const;
    VkResult wait(uint64_t epoch, uint64_t timeoutNs) const;
    bool publish(VkSemaphore semaphore, uint64_t value, uint64_t* epoch) const;
    bool registerStream(VkDevice device, VkSemaphore semaphore, uint32_t* ctx, uint64_t* cookie) const {
      return m_api.stream(reinterpret_cast<uintptr_t>(device), uint64_t(uintptr_t(semaphore)), ctx, cookie) == VK_SUCCESS;
    }
    uint64_t generation() const { return m_generation; }
    void abort() const { m_api.abort(m_binding); }
    static void noteGateFlush() { s_gateFlushes.fetch_add(1, std::memory_order_relaxed); }
    static uint64_t gateFlushCount() { return s_gateFlushes.load(std::memory_order_relaxed); }
  private:
    helios_producer_api_v1 m_api = { };
    void* m_binding = nullptr;
    uint64_t m_generation = 0;
    inline static std::atomic<uint64_t> s_gateFlushes = 0;
  };

  struct HeliosProducerDependency {
    Rc<HeliosProducerBinding> binding;
    uint64_t epoch = 0;
    Rc<DxvkFence> externalSemaphore; // Explicit WSI dependency, retained through the copy.
  };

  // Retained by the CS closure and command list. Dropping committed work
  // before its exact signal was submitted must wake consumers as failure.
  class HeliosProducerOperation : public RcObject {
  public:
    explicit HeliosProducerOperation(Rc<HeliosProducerBinding> binding)
    : m_binding(std::move(binding)) { }
    ~HeliosProducerOperation() { if (!m_submitted) m_binding->abort(); }
    void submitted(bool success) {
      if (success) m_submitted = true;
      else m_binding->abort();
    }
  private:
    Rc<HeliosProducerBinding> m_binding;
    std::atomic<bool> m_submitted = false;
  };
}
