#include "dxvk_device.h"
#include "dxvk_helios_feed_trace.h"
#include "dxvk_queue.h"

namespace dxvk {
  
  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device, const DxvkQueueCallback& callback)
  : m_device        (device),
    m_checkpoints   (device->getCheckpointBuffer()),
    m_callback      (callback),
    m_recordOnly    (device->instance()->isRecordOnlyDirect()) {
    if (m_recordOnly) {
      // HELIOS D1: record-only mode spawns no submission threads, so nothing
      // runs at idle — and the ICD's per-device pending object-command lane
      // (deferred view creates / descriptor updates) drains only on real
      // queue submits. A device that defers without submitting fills the lane
      // to its 8192 cap and the device dies (the start-menu freeze,
      // 2026-09-01). This 1 Hz thread pushes an empty submit through the
      // outer bracket; the ICD makes it a no-op when the lane is empty.
      m_submitThread = dxvk::thread([this] () { recordOnlyFlushLoop(); });
      return;
    }

    m_submitThread = dxvk::thread([this] () { submitCmdLists(); });
    m_finishThread = dxvk::thread([this] () { finishCmdLists(); });

    auto vk = m_device->vkd();

    VkSemaphoreTypeCreateInfo semaphoreType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &semaphoreType };

    VkResult vrGraphics = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.graphics);
    VkResult vrTransfer = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.transfer);

    if (vrGraphics || vrTransfer) {
      throw DxvkError(str::format("Failed to create timeline semaphores: ",
        vrGraphics > vrTransfer ? vrGraphics : vrTransfer));
    }
  }
  
  
  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    auto vk = m_device->vkd();

    if (m_recordOnly) {
      { std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_stopped.store(true);
      }
      m_appendCond.notify_all();
      m_submitThread.join();

      // DxvkDevice normally joined before member destruction. Keep this
      // bounded fallback for constructor unwinds where no D3D11 object could
      // have submitted work yet.
      if (!m_finishQueue.empty())
        completeRecordOnlySubmissions();
      return;
    }

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }

    m_appendCond.notify_all();
    m_submitCond.notify_all();

    m_submitThread.join();
    m_finishThread.join();

    vk->vkDestroySemaphore(vk->device(), m_semaphores.graphics, nullptr);
    vk->vkDestroySemaphore(vk->device(), m_semaphores.transfer, nullptr);
  }
  
  
  void DxvkSubmissionQueue::submit(
          DxvkSubmitInfo            submitInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    if (m_recordOnly) {
      std::unique_lock<dxvk::mutex> queueLock(m_mutexQueue);

      bool atCapacity = false;
      {
        std::unique_lock<dxvk::mutex> lock(m_mutex);
        atCapacity = m_finishQueue.size() >= MaxNumQueuedCommandBuffers;
      }

      // Pool pressure is one of the explicit places where record-only DXVK
      // may synchronously join the exact outer context.
      if (atCapacity && completeRecordOnlySubmissionsLocked() != VK_SUCCESS) {
        if (status)
          status->result = VK_ERROR_DEVICE_LOST;
        return;
      }

      DxvkSubmitEntry entry = { };
      entry.status = status;
      entry.submit = std::move(submitInfo);
      entry.latency = std::move(latencyInfo);

      if (m_lastError != VK_ERROR_DEVICE_LOST) {
        if (m_callback)
          m_callback(true);

        if (entry.latency.tracker)
          entry.latency.tracker->notifyQueueSubmit(entry.latency.frameId);

        entry.result = entry.submit.cmdList != nullptr
          ? entry.submit.cmdList->submit(m_semaphores, m_timelines, 0u)
          : VK_SUCCESS;

        if (m_callback)
          m_callback(false);
      } else {
        entry.result = VK_ERROR_DEVICE_LOST;
      }

      if (entry.status)
        entry.status->result = entry.result;

      if (entry.result != VK_SUCCESS)
        m_lastError = entry.result;

      // Retain the whole list until an exact HQC1 join proves every accepted
      // batch has retired. A failure may follow an earlier accepted sub-batch,
      // so the failure path must retain it as well.
      {
        std::unique_lock<dxvk::mutex> lock(m_mutex);
        if (entry.submit.cmdList != nullptr)
          m_finishQueue.push(std::move(entry));
        m_submitCond.notify_all();
        m_finishCond.notify_all();
      }

      m_device->m_objects.memoryManager().performTimedTasks();
      return;
    }

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_finishCond.wait(lock, [this] {
      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
    });

    DxvkSubmitEntry entry = { };
    entry.status = status;
    entry.submit = std::move(submitInfo);
    entry.latency = std::move(latencyInfo);

    m_submitQueue.push(std::move(entry));
    helios_feed::submissionEnqueued(m_submitQueue.size());
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::present(
          DxvkPresentInfo           presentInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    if (m_recordOnly) {
      Logger::err("DxvkSubmissionQueue: record-only WSI present refused before the present-layer cutover");
      if (status)
        status->result = VK_ERROR_EXTENSION_NOT_PRESENT;
      return;
    }

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    DxvkSubmitEntry entry = { };
    entry.status  = status;
    entry.present = std::move(presentInfo);
    entry.latency = std::move(latencyInfo);

    m_submitQueue.push(std::move(entry));
    helios_feed::submissionEnqueued(m_submitQueue.size());
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    if (m_recordOnly)
      return;

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [status] {
      return status->result.load() != VK_NOT_READY;
    });
  }


  void DxvkSubmissionQueue::synchronize() {
    if (m_recordOnly)
      return;

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });
  }


  void DxvkSubmissionQueue::waitForIdle() {
    if (m_recordOnly) {
      completeRecordOnlySubmissions();
      return;
    }

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });

    m_finishCond.wait(lock, [this] {
      return m_finishQueue.empty();
    });
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();

    if (m_callback)
      m_callback(true);
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    if (m_callback)
      m_callback(false);

    m_mutexQueue.unlock();
  }


  VkResult DxvkSubmissionQueue::completeRecordOnlySubmissions() {
    std::unique_lock<dxvk::mutex> queueLock(m_mutexQueue);
    return completeRecordOnlySubmissionsLocked();
  }


  VkResult DxvkSubmissionQueue::completeRecordOnlySubmissionsLocked() {
    VkResult result = m_device->joinHeliosOuterSubmit();

    if (result != VK_SUCCESS)
      m_lastError = result;

    while (true) {
      DxvkSubmitEntry entry = { };

      {
        std::unique_lock<dxvk::mutex> lock(m_mutex);
        if (m_finishQueue.empty())
          break;
        entry = std::move(m_finishQueue.front());
        m_finishQueue.pop();
        m_finishCond.notify_all();
      }

      if (entry.submit.cmdList == nullptr)
        continue;

      if (result == VK_SUCCESS) {
        if (entry.latency.tracker) {
          entry.latency.tracker->notifyGpuExecutionBegin(entry.latency.frameId);
          entry.latency.tracker->notifyGpuExecutionEnd(entry.latency.frameId);
        }
        entry.submit.cmdList->notifyObjects();
        entry.submit.cmdList->reset();
        m_device->recycleCommandList(entry.submit.cmdList);
      } else {
        m_leakedCmdLists.push_back(std::move(entry.submit.cmdList));
      }
    }

    m_device->m_objects.memoryManager().performTimedTasks();
    return result;
  }


  void DxvkSubmissionQueue::recordOnlyFlushLoop() {
    env::setThreadName("dxvk-helios-flush");

    while (true) {
      { std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_appendCond.wait_for(lock, std::chrono::seconds(1));
        if (m_stopped.load())
          return;
      }

      if (m_lastError == VK_ERROR_DEVICE_LOST)
        continue;

      std::unique_lock<dxvk::mutex> queueLock(m_mutexQueue);

      if (m_callback)
        m_callback(true);

      // Bounded proof-of-life: the flush is otherwise silent (empty lane =
      // ICD no-op, empty scope closes abandoned), so log the first ticks.
      static std::atomic<uint32_t> s_flushTicks = { 0u };
      uint32_t tick = s_flushTicks.fetch_add(1u);
      if (tick < 4u || (tick & 0x1FFu) == 0u)
        Logger::info(str::format("Helios: idle object-lane flush tick ", tick));

      void* outerScope = m_device->beginHeliosOuterSubmit();

      if (outerScope) {
        auto vk = m_device->vkd();
        VkResult vr = vk->vkQueueSubmit2(
          m_device->queues().graphics.queueHandle, 0, nullptr, VK_NULL_HANDLE);
        vr = m_device->finishHeliosOuterSubmit(outerScope, vr);

        if (vr != VK_SUCCESS)
          Logger::warn(str::format("Helios: idle object-lane flush failed: ", vr));
      }

      if (m_callback)
        m_callback(false);
    }
  }


  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");

    uint64_t trackedSubmitId = 0u;
    uint64_t trackedPresentId = 0u;

    while (!m_stopped.load()) {
      DxvkSubmitEntry entry;

      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        m_appendCond.wait(lock, [this] {
          return m_stopped.load() || !m_submitQueue.empty();
        });

        if (m_stopped.load())
          return;

        entry = std::move(m_submitQueue.front());
      }

      // Submit command buffer to device
      if (m_lastError != VK_ERROR_DEVICE_LOST) {
        std::lock_guard<dxvk::mutex> lock(m_mutexQueue);

        if (m_callback)
          m_callback(true);

        if (entry.submit.cmdList != nullptr) {
          if (entry.latency.tracker) {
            entry.latency.tracker->notifyQueueSubmit(entry.latency.frameId);

            if (!trackedSubmitId && entry.latency.frameId > trackedPresentId)
              trackedSubmitId = entry.latency.frameId;
          }

          entry.result = entry.submit.cmdList->submit(
            m_semaphores, m_timelines, trackedSubmitId);
          entry.timelines = m_timelines;
        } else if (entry.present.presenter != nullptr) {
          if (entry.latency.tracker)
            entry.latency.tracker->notifyQueuePresentBegin(entry.latency.frameId);

          entry.result = entry.present.presenter->presentImage(
            entry.present.frameId, entry.latency.tracker);

          if (entry.latency.tracker) {
            entry.latency.tracker->notifyQueuePresentEnd(
              entry.latency.frameId, entry.result);

            trackedPresentId = entry.latency.frameId;
            trackedSubmitId = 0u;
          }
        }

        if (m_callback)
          m_callback(false);
      } else {
        // Don't submit anything after device loss
        // so that drivers get a chance to recover
        entry.result = VK_ERROR_DEVICE_LOST;
      }

      if (entry.status)
        entry.status->result = entry.result;

      if (entry.result == VK_ERROR_DEVICE_LOST && m_checkpoints)
        m_checkpoints->printHangInfo();

      // On success, pass it on to the queue thread
      Rc<DxvkCommandList> droppedCmdList;
      bool droppedDeviceLost = false;

      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        bool doForward = (entry.result == VK_SUCCESS) ||
          (entry.present.presenter != nullptr && entry.result != VK_ERROR_DEVICE_LOST);

        if (doForward) {
          m_finishQueue.push(std::move(entry));
        } else {
          Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
          droppedDeviceLost = (entry.result == VK_ERROR_DEVICE_LOST);
          m_lastError = entry.result;

          if (m_lastError != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();

          // HELIOS: a dropped submission still holds lifetime refs on every
          // resource recorded into it. On a HEALTHY-device failure the
          // waitForIdle above drained the GPU, so nothing is pending: without
          // notifyObjects() those refs would leak and a later
          // waitForResource() on any of them never returns (2026-07-05: dwm
          // compositor wedged in Map). reset()+recycle below is then legal.
          //
          // But when the device is LOST the waitForIdle is skipped and the
          // host may still be executing EARLIER submissions we deliberately
          // leaked in finishCmdLists. reset() here would run the descriptor
          // pool's monotonic notifyCompletion(), which resets every still-
          // InFlight pool with a lower tracking id — including those earlier
          // pending pools (2026-07-09 host log: vkResetDescriptorPool "in use
          // by VkCommandBuffer"); and releasing refs would free resources
          // those pending buffers reference. So on loss leak the dropped list
          // whole instead (device loss ends the device anyway).
          droppedCmdList = std::move(entry.submit.cmdList);
        }

        m_submitQueue.pop();
        helios_feed::submissionDequeued(m_submitQueue.size());
        m_submitCond.notify_all();

        if (droppedDeviceLost && droppedCmdList != nullptr) {
          m_leakedCmdLists.push_back(std::move(droppedCmdList));
          Logger::err(str::format("DxvkSubmissionQueue: leaking dropped command "
            "list (device lost); leaked=", m_leakedCmdLists.size()));
          // Wake synchronizeUntil waiters so they observe m_lastError.
          m_finishCond.notify_all();
        }
      }

      if (droppedCmdList != nullptr) {
        // Reached only on a healthy-device failure (GPU drained above): safe
        // to release refs, reset pools and recycle the list.
        droppedCmdList->notifyObjects();
        droppedCmdList->reset();
        m_device->recycleCommandList(droppedCmdList);

        // Wake threads blocked in synchronizeUntil so they observe both the
        // released refs and m_lastError.
        std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_finishCond.notify_all();
      }

      // Good time to invoke allocator tasks now since we
      // expect this to get called somewhat periodically.
      m_device->m_objects.memoryManager().performTimedTasks();
    }
  }
  
  
  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");

    auto vk = m_device->vkd();

    while (!m_stopped.load()) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);

      if (m_finishQueue.empty()) {
        auto t0 = dxvk::high_resolution_clock::now();

        m_submitCond.wait(lock, [this] {
          return m_stopped.load() || !m_finishQueue.empty();
        });

        auto t1 = dxvk::high_resolution_clock::now();
        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      }

      if (m_stopped.load())
        return;
      
      DxvkSubmitEntry entry = std::move(m_finishQueue.front());
      lock.unlock();
      
      bool hostRetired = true;

      if (entry.submit.cmdList != nullptr) {
        VkResult status = m_lastError.load();

        std::array<VkSemaphore, 2> semaphores = { m_semaphores.graphics, m_semaphores.transfer };
        std::array<uint64_t, 2> timelines = { entry.timelines.graphics, entry.timelines.transfer };

        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = semaphores.size();
        waitInfo.pSemaphores = semaphores.data();
        waitInfo.pValues = timelines.data();

        if (status != VK_ERROR_DEVICE_LOST) {
          if (entry.latency.tracker)
            entry.latency.tracker->notifyGpuExecutionBegin(entry.latency.frameId);

          status = m_device->instance()->isRecordOnlyDirect()
            ? m_device->joinHeliosOuterSubmit()
            : vk->vkWaitSemaphores(vk->device(), &waitInfo, ~0ull);
          hostRetired = (status == VK_SUCCESS);

          if (entry.latency.tracker && status == VK_SUCCESS)
            entry.latency.tracker->notifyGpuExecutionEnd(entry.latency.frameId);
        } else {
          // HELIOS: the guest-side loss latch (vn sem-deadline) can declare
          // this device lost while the HOST device is healthy and still
          // executing the submission. Resetting or destroying its command
          // pool then is host-side UB (2026-07-05 freeze evidence:
          // vkResetCommandPool-in-use VUs during post-loss teardown). Give
          // the host a bounded grace to retire the work; leak the command
          // list below if it does not.
          hostRetired = m_device->instance()->isRecordOnlyDirect()
            ? m_device->joinHeliosOuterSubmit() == VK_SUCCESS
            : vk->vkWaitSemaphores(vk->device(), &waitInfo,
                2'000'000'000ull) == VK_SUCCESS;
        }

        if (status == VK_ERROR_DEVICE_LOST && m_checkpoints)
          m_checkpoints->printHangInfo();

        if (status != VK_SUCCESS) {
          m_lastError = status;

          if (status != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();
        }
      } else if (entry.present.presenter != nullptr) {
        // Signal the frame and then immediately destroy the reference.
        // This is necessary since the front-end may want to explicitly
        // destroy the presenter object. 
        entry.present.presenter->signalFrame(entry.present.frameId, entry.latency.tracker);
        entry.present.presenter = nullptr;
      }

      // Release resources and signal events, then immediately wake
      // up any thread that's currently waiting on a resource in
      // order to reduce delays as much as possible.
      //
      // HELIOS: only release the object-tracker refs when the host actually
      // retired the work. When the guest-side loss latch fires while the HOST
      // is still executing the submission (hostRetired == false),
      // notifyObjects() would drop this list's lifetime refs on every image,
      // view, buffer and memory it recorded — letting dwm's teardown destroy
      // objects a still-pending (leaked) command buffer references. That is
      // the 2026-07-09 host-log flood (vkDestroyImage / vkDestroyImageView /
      // vkFreeMemory "currently in use by VkCommandBuffer/VkDescriptorSet"
      // preceding every dwm CS-error death). The leaked command list below
      // retains those refs, keeping the resources alive (and the descriptor
      // pool un-reset) until the process exits. waitForResource already bails
      // on DEVICE_LOST, so retaining the refs cannot wedge a Map waiter.
      if (entry.submit.cmdList != nullptr && hostRetired)
        entry.submit.cmdList->notifyObjects();

      lock.lock();
      m_finishQueue.pop();
      m_finishCond.notify_all();
      lock.unlock();

      // Free the command list and associated objects now
      if (entry.submit.cmdList != nullptr) {
        if (hostRetired) {
          entry.submit.cmdList->reset();
          m_device->recycleCommandList(entry.submit.cmdList);
        } else {
          // HELIOS: host work never retired (device lost). Keep the WHOLE
          // command list alive forever (its object-tracker refs included, per
          // above) — resetting/destroying a pool or freeing a resource with
          // pending buffers corrupts the (possibly healthy) host device. Loud,
          // and bounded in practice: device loss ends the device's useful life.
          std::unique_lock<dxvk::mutex> leakLock(m_mutex);
          m_leakedCmdLists.push_back(std::move(entry.submit.cmdList));
          Logger::err(str::format("DxvkSubmissionQueue: leaking command list "
            "with unretired host work (device lost); leaked=", m_leakedCmdLists.size()));
        }
      }
    }
  }
  
}
