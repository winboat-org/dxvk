#pragma once

#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "dxvk_include.h"
#include "dxvk_adapter.h"

#include "../util/thread.h"

namespace dxvk {

  class DxvkDevice;
  class DxvkFence;

  using DxvkFenceEvent = std::function<void ()>;

  /**
   * \brief Fence create info
   */
  struct DxvkFenceCreateInfo {
    uint64_t        initialValue;
    VkExternalSemaphoreHandleTypeFlagBits sharedType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
    union {
      // When we want to implement this on non-Windows platforms,
      // we could add a `int fd` here, etc.
      HANDLE          sharedHandle = INVALID_HANDLE_VALUE;
    };
    // Helios named NT sharing (WS1 #4). At most one may be set, and only
    // with sharedType = OPAQUE_WIN32 (monitored fences have no KMT flavor):
    //  - ntExportName: publish the created semaphore's WDDM sync under this
    //    kernel object name (e.g. a caller-supplied name);
    //    ntSecurityAttributes (a SECURITY_ATTRIBUTES*) supplies the DACL a
    //    cross-principal consumer needs to open it.
    //  - ntImportName: import an existing named sync into this semaphore
    //    (VkImportSemaphoreWin32HandleInfoKHR::name, null handle).
    // Named fences skip the D3DKMT local-handle bookkeeping (kmtLocal/
    // kmtGlobal stay 0) — signal and wait ride the Vulkan semaphore only.
    const wchar_t*  ntExportName = nullptr;
    const wchar_t*  ntImportName = nullptr;
    const void*     ntSecurityAttributes = nullptr;
  };

  /**
   * \brief Fence-value pair
   */
  struct DxvkFenceValuePair {
    DxvkFenceValuePair() { }
    DxvkFenceValuePair(Rc<DxvkFence>&& fence_, uint64_t value_)
    : fence(std::move(fence_)), value(value_) { }
    DxvkFenceValuePair(const Rc<DxvkFence>& fence_, uint64_t value_)
    : fence(fence_), value(value_) { }

    Rc<DxvkFence> fence;
    uint64_t value;
  };

  /**
   * \brief Fence
   *
   * Wrapper around Vulkan timeline semaphores that
   * can signal an event when the value changes.
   */
  class DxvkFence : public RcObject {

  public:

    DxvkFence(
            DxvkDevice*           device,
      const DxvkFenceCreateInfo&  info);

    ~DxvkFence();

    /**
     * \brief Semaphore handle
     */
    VkSemaphore handle() const {
      return m_semaphore;
    }
    
    /**
     * \brief D3DKMT sync object local handle
     * \returns The sync object D3DKMT local handle
     * \returns \c 0 if fence is not shared
     */
    D3DKMT_HANDLE kmtLocal() const {
      return m_kmtLocal;
    }

    /**
     * \brief D3DKMT sync object global handle
     * \returns The sync object D3DKMT global handle
     * \returns \c 0 if sync object is not shared or shared with NT handle
     */
    D3DKMT_HANDLE kmtGlobal() const {
      return m_kmtGlobal;
    }

    /**
     * \brief Retrieves current semaphore value
     * \returns Current semaphore value
     */
    uint64_t getValue();

    /**
     * \brief Enqueues semaphore wait
     *
     * Signals the given event when the
     * semaphore reaches the given value.
     * \param [in] value Enqueue value
     * \param [in] event Callback
     */
    void enqueueWait(uint64_t value, DxvkFenceEvent&& event);

    /**
     * \brief Create a new shared handle to timeline semaphore backing the fence
     * \returns The shared handle with the type given by DxvkFenceCreateInfo::sharedType
     */
    HANDLE sharedHandle() const;

    /*
     * \brief Waits for the given value
     *
     * Blocks the calling thread until
     * the fence reaches the given value.
     * \param [in] value Value to wait for
    */
    void wait(uint64_t value);

    /**
     * \brief Bounded wait for the given value
     *
     * Never blocks past the timeout — the Helios consumer-side present
     * wait proceeds (loudly) on VK_TIMEOUT instead of wedging the IDD.
     * \param [in] value Value to wait for
     * \param [in] timeoutNs Wait budget in nanoseconds
     * \returns VK_SUCCESS, VK_TIMEOUT, or an error
     */
    VkResult waitBounded(uint64_t value, uint64_t timeoutNs);

  private:

    struct QueueItem {
      QueueItem() { }
      QueueItem(uint64_t v, DxvkFenceEvent&& e)
      : value(v), event(std::move(e)) { }

      uint64_t        value;
      DxvkFenceEvent  event;

      bool operator == (const QueueItem& item) const { return value == item.value; }
      bool operator != (const QueueItem& item) const { return value != item.value; }
      bool operator <  (const QueueItem& item) const { return value <  item.value; }
      bool operator <= (const QueueItem& item) const { return value <= item.value; }
      bool operator >  (const QueueItem& item) const { return value >  item.value; }
      bool operator >= (const QueueItem& item) const { return value >= item.value; }
    };

    Rc<vk::DeviceFn>                m_vkd;
    DxvkFenceCreateInfo             m_info;
    VkSemaphore                     m_semaphore;
    D3DKMT_HANDLE                   m_kmtDevice = 0;
    D3DKMT_HANDLE                   m_kmtLocal = 0;
    D3DKMT_HANDLE                   m_kmtGlobal = 0;

    std::priority_queue<QueueItem>  m_queue;
    bool                            m_running    = false;

    dxvk::mutex                     m_mutex;
    dxvk::condition_variable        m_condVar;
    dxvk::thread                    m_thread;

    void run();

    void initKmtHandles();

  };

}
