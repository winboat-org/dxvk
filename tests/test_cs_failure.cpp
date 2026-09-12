// CPU-only regression tests against the actual CS worker and chunk pool.
// No Vulkan instance, adapter, device, or guest is opened by this executable.
#include "../src/dxvk/dxvk_cs.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>

// Fail every ordinary C++ allocation on the faulting worker only. This tests
// cleanup and terminal logging under real allocation failures, not just an
// exception with the same type. Other test threads can still coordinate.
static thread_local bool failWorkerAllocations = false;

void* operator new(std::size_t size) {
  if (failWorkerAllocations)
    throw std::bad_alloc();
  if (void* p = std::malloc(size ? size : 1u))
    return p;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace dxvk {
  Logger Logger::s_instance("cs-failure-test.log");
}

using namespace dxvk;
using namespace std::chrono_literals;

static void require(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
  }
}

struct Capture {
  explicit Capture(std::atomic<unsigned>& count) : destroyed(count) { }
  ~Capture() { destroyed.fetch_add(1u); }
  std::atomic<unsigned>& destroyed;
};

static DxvkCsChunkRef alloc(DxvkCsChunkPool& pool, bool singleUse = true) {
  const auto a = pool.allocChunk(singleUse
    ? DxvkCsChunkFlags(DxvkCsChunkFlag::SingleUse) : DxvkCsChunkFlags());
  return DxvkCsChunkRef(a.chunk, &pool, a.shard);
}

static void throwError(unsigned kind) {
  if (kind == 0u)
    throw DxvkError("intentional recording failure");
  if (kind == 1u)
    throw std::runtime_error("intentional standard exception");
  if (kind == 2u)
    throw 42;
  failWorkerAllocations = true;
  throw std::bad_alloc();
}

static void testChunkLifetime() {
  for (bool singleUse : { false, true }) {
    DxvkCsChunkPool pool;
    std::array<std::atomic<unsigned>, 3> destroyed { };
    std::array<unsigned, 3> executed { };
    {
      auto chunk = alloc(pool, singleUse);
      for (unsigned i = 0; i < 3; i++) {
        require(chunk->push([owner = std::make_unique<Capture>(destroyed[i]), &executed, i](DxvkContext*) {
          executed[i]++;
          if (i == 1u)
            throwError(0u);
        }), "record lifetime command");
      }
      try {
        chunk->executeAll(nullptr);
        require(false, "throwing chunk returned success");
      } catch (const DxvkError&) { }
      require(executed[0] == 1u && executed[1] == 1u && executed[2] == 0u,
        "commands after exception did not execute");
    }
    for (auto& count : destroyed)
      require(count.load() == 1u, "every capture destroyed exactly once");
    auto reused = alloc(pool);
    require(reused->empty(), "failed chunk resets before pool reuse");
    require(reused->push([](DxvkContext*) { }), "reused chunk accepts a command");
    reused->executeAll(nullptr);
  }
}

static void testFailedWorker(unsigned kind, DxvkCsQueue faultQueue) {
  DxvkCsChunkPool pool;
  std::atomic<unsigned> executed { 0u }, destroyed { 0u };
  {
    DxvkCsThread cs(nullptr, nullptr);
    auto completed = alloc(pool);
    require(completed->push([](DxvkContext*) { }), "record completed command");
    const auto completedSeq = cs.dispatchChunk(std::move(completed));
    require(completedSeq == 1u && cs.synchronize(completedSeq), "healthy baseline completes");

    std::promise<void> started, release;
    auto releaseFuture = release.get_future().share();
    auto fault = alloc(pool);
    require(fault->push([&started, releaseFuture, kind](DxvkContext*) {
      started.set_value();
      releaseFuture.wait();
      throwError(kind);
    }), "record fault command");
    if (faultQueue == DxvkCsQueue::Ordered)
      require(cs.dispatchChunk(std::move(fault)) == 2u, "accept ordered fault");
    else
      require(cs.injectChunk(faultQueue, std::move(fault), false), "accept priority fault");
    require(started.get_future().wait_for(5s) == std::future_status::ready, "worker enters fault command");

    auto pending = alloc(pool);
    require(pending->push([owner = std::make_unique<Capture>(destroyed), &executed](DxvkContext*) {
      executed.fetch_add(1u);
    }), "record discarded command");
    const auto pendingSeq = cs.dispatchChunk(std::move(pending));
    require(pendingSeq > completedSeq, "pending command accepted before failure");

    std::array<std::future<bool>, 8> waits;
    for (unsigned i = 0; i < waits.size(); i++) {
      waits[i] = std::async(std::launch::async, [&, i] {
        if (i < 4u)
          return cs.synchronize(i & 1u ? pendingSeq : DxvkCsThread::SynchronizeAll);
        auto injected = alloc(pool);
        require(injected->push([owner = std::make_unique<Capture>(destroyed), &executed](DxvkContext*) {
          executed.fetch_add(1u);
        }), "record synchronous injection");
        return cs.injectChunk(i & 1u ? DxvkCsQueue::HighPriority : DxvkCsQueue::Ordered,
          std::move(injected), true);
      });
    }
    release.set_value();
    for (auto& waiter : waits) {
      require(waiter.wait_for(5s) == std::future_status::ready, "failure wakes every timeline waiter");
      require(!waiter.get(), "failed synchronization is explicitly false");
    }
    require(cs.hasError(), "failure remains observable");
    require(cs.lastSequenceNumber() == completedSeq, "discarded work never advances completion");
    require(!cs.synchronize(completedSeq), "failure cannot masquerade as a successful old sync");
    require(!cs.synchronize(DxvkCsThread::SynchronizeAll), "future sync refuses immediately");

    auto rejected = alloc(pool);
    require(rejected->push([&executed](DxvkContext*) { executed.fetch_add(1u); }), "record rejected command");
    require(cs.dispatchChunk(std::move(rejected)) == 0u, "new ordered work refused");
    require(!cs.injectChunk(DxvkCsQueue::HighPriority, std::move(rejected), false), "new asynchronous work refused");
    require(!cs.injectChunk(DxvkCsQueue::HighPriority, std::move(rejected), true), "new synchronous work refused");
  }
  require(executed.load() == 0u, "no pending or rejected command executed");
  require(destroyed.load() == 5u, "all rejected/pending captures released before teardown returns");
}

static void testSuccessfulWaiters() {
  DxvkCsChunkPool pool;
  DxvkCsThread cs(nullptr, nullptr);
  std::promise<void> started, release;
  auto releaseFuture = release.get_future().share();
  auto chunk = alloc(pool);
  require(chunk->push([&started, releaseFuture](DxvkContext*) {
    started.set_value();
    releaseFuture.wait();
  }), "record successful blocked command");
  auto seq = cs.dispatchChunk(std::move(chunk));
  require(started.get_future().wait_for(5s) == std::future_status::ready, "worker starts successful command");
  std::array<std::future<bool>, 8> waits;
  for (auto& waiter : waits)
    waiter = std::async(std::launch::async, [&] { return cs.synchronize(seq); });
  release.set_value();
  for (auto& waiter : waits) {
    require(waiter.wait_for(5s) == std::future_status::ready, "normal completion wakes all matching waiters");
    require(waiter.get(), "successful completion stays successful");
  }
  require(!cs.hasError() && cs.lastSequenceNumber() == seq, "normal completion retains exact sequence");
}

int main() {
  testChunkLifetime();
  for (unsigned i = 0; i < 12u; i++) {
    testSuccessfulWaiters();
    for (unsigned kind = 0; kind < 4u; kind++) {
      testFailedWorker(kind, DxvkCsQueue::Ordered);
      testFailedWorker(kind, DxvkCsQueue::HighPriority);
    }
  }
  std::puts("PASS: CS capture lifetime, 96 worker failures (including allocation failure), both queues, all waiters, refusal and teardown");
  return 0;
}
