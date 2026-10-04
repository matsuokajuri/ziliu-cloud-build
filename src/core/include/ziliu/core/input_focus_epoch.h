#pragma once

#include <cstdint>
#include <limits>

namespace ziliu::core {

// Owner-thread revision for bracketing reentrant host calls. A focus round trip
// invalidates old work even when the host reuses the same context and selection.
// This revision is not a field identity or a privacy authorization.
class InputFocusEpoch final {
 public:
  [[nodiscard]] std::uint64_t Capture() const noexcept { return revision_; }
  [[nodiscard]] bool IsCurrent(std::uint64_t revision) const noexcept {
    return revision != 0 && revision == revision_;
  }
  void Invalidate() noexcept {
    revision_ = Next(revision_);
  }
  [[nodiscard]] static constexpr std::uint64_t Next(std::uint64_t revision) noexcept {
    return revision == 0 || revision == std::numeric_limits<std::uint64_t>::max()
               ? 0 : revision + 1;
  }

 private:
  std::uint64_t revision_ = 1;
};

}  // namespace ziliu::core
