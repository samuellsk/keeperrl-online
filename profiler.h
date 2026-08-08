#pragma once

#ifdef EASY_PROFILER
#define BUILD_WITH_EASY_PROFILER

#include <easy/profiler.h>


#define PROFILE EASY_FUNCTION(__LINE__)
#define PROFILE_BLOCK(...) EASY_BLOCK(__VA_ARGS__)

#define ENABLE_PROFILER\
  profiler::startListen()
/*  EASY_PROFILER_ENABLE\
  DestructorFunction dumpProfileData([]{profiler::dumpBlocksToFile("test_profile.prof");})
*/

#elif defined(SIMPLE_PROFILER)

// Lightweight built-in profiler: reuses the existing PROFILE markers to measure
// per-function self-time (exclusive) and call counts, and periodically writes a
// sorted hotspot report to profile_report.txt. No external dependency.
#include <atomic>
#include <chrono>
#include <string>

struct SimpleProfSlot {
  std::string name;
  std::atomic<long long> selfNanos{0};
  std::atomic<long long> inclusiveNanos{0};
  std::atomic<long long> calls{0};
};

SimpleProfSlot* simpleProfGetSlot(const char* name);
void simpleProfStartDumper();

struct SimpleProfScope {
  SimpleProfSlot* slot;
  long long start;
  long long childNanos = 0;
  SimpleProfScope* parent;
  static thread_local SimpleProfScope* current;
  static long long nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  explicit SimpleProfScope(SimpleProfSlot* s)
      : slot(s), start(nowNanos()), parent(current) {
    current = this;
  }
  ~SimpleProfScope() {
    long long total = nowNanos() - start;
    slot->inclusiveNanos.fetch_add(total, std::memory_order_relaxed);
    slot->selfNanos.fetch_add(total - childNanos, std::memory_order_relaxed);
    slot->calls.fetch_add(1, std::memory_order_relaxed);
    current = parent;
    if (parent)
      parent->childNanos += total;
  }
};

#define SPROF_CONCAT_(a, b) a##b
#define SPROF_CONCAT(a, b) SPROF_CONCAT_(a, b)
#define PROFILE \
  static SimpleProfSlot* SPROF_CONCAT(sprofSlot_, __LINE__) = simpleProfGetSlot(__PRETTY_FUNCTION__); \
  SimpleProfScope SPROF_CONCAT(sprofScope_, __LINE__)(SPROF_CONCAT(sprofSlot_, __LINE__));
#define PROFILE_BLOCK(name) \
  static SimpleProfSlot* SPROF_CONCAT(sprofSlot_, __LINE__) = simpleProfGetSlot(name); \
  SimpleProfScope SPROF_CONCAT(sprofScope_, __LINE__)(SPROF_CONCAT(sprofSlot_, __LINE__));
#define ENABLE_PROFILER simpleProfStartDumper()

#elif defined(SAMPLE_PROFILER)

// Statistical sampling profiler: a background thread periodically suspends the
// main (game-loop) thread, walks its stack, and aggregates by function. No
// per-call overhead, so it does NOT over-count high-frequency functions the way
// an instrumenting profiler does. Writes profile_report.txt every few seconds.
void sampleProfStart();
#define PROFILE
#define PROFILE_BLOCK(...)
#define ENABLE_PROFILER sampleProfStart()

#else

#define PROFILE
#define PROFILE_BLOCK(...)
#define ENABLE_PROFILER

#endif
