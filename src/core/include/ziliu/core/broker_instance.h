#pragma once

#include <cstdint>

namespace ziliu::core {

// Identifies one PipeServer/session-host lifetime, not a credential or dictionary epoch.
// The all-zero value means the identity is unavailable.
struct BrokerInstanceId {
  std::uint64_t high{};
  std::uint64_t low{};

  [[nodiscard]] constexpr bool valid() const noexcept { return high != 0 || low != 0; }
  friend constexpr bool operator==(const BrokerInstanceId&, const BrokerInstanceId&) = default;
};

static_assert(sizeof(BrokerInstanceId) == 16);

}  // namespace ziliu::core
