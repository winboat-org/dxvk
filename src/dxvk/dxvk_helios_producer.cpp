#include "dxvk_helios_producer.h"

namespace dxvk {
  HeliosProducerBinding::HeliosProducerBinding(VkDevice device,
      PFN_vkGetSemaphoreCounterValue deviceEntryPoint, uint32_t allocation) {
#ifdef _WIN32
    // The installer uses content-hashed ICD names. Resolve the module from
    // this device's dispatch, whose lifetime is pinned by the live device.
    // An intervening layer without the interface fails explicitly.
    HMODULE module = nullptr;
    if (deviceEntryPoint)
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(deviceEntryPoint), &module);
    const auto get = module ? reinterpret_cast<helios_get_producer_api_fn>(
      GetProcAddress(module, "helios_venus_producer_interface")) : nullptr;
    if (!get || get(HELIOS_PRODUCER_ABI, &m_api) != VK_SUCCESS
     || m_api.version != HELIOS_PRODUCER_ABI || m_api.size != sizeof(m_api)
     || m_api.bind(reinterpret_cast<uintptr_t>(device), allocation, &m_binding) != VK_SUCCESS)
      throw DxvkError("Helios: exact allocation producer binding failed");
    helios_producer_snapshot snapshot = { };
    if (m_api.status(m_binding, &snapshot) != VK_SUCCESS) {
      m_api.release(m_binding);
      m_binding = nullptr;
      throw DxvkError("Helios: producer binding has no stable live status");
    }
    m_generation = snapshot.generation;
#else
    (void)device; (void)deviceEntryPoint; (void)allocation;
    throw DxvkError("Helios: allocation producer binding requires Windows");
#endif
  }

  HeliosProducerBinding::~HeliosProducerBinding() {
    if (m_binding) m_api.release(m_binding);
  }

  VkResult HeliosProducerBinding::sample(helios_producer_snapshot& snapshot) const {
    return VkResult(m_api.status(m_binding, &snapshot));
  }

  VkResult HeliosProducerBinding::wait(uint64_t epoch, uint64_t timeoutNs) const {
    return VkResult(m_api.wait(m_binding, epoch, timeoutNs, 0));
  }

  bool HeliosProducerBinding::publish(VkSemaphore semaphore, uint64_t value, uint64_t* epoch) const {
    return m_api.publish(m_binding, uint64_t(uintptr_t(semaphore)), value, epoch) == VK_SUCCESS;
  }
}
