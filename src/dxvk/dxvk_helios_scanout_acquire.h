#pragma once

#include <atomic>
#include <unordered_map>

#include "dxvk_fence.h"

#include "../util/thread.h"
#include "../util/util_time.h"

namespace dxvk {

  class DxvkDevice;

  /**
   * \brief One KMD read-ledger slot (Helios D4a)
   *
   * A nonzero generation is the claim identity; `issued` counts reads for that
   * claim and `retired` their terminal outcomes. `resid == 0` or generation 0
   * means no valid slot.
   */
  struct DxvkHeliosLedgerSlot {
    uint32_t resid;
    uint32_t pad0;
    uint64_t generation;
    uint64_t issued;
    uint64_t retired;
  };

  /**
   * \brief The Helios scanout-acquire seam (D4a)
   *
   * Runtime-resolved readers over state the UMD (helios_umd.dll, Rust) owns:
   * the ScanoutAcquire knob + KMD capability probe, and the mapped read-only
   * ledger page. Resolved BY NAME from the module containing this code —
   * statically linked into helios_umd.dll in the shipping configuration — plus
   * the venus ICD's memory→resid export from the loaded-module list. By-name
   * rather than a link-time reference so the fork's standalone d3d11.dll
   * target keeps linking; where the exports are absent, \c enabled() latches
   * false and the feature costs one atomic load per flush.
   */
  namespace helios_acquire {

    /** Fast-path gate: knob ON, KMD probe OK, ledger mapped. */
    bool enabled();

    /**
     * \brief Reads one ledger slot by venus resource id
     *
     * \c false = no slot (no read ever issued for this buffer, slot table
     * overflow, or feature off) — the caller arms no wait, which is exactly
     * today's behavior.
     */
    bool ledgerLookupV2(
      uint32_t resid, uint64_t* generation, uint64_t* issued, uint64_t* retired);

    /** Snapshots up to \c maxSlots slots; returns the count written. */
    uint32_t ledgerSnapshotV2(DxvkHeliosLedgerSlot* slots, uint32_t maxSlots);

    /**
     * \brief Venus resource id backing a device memory object
     *
     * The cross-layer identity (never recycled). Resolved through the venus
     * ICD's \c helios_venus_memory_res_id export — the same reader
     * get_resource_memory_info uses on the UMD side. 0 = unknown.
     */
    uint32_t residFromMemory(VkDeviceMemory memory);

    /** Register a dedicated external timeline for Present-buffer consumers. */
    bool registerPresentBufferStream(VkDevice device, VkSemaphore semaphore);

    /** Atomically claim one KMD Present buffer until `value` is signaled. */
    bool claimPresentBufferRead(
      VkDevice device, VkSemaphore semaphore, uint32_t resid, uint32_t value);

  }

  /**
   * \brief Per-device scanout-reuse gate state (Helios D4a)
   *
   * Owns the gate fences T_generation — one PLAIN INTERNAL timeline semaphore
   * per scan-out buffer, value space = the ledger's \c retired counter — and
   * the signaler thread that forwards ledger retirements onto them with
   * \c vkSignalSemaphore (CPU signal; DxvkKeyedMutex::ReleaseSync precedent).
   *
   * The emission side (DxvkContext::heliosEmitScanoutReuseWaits) arms
   * `waitFence(T_generation, issued)` on the command list that re-writes a buffer
   * iff the ledger says a host readback of it is in flight; the wait is a
   * TOP_OF_PIPE VkSemaphoreSubmitInfo on that list's first vkQueueSubmit2 —
   * it parks the host GPU queue, never a guest CPU thread.
   *
   * Liveness: the signaler is LEVEL-TRIGGERED — every wake (KMD retirement
   * event OR the 10 ms timeout) re-reads the whole ledger — so lost wakeups,
   * event-table overflow and registration races are a bounded hiccup, never a
   * hang; the ledger's advance is KMD-guaranteed (every issue retires by
   * type). A vanished claim means every read retired (slots recycle only at
   * `issued == retired`), so its generation-keyed gate is released to the
   * highest value ever armed and dropped. The gate retains and validates resid
   * for diagnostics; a recycled slot can never cross-signal an old generation.
   */
  class DxvkHeliosScanoutAcquire {

  public:

    explicit DxvkHeliosScanoutAcquire(DxvkDevice* device);

    ~DxvkHeliosScanoutAcquire();

    /**
     * \brief Delivers the KMD retirement event
     *
     * Auto-reset event the KMD signals on every scanout-read retirement.
     * The UMD owns the handle; it outlives this object (DestroyDevice closes
     * it only after ~DxvkDevice joined the signaler). Without one the
     * signaler runs on its 10 ms timeout alone — correct, just slower.
     */
    void setEventHandle(HANDLE event);

    /**
     * Gate fence for a generation-qualified resid, arming `issued`.
     *
     * Creates the fence lazily (initial value = \c retired read from the
     * ledger at creation) and starts the signaler with the first fence.
     * Records \c issued as armed so teardown and slot-reclaim can release it.
     * \returns \c nullptr after shutdown or on creation failure — the caller
     *          skips the wait (runs ungated, counted).
     */
    Rc<DxvkFence> armFence(
      uint32_t resid, uint64_t generation, uint64_t issued, uint64_t retired);

    /**
     * \brief Teardown, spec §5.3 order
     *
     * stop new arms → signal every gate to the highest value ever armed →
     * join the signaler → drop the fences (in-flight command lists keep their
     * own Rc until they retire). Idempotent. MUST run before ~DxvkDevice's
     * waitForIdle: an unsatisfied gate wait would park the queue into the
     * vn 8 s forward-progress deadline (device lost).
     */
    void shutdown();

    /** Census counters for the helios-acquire log line (§5.5). */
    uint64_t signalCount() const {
      return m_signals.load(std::memory_order_relaxed);
    }

    uint64_t signalFailCount() const {
      return m_signalFails.load(std::memory_order_relaxed);
    }

    uint64_t createFailCount() const {
      return m_createFails.load(std::memory_order_relaxed);
    }

    uint64_t maxArmToSigUs() const {
      return m_maxArmToSigUs.load(std::memory_order_relaxed);
    }

  private:

    struct Gate {
      Rc<DxvkFence> fence;
      uint32_t      resid = 0u;
      uint64_t      lastSignaled = 0u;
      uint64_t      highestArmed = 0u;
      high_resolution_clock::time_point armTime = { };
    };

    DxvkDevice*                         m_device;

    dxvk::mutex                         m_mutex;
    dxvk::condition_variable            m_cond;
    std::unordered_map<uint64_t, Gate>  m_gates;

    HANDLE                              m_event         = nullptr;
    dxvk::thread                        m_thread;
    bool                                m_threadStarted = false;
    bool                                m_stop          = false;

    std::atomic<uint64_t>               m_signals       = { 0u };
    std::atomic<uint64_t>               m_signalFails   = { 0u };
    std::atomic<uint64_t>               m_createFails   = { 0u };
    std::atomic<uint64_t>               m_maxArmToSigUs = { 0u };

    void startThreadLocked();

    void run();

    void processRetirements();

    void signalGateLocked(uint64_t generation, Gate& gate, uint64_t value);

  };

}
