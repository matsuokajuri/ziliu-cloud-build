#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace ziliu::core {

// Tracks whether the shared dictionary is safe to identify across all adapter
// operations. This is not a disk hash/version or a measure of Rime health.
class DictionaryEpoch final {
 public:
  DictionaryEpoch() noexcept = default;
  DictionaryEpoch(const DictionaryEpoch&) = delete;
  DictionaryEpoch& operator=(const DictionaryEpoch&) = delete;

  [[nodiscard]] bool EnableAfterInitialization() noexcept {
    std::uint64_t expected = 1;
    return value_.compare_exchange_strong(expected, 2, std::memory_order_release,
                                          std::memory_order_relaxed);
  }

  void Invalidate() noexcept { value_.store(0, std::memory_order_release); }

  [[nodiscard]] std::uint64_t Read() const noexcept {
    const auto value = value_.load(std::memory_order_acquire);
    return value >= 2 && (value & 1U) == 0 ? value : 0;
  }

  [[nodiscard]] std::uint64_t BeginOperation() noexcept {
    auto current = value_.load(std::memory_order_acquire);
    if (current == 0) return 0;
    if (current < 2 || (current & 1U) != 0 ||
        current == std::numeric_limits<std::uint64_t>::max() - 1) {
      Invalidate();
      return 0;
    }
    const auto token = current + 1;
    if (value_.compare_exchange_strong(current, token, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return token;
    }
    // Calls are serialized by contract; a competing operation is therefore
    // an unexpected state, while explicit invalidation remains terminal.
    if (current != 0) Invalidate();
    return 0;
  }

  [[nodiscard]] std::uint64_t EndOperation(std::uint64_t token) noexcept {
    if (token == 0) return 0;
    if (token < 3 || (token & 1U) == 0 ||
        token == std::numeric_limits<std::uint64_t>::max()) {
      Invalidate();
      return 0;
    }
    auto expected = token;
    if (value_.compare_exchange_strong(expected, token + 1, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return token + 1;
    }
    if (expected != 0) Invalidate();
    return 0;
  }

 private:
  friend struct DictionaryEpochTestAccess;
  std::atomic<std::uint64_t> value_{1};
};

}  // namespace ziliu::core
