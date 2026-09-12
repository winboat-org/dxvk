#include "dxvk_cs.h"
#include "dxvk_helios_feed_trace.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace dxvk {

  namespace {

    template<typename Fn>
    void reportCsError(Fn&& report) noexcept {
      try {
        report();
      } catch (...) {
        // In particular, a bad_alloc on the worker may also prevent Logger's
        // string construction. Failure was already published; attempt a stderr
        // diagnostic without further C++ allocation or an escaped exception.
        std::fputs("DXVK: CS worker failed; command stream quarantined (logging failed)\n", stderr);
      }
    }

    // Default to producer sharding in Helios. Setting this to zero selects
    // only shard zero and reproduces the former single-pool mutex behavior.
    bool heliosCsPoolSharding() {
      static const bool enabled = [] {
        const char* env = std::getenv("HELIOS_DXVK_CS_POOL_SHARDING");
        return !(env && env[0] == '0');
      }();

      return enabled;
    }

  }
  
  DxvkCsChunk::DxvkCsChunk() {
    
  }
  
  
  DxvkCsChunk::~DxvkCsChunk() {
    this->reset();
  }
  
  
  void DxvkCsChunk::init(DxvkCsChunkFlags flags) {
    m_flags = flags;
  }


  void DxvkCsChunk::executeAll(DxvkContext* ctx) {
    auto cmd = m_head;
    
    if (m_flags.test(DxvkCsChunkFlag::SingleUse)) {
      while (cmd != nullptr) {
        // Keep the throwing command and every unexecuted command in the
        // live chain. reset() must never destroy an earlier command twice.
        cmd->exec(ctx);
        m_head = cmd->next();
        cmd->~DxvkCsCmd();
        cmd = m_head;
      }

      m_next = &m_head;
      m_commandOffset = 0;
    } else {
      while (cmd != nullptr) {
        cmd->exec(ctx);
        cmd = cmd->next();
      }
    }
  }
  
  
  void DxvkCsChunk::reset() {
    auto cmd = m_head;

    while (cmd != nullptr) {
      auto next = cmd->next();
      cmd->~DxvkCsCmd();
      cmd = next;
    }
    
    m_head = nullptr;
    m_next = &m_head;

    m_commandOffset = 0;
  }
  
  
  DxvkCsChunkPool::DxvkCsChunkPool()
  : m_shardMask(heliosCsPoolSharding() ? ShardCount - 1u : 0u) { }
  
  
  DxvkCsChunkPool::~DxvkCsChunkPool() {
    for (auto& shard : m_shards) {
      for (DxvkCsChunk* chunk : shard.chunks)
        delete chunk;
    }
  }
  
  
  uint32_t DxvkCsChunkPool::pickShard() const {
    // Keep HELIOS_DXVK_CS_POOL_SHARDING=0 a true single-pool baseline:
    // do not materialize a thread ID or run the mixer when every operation
    // necessarily selects shard zero.
    if (!m_shardMask)
      return 0u;

    // Mix the producer thread ID before masking: Windows thread IDs often
    // have aligned low bits, which would otherwise leave most shards idle.
    uint32_t threadId = this_thread::get_id();
    threadId *= 0x9e3779b9u;
    threadId ^= threadId >> 16u;
    return threadId & m_shardMask;
  }


  DxvkCsChunkPool::Allocation DxvkCsChunkPool::allocChunk(DxvkCsChunkFlags flags) {
    uint32_t shardIndex = pickShard();
    auto& shard = m_shards[shardIndex];
    DxvkCsChunk* chunk = nullptr;

    { std::lock_guard<dxvk::mutex> lock(shard.mutex);
      
      if (shard.chunks.size() != 0) {
        chunk = shard.chunks.back();
        shard.chunks.pop_back();
      }
    }
    
    if (!chunk)
      chunk = new DxvkCsChunk();
    
    chunk->init(flags);
    return { chunk, shardIndex };
  }
  
  
  void DxvkCsChunkPool::freeChunk(
          DxvkCsChunk* chunk,
          uint32_t     shardIndex) {
    chunk->reset();

    auto& shard = m_shards[shardIndex];
    try {
      std::lock_guard<dxvk::mutex> lock(shard.mutex);
      shard.chunks.push_back(chunk);
    } catch (const std::bad_alloc&) {
      // Recycling is optional. A capture release/destructor must not throw
      // merely because the cache cannot grow during failure cleanup.
      delete chunk;
    }
  }
  
  
  DxvkCsThread::DxvkCsThread(
    const Rc<DxvkDevice>&   device,
    const Rc<DxvkContext>&  context)
  : m_device(device), m_context(context),
    m_thread([this] { threadFunc(); }) {
    
  }
  
  
  DxvkCsThread::~DxvkCsThread() {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }
    
    m_condOnAdd.notify_one();
    m_thread.join();
  }
  
  
  uint64_t DxvkCsThread::dispatchChunk(DxvkCsChunkRef&& chunk) {
    uint64_t seq;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      if (m_hasError.load(std::memory_order_acquire) || m_stopped.load())
        return 0u;

      seq = m_queueOrdered.seqDispatch + 1u;

      auto& entry = m_queueOrdered.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = seq;
      m_queueOrdered.seqDispatch = seq;

      helios_feed::csChunkEnqueued(
        m_queueOrdered.queue.size() + m_queueHighPrio.queue.size());

      m_condOnAdd.notify_one();
    }
    
    return seq;
  }


  bool DxvkCsThread::injectChunk(DxvkCsQueue queue, DxvkCsChunkRef&& chunk, bool synchronize) {
    uint64_t timeline = 0u;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      if (m_hasError.load(std::memory_order_acquire) || m_stopped.load())
        return false;

      auto& q = getQueue(queue);
      if (synchronize)
        timeline = q.seqDispatch + 1u;

      auto& entry = q.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = timeline;
      if (synchronize)
        q.seqDispatch = timeline;

      helios_feed::csChunkEnqueued(
        m_queueOrdered.queue.size() + m_queueHighPrio.queue.size());

      m_condOnAdd.notify_one();

      if (queue == DxvkCsQueue::HighPriority) {
        // Worker will check this flag after executing any
        // chunk without causing additional lock contention
        m_hasHighPrio.store(true);
      }
    }

    if (synchronize) {
      std::unique_lock<dxvk::mutex> lock(m_counterMutex);

      m_condOnSync.wait(lock, [this, queue, timeline] {
        return getCounter(queue).load() >= timeline || hasError();
      });
    }

    return !hasError();
  }


  bool DxvkCsThread::synchronize(uint64_t seq) {
    if (hasError())
      return false;

    // Avoid locking if we know the sync is a no-op, may
    // reduce overhead if this is being called frequently
    if (seq > m_seqOrdered.load()) {
      // Snapshot all work accepted before this call. Concurrent dispatch
      // after this snapshot belongs to the next synchronization.
      if (seq == SynchronizeAll) {
        std::lock_guard lock(m_mutex);
        seq = m_queueOrdered.seqDispatch;
      }

      auto t0 = dxvk::high_resolution_clock::now();

      { std::unique_lock<dxvk::mutex> lock(m_counterMutex);
        m_condOnSync.wait(lock, [this, seq] {
          return m_seqOrdered.load() >= seq || hasError();
        });
      }

      auto t1 = dxvk::high_resolution_clock::now();
      auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

      if (m_device) {
        m_device->addStatCtr(DxvkStatCounter::CsSyncCount, 1);
        m_device->addStatCtr(DxvkStatCounter::CsSyncTicks, ticks.count());
      }
    }

    return !hasError();
  }


  void DxvkCsThread::fail() {
    std::vector<DxvkCsQueuedChunk> ordered;
    std::vector<DxvkCsQueuedChunk> highPrio;

    {
      // Admission and the wait predicate are protected by different mutexes.
      // Publish while holding both: no work can slip in after quarantine and
      // no waiter can miss the terminal transition between checking and sleep.
      std::lock_guard queueLock(m_mutex);
      std::lock_guard counterLock(m_counterMutex);
      m_hasError.store(true, std::memory_order_release);
      ordered.swap(m_queueOrdered.queue);
      highPrio.swap(m_queueHighPrio.queue);
      m_hasHighPrio.store(false);
    }

    // A recording failure makes the API device unusable, but submitted GPU
    // work retains its real completion path. Never signal a sequence/fence
    // for a discarded command. Release captures outside both mutexes.
    if (m_device)
      m_device->notifyCsError();
    m_condOnSync.notify_all();
  }
  
  
  void DxvkCsThread::threadFunc() {
    // Local chunk queues, we use two queues and swap between
    // them in order to potentially reduce lock contention.
    std::vector<DxvkCsQueuedChunk> ordered;
    std::vector<DxvkCsQueuedChunk> highPrio;

    try {
      // Startup helpers may allocate too. A failure before the first chunk
      // must publish the same terminal state as a recording exception.
      env::setThreadName("dxvk-cs");
      const bool traceFeed = helios_feed::enabled();

      while (!m_stopped.load()) {
        { std::unique_lock<dxvk::mutex> lock(m_mutex);

          auto pred = [this] { return
              !m_queueOrdered.queue.empty()
              || !m_queueHighPrio.queue.empty()
              || m_stopped.load();
          };

          if (unlikely(!pred())) {
            auto t0 = dxvk::high_resolution_clock::now();

            m_condOnAdd.wait(lock, [&] {
              return pred();
            });

            auto t1 = dxvk::high_resolution_clock::now();
            if (m_device)
              m_device->addStatCtr(DxvkStatCounter::CsIdleTicks, std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

            if (traceFeed)
              helios_feed::csWorkerIdle(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
          }

          const size_t dequeued = m_queueOrdered.queue.size()
            + m_queueHighPrio.queue.size();
          std::swap(ordered, m_queueOrdered.queue);
          std::swap(highPrio, m_queueHighPrio.queue);

          if (traceFeed && dequeued)
            helios_feed::csChunkDequeued(dequeued, 0u);

          m_hasHighPrio.store(false);
        }

        size_t orderedIndex = 0u;
        size_t highPrioIndex = 0u;

        while (highPrioIndex < highPrio.size() || orderedIndex < ordered.size()) {
          // Re-fill local high-priority queue if the app has queued anything up
          // in the meantime, we want to reduce possible synchronization delays.
          if (highPrioIndex >= highPrio.size() && m_hasHighPrio.load()) {
            highPrio.clear();
            highPrioIndex = 0u;

            std::unique_lock<dxvk::mutex> lock(m_mutex);
            const size_t dequeued = m_queueHighPrio.queue.size();
            std::swap(highPrio, m_queueHighPrio.queue);

            if (traceFeed && dequeued)
              helios_feed::csChunkDequeued(dequeued, 0u);

            m_hasHighPrio.store(false);
          }

          // Drain high-priority queue first
          bool isHighPrio = highPrioIndex < highPrio.size();
          auto& entry = isHighPrio ? highPrio[highPrioIndex++] : ordered[orderedIndex++];

          if (m_context)
            m_context->addStatCtr(DxvkStatCounter::CsChunkCount, 1);

          const auto workStart = traceFeed
            ? dxvk::high_resolution_clock::now()
            : dxvk::high_resolution_clock::time_point();
          entry.chunk->executeAll(m_context.ptr());

          if (traceFeed) {
            const auto workEnd = dxvk::high_resolution_clock::now();
            helios_feed::csWorkerWork(
              std::chrono::duration_cast<std::chrono::nanoseconds>(workEnd - workStart).count());
          }

          if (entry.seq) {
            // Use a separate mutex for the chunk counter, this will only
            // ever be contested if synchronization is actually necessary.
            std::lock_guard lock(m_counterMutex);

            auto& counter = isHighPrio ? m_seqHighPrio : m_seqOrdered;
            counter.store(entry.seq);

            // Both timelines share this condition variable. Waking only one
            // may select a waiter for the other timeline and strand this one.
            m_condOnSync.notify_all();
          }

          // Immediately free the chunk to release
          // references to any resources held by it
          entry.chunk = DxvkCsChunkRef();
        }

        ordered.clear();
        highPrio.clear();
      }
    } catch (const DxvkError& e) {
      fail();
      reportCsError([&] {
        Logger::err("Exception on CS thread! Command stream quarantined.");
        Logger::err(e.message());
      });
    } catch (const std::exception& e) {
      fail();
      reportCsError([&] {
        Logger::err("Standard exception on CS thread! Command stream quarantined.");
        Logger::err(e.what());
      });
    } catch (...) {
      fail();
      reportCsError([] {
        Logger::err("Unknown exception on CS thread! Command stream quarantined.");
      });
    }
  }
  
}
