#include "ziliu/core/dictionary_epoch.h"

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <latch>
#include <thread>

namespace ziliu::core {
struct DictionaryEpochTestAccess {
  static void Set(DictionaryEpoch& epoch, std::uint64_t value) noexcept {
    epoch.value_.store(value, std::memory_order_relaxed);
  }
};
}  // namespace ziliu::core

namespace {
void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void TestLifecycle() {
  using ziliu::core::DictionaryEpoch;
  DictionaryEpoch epoch;
  Expect(epoch.Read() == 0, "initializing state is unavailable");
  Expect(epoch.EnableAfterInitialization(), "initialization enables generation two once");
  Expect(epoch.Read() == 2, "stable generation can be read");
  Expect(!epoch.EnableAfterInitialization(), "initialization cannot enable twice");
  const auto first = epoch.BeginOperation();
  Expect(first == 3 && epoch.Read() == 0, "operation-in-progress is unavailable");
  Expect(epoch.EndOperation(first) == 4 && epoch.Read() == 4,
         "completed operation advances stable generation");
  const auto second = epoch.BeginOperation();
  Expect(second == 5 && epoch.EndOperation(second) == 6,
         "each operation consumes a fresh generation");
}

void TestFailClosedStates() {
  using ziliu::core::DictionaryEpoch;
  DictionaryEpoch epoch;
  Expect(epoch.BeginOperation() == 0 && epoch.Read() == 0,
         "operation before initialization permanently invalidates");
  Expect(!epoch.EnableAfterInitialization() && epoch.Read() == 0,
         "initialization cannot revive invalid state");

  DictionaryEpoch overlap;
  Expect(overlap.EnableAfterInitialization(), "overlap fixture initializes");
  const auto token = overlap.BeginOperation();
  Expect(overlap.BeginOperation() == 0 && overlap.Read() == 0,
         "overlapping operation invalidates");
  Expect(overlap.EndOperation(token) == 0, "overlap completion cannot revive");

  DictionaryEpoch wrong_token;
  Expect(wrong_token.EnableAfterInitialization(), "wrong-token fixture initializes");
  const auto active = wrong_token.BeginOperation();
  Expect(wrong_token.EndOperation(active + 2) == 0 && wrong_token.Read() == 0,
         "wrong completion token invalidates");

  DictionaryEpoch before;
  Expect(before.EnableAfterInitialization(), "pre-invalidation fixture initializes");
  before.Invalidate();
  Expect(before.BeginOperation() == 0 && before.Read() == 0,
         "invalidation before operation is terminal");
  Expect(!before.EnableAfterInitialization(), "initialization cannot revive invalidation");

  DictionaryEpoch during;
  Expect(during.EnableAfterInitialization(), "mid-operation fixture initializes");
  const auto in_flight = during.BeginOperation();
  during.Invalidate();
  Expect(during.Read() == 0 && during.EndOperation(in_flight) == 0,
         "invalidation during operation is terminal");

  DictionaryEpoch after;
  Expect(after.EnableAfterInitialization(), "post-operation fixture initializes");
  const auto completed = after.BeginOperation();
  Expect(after.EndOperation(completed) == 4, "fixture operation completes");
  after.Invalidate();
  Expect(after.Read() == 0 && after.BeginOperation() == 0,
         "invalidation after completion is terminal");
  Expect(after.EndOperation(completed) == 0 && !after.EnableAfterInitialization() &&
             after.BeginOperation() == 0 && after.Read() == 0,
         "delayed completion and fresh operations cannot restore invalidation");

  DictionaryEpoch no_token;
  Expect(no_token.EnableAfterInitialization(), "zero-token fixture initializes");
  Expect(no_token.EndOperation(0) == 0 && no_token.Read() == 2,
         "zero token does not alter a stable generation");
}

void TestExhaustionAndConcurrentRead() {
  using ziliu::core::DictionaryEpoch;
  constexpr auto exhausted = std::numeric_limits<std::uint64_t>::max() - 1;
  DictionaryEpoch epoch;
  ziliu::core::DictionaryEpochTestAccess::Set(epoch, exhausted);
  Expect(epoch.Read() == exhausted, "last stable generation is readable");
  Expect(epoch.BeginOperation() == 0 && epoch.Read() == 0,
         "generation exhaustion invalidates instead of wrapping");

  DictionaryEpoch concurrent;
  Expect(concurrent.EnableAfterInitialization(), "concurrency fixture initializes");
  const auto token = concurrent.BeginOperation();
  std::latch ready(1);
  std::latch release(1);
  std::atomic<std::uint64_t> observed{99};
  std::jthread reader([&] {
    ready.count_down();
    release.wait();
    observed.store(concurrent.Read(), std::memory_order_relaxed);
  });
  ready.wait();
  release.count_down();
  reader.join();
  Expect(observed.load(std::memory_order_relaxed) == 0,
         "reader observes unavailable while mutation is in progress");
  Expect(concurrent.EndOperation(token) == 4, "serialized mutation can then finish");

  DictionaryEpoch racing;
  Expect(racing.EnableAfterInitialization(), "invalidation-race fixture initializes");
  const auto racing_token = racing.BeginOperation();
  std::barrier start(3);
  std::atomic<std::uint64_t> end_result{99};
  std::jthread completion([&] {
    start.arrive_and_wait();
    end_result.store(racing.EndOperation(racing_token), std::memory_order_relaxed);
  });
  std::jthread invalidation([&] {
    start.arrive_and_wait();
    racing.Invalidate();
  });
  start.arrive_and_wait();
  completion.join();
  invalidation.join();
  const auto raced_result = end_result.load(std::memory_order_relaxed);
  Expect((raced_result == 0 || raced_result == 4) && racing.Read() == 0,
         "racing invalidation is terminal and completion cannot restore it");

  DictionaryEpoch last_usable;
  ziliu::core::DictionaryEpochTestAccess::Set(last_usable,
      std::numeric_limits<std::uint64_t>::max() - 3);
  const auto last_token = last_usable.BeginOperation();
  Expect(last_token == std::numeric_limits<std::uint64_t>::max() - 2 &&
             last_usable.EndOperation(last_token) ==
                 std::numeric_limits<std::uint64_t>::max() - 1,
         "last usable operation reaches the final stable generation");
  Expect(last_usable.BeginOperation() == 0 && last_usable.Read() == 0,
         "beginning beyond the final stable generation invalidates");
}
}  // namespace

int main() {
  TestLifecycle();
  TestFailClosedStates();
  TestExhaustionAndConcurrentRead();
}
