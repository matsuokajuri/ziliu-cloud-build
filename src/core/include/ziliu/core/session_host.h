#pragma once

#include "ziliu/core/engine.h"
#include "ziliu/core/ipc_protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace ziliu::core {

// Advance the generations for an engine operation. If either counter is
// unavailable or would overflow, both remain permanently unavailable.
[[nodiscard]] inline bool AdvanceSessionRevisions(std::uint64_t* input_revision,
                                                  std::uint64_t* candidate_revision,
                                                  bool input_changed) noexcept {
  if (input_revision == nullptr || candidate_revision == nullptr ||
      input_revision == candidate_revision ||
      *input_revision == 0 || *candidate_revision == 0 ||
      *candidate_revision == UINT64_MAX ||
      (input_changed && *input_revision == UINT64_MAX)) {
    if (input_revision != nullptr) {
      *input_revision = 0;
    }
    if (candidate_revision != nullptr) {
      *candidate_revision = 0;
    }
    return false;
  }
  if (input_changed) {
    ++*input_revision;
  }
  ++*candidate_revision;
  return true;
}

class SessionHost final {
 public:
  using EngineFactory = std::function<std::unique_ptr<Engine>(bool restricted)>;

  explicit SessionHost(EngineFactory engine_factory = CreateStubEngineForSession);

  [[nodiscard]] ipc::Response Handle(const ipc::Request& request);
  [[nodiscard]] std::size_t session_count() const noexcept { return sessions_.size(); }

 private:
  struct Session final {
    explicit Session(std::unique_ptr<Engine> value) : engine(std::move(value)) {}

    std::unique_ptr<Engine> engine;
    std::uint64_t input_revision = 1;
    std::uint64_t candidate_revision = 1;
  };

  [[nodiscard]] std::uint64_t CreateSession(bool restricted);

  EngineFactory engine_factory_;
  std::unordered_map<std::uint64_t, Session> sessions_;
  std::uint64_t next_session_id_ = 1;
};

}  // namespace ziliu::core
