#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

#include "../util/thread.h"

#include "dxvk_cmdlist.h"
#include "dxvk_latency.h"
#include "dxvk_presenter.h"

namespace dxvk {
  
  class DxvkDevice;
  class DxvkCheckpointBuffer;

  /**
   * \brief Submission status
   * 
   * Stores the result of a queue
   * submission or a present call.
   */
  struct DxvkSubmitStatus {
    std::atomic<VkResult> result = { VK_SUCCESS };
  };


  /**
   * \brief Queue submission info
   * 
   * Stores parameters used to submit
   * a command buffer to the device.
   */
  struct DxvkSubmitInfo {
    Rc<DxvkCommandList> cmdList;
  };
  
  
  /**
   * \brief Present info
   *
   * Stores parameters used to present
   * a swap chain image on the device.
   */
  struct DxvkPresentInfo {
    Rc<Presenter>       presenter;
    uint64_t            frameId;
  };


  /**
   * \brief Latency info
   *
   * Optionally stores a latency tracker
   * and the associated frame ID.
   */
  struct DxvkLatencyInfo {
    Rc<DxvkLatencyTracker>  tracker;
    uint64_t                frameId = 0;
  };


  /**
   * \brief Submission queue entry
   */
  struct DxvkSubmitEntry {
    VkResult            result;
    DxvkSubmitStatus*   status;
    DxvkSubmitInfo      submit;
    DxvkPresentInfo     present;
    DxvkLatencyInfo     latency;
    DxvkTimelineSemaphoreValues timelines;
  };


  /**
   * \brief Submission queue
   */
  class DxvkSubmissionQueue {

  public:
    
    DxvkSubmissionQueue(
            DxvkDevice*         device,
      const DxvkQueueCallback&  callback);

    ~DxvkSubmissionQueue();

    // Recording failure is separate from Vulkan submission failure. It must
    // wake API resource waiters without discarding or completing GPU work.
    void notifyCsError() {
      std::lock_guard lock(m_mutex);
      m_csError.store(true, std::memory_order_release);
      m_finishCond.notify_all();
    }

    bool hasCsError() const {
      return m_csError.load(std::memory_order_acquire);
    }

    void cancelProducerWaits() {
      m_cancelProducerWaits.store(true, std::memory_order_release);
    }

    /**
     * \brief Retrieves estimated GPU idle time
     *
     * This is a monotonically increasing counter
     * which can be evaluated periodically in order
     * to calculate the GPU load.
     * \returns Accumulated GPU idle time, in us
     */
    uint64_t gpuIdleTicks() const {
      return m_gpuIdle.load();
    }

    /**
     * \brief Retrieves last submission error
     * 
     * In case an error occured during asynchronous command
     * submission, it will be returned by this function.
     * \returns Last error from command submission
     */
    VkResult getLastError() const {
      return m_lastError.load();
    }
    
    /**
     * \brief Submits a command list asynchronously
     * 
     * Queues a command list for submission on the
     * dedicated submission thread. Use this to take
     * the submission overhead off the calling thread.
     * \param [in] submitInfo Submission parameters
     * \param [in] latencyInfo Latency tracker info
     * \param [out] status Submission feedback
     */
    void submit(
            DxvkSubmitInfo      submitInfo,
            DxvkLatencyInfo     latencyInfo,
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Presents an image synchronously
     *
     * Waits for queued command lists to be submitted
     * and then presents the current swap chain image
     * of the presenter. May stall the calling thread.
     * \param [in] present Present parameters
     * \param [in] latencyInfo Latency tracker info
     * \param [out] status Submission feedback
     */
    void present(
            DxvkPresentInfo     presentInfo,
            DxvkLatencyInfo     latencyInfo,
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Synchronizes with one queue submission
     * 
     * Waits for the result of the given submission
     * or present operation to become available.
     * \param [in,out] status Submission status
     */
    void synchronizeSubmission(
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Synchronizes with queue submissions
     * 
     * Waits for all pending command lists to be
     * submitted to the GPU before returning.
     */
    void synchronize();

    /**
     * \brief Synchronizes until a given condition becomes true
     *
     * Useful to wait for the GPU without busy-waiting.
     * \param [in] pred Predicate to check
     */
    template<typename Pred>
    void synchronizeUntil(const Pred& pred) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_finishCond.wait(lock, pred);
    }

    /**
     * \brief Whether both submission stages are empty
     *
     * HELIOS. Nothing is queued for submission and nothing is in flight, so
     * no further completion can occur and \ref m_finishCond will not be
     * signalled again by this queue. MUST be called with \ref m_mutex already
     * held — the only legal caller is a \ref synchronizeUntil predicate, which
     * holds it. Taking the lock here instead would self-deadlock.
     *
     * A waiter that observes "resource still in use" together with this
     * returning \c true is waiting on a reference held by a command list that
     * has not been submitted. That is terminal only if no other thread will
     * submit it; see the loud check in DxvkDevice::waitForResource.
     */
    bool isDrainedLocked() const {
      return m_submitQueue.empty() && m_finishQueue.empty();
    }

    /**
     * \brief Waits for all submissions to complete
     */
    void waitForIdle();

    /**
     * \brief Locks device queue
     *
     * Locks the mutex that protects the Vulkan queue
     * that DXVK uses for command buffer submission.
     * This is needed when the app submits its own
     * command buffers to the queue.
     */
    void lockDeviceQueue();

    /**
     * \brief Unlocks device queue
     *
     * Unlocks the mutex that protects the Vulkan
     * queue used for command buffer submission.
     */
    void unlockDeviceQueue();
    
  private:

    DxvkDevice*                 m_device;
    DxvkCheckpointBuffer*       m_checkpoints = nullptr;
    DxvkQueueCallback           m_callback;

    DxvkTimelineSemaphores      m_semaphores;
    DxvkTimelineSemaphoreValues m_timelines;

    std::atomic<VkResult>       m_lastError = { VK_SUCCESS };
    std::atomic<bool>           m_csError = { false };
    
    std::atomic<bool>           m_stopped = { false };
    std::atomic<bool>           m_cancelProducerWaits = { false };
    std::atomic<uint64_t>       m_gpuIdle = { 0ull };

    dxvk::mutex                 m_mutex;
    dxvk::mutex                 m_mutexQueue;
    
    dxvk::condition_variable    m_appendCond;
    dxvk::condition_variable    m_submitCond;
    dxvk::condition_variable    m_finishCond;

    std::queue<DxvkSubmitEntry> m_submitQueue;
    std::queue<DxvkSubmitEntry> m_finishQueue;

    // HELIOS: command lists whose host work never retired before a
    // guest-side device-loss teardown. Deliberately kept alive for the
    // process lifetime — resetting or destroying a command pool with
    // pending buffers is UB against a still-healthy host device.
    std::vector<Rc<DxvkCommandList>> m_leakedCmdLists;

    dxvk::thread                m_submitThread;
    dxvk::thread                m_finishThread;

    void submitCmdLists();

    void finishCmdLists();
    
  };
  
}
