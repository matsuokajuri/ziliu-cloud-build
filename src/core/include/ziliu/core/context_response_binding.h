#pragma once

#include "ziliu/core/context_prefix_binding.h"
#include "ziliu/core/ipc_protocol.h"

namespace ziliu::core {

// Caller-thread-only binding of a candidate response to fresh field evidence.
// IPC correlation is not field identity: the caller supplies independently
// verified field/privacy/caret evidence and invalidates on local state changes.
// A state query is an extra veto, NOT a lock across IPC and candidate publication.
// Fresh-query IDs must follow their source request without wrapping. The
// caller must perform the supplied fresh query, not replay a cached exchange.
// No model result or candidate is applied by this class.
class ContextResponseBinding final {
 public:
  using TimePoint = ContextPrefixBinding::TimePoint;

  explicit ContextResponseBinding(ContextLifetimeToken owner) noexcept : binding_(owner) {}

  [[nodiscard]] std::optional<ContextRequestTicket> Begin(
      ContextRequestSnapshot field, std::u16string_view prefix,
      const ipc::Request& request, const ipc::Response& response,
      TimePoint now, TimePoint deadline) noexcept {
    source_request_id_ = request.request_id;
    if (!IsCandidateOperation(request.command) || !Correlates(request, response) ||
        !response.commit.empty() || response.snapshot.empty() ||
        response.snapshot.candidates.empty() ||
        response.snapshot.candidates.size() > ipc::kMaximumCandidates ||
        response.snapshot.highlighted_index >= response.snapshot.candidates.size()) {
      binding_.Invalidate();
      return std::nullopt;
    }
    return binding_.Begin(WithEngineState(field, response), prefix, now, deadline);
  }

  [[nodiscard]] bool TryAccept(
      const ContextRequestTicket& ticket, ContextRequestSnapshot fresh_field,
      std::u16string_view fresh_prefix, const ipc::Request& query,
      const ipc::Response& response, TimePoint now) noexcept {
    const bool valid_query = query.command == ipc::Command::kGetSessionState &&
        query.request_id > source_request_id_ &&
        query.value == 0 && query.theme_id.empty() && query.resource.empty() &&
        query.point_x == 0 && query.point_y == 0 && Correlates(query, response) &&
        !response.consumed && response.commit.empty() && response.snapshot.empty() &&
        response.snapshot.candidates.empty() && response.snapshot.highlighted_index == 0 &&
        !response.snapshot.has_previous_page && !response.snapshot.has_next_page &&
        response.settings_text.empty() && response.theme_chunk.empty();
    // Fail the matching ticket without allowing a stale/foreign ticket to wipe
    // a newer binding. Never reuse caller-cached engine metadata on query error.
    const ipc::Response unavailable{};
    return binding_.TryAccept(ticket,
        WithEngineState(fresh_field, valid_query ? response : unavailable),
        fresh_prefix, now);
  }

  void Invalidate() noexcept { binding_.Invalidate(); }
  [[nodiscard]] bool Expire(TimePoint now) noexcept { return binding_.Expire(now); }

 private:
  [[nodiscard]] static bool IsCandidateOperation(ipc::Command command) noexcept {
    switch (command) {
      case ipc::Command::kInputLetter:
      case ipc::Command::kInputSeparator:
      case ipc::Command::kBackspace:
      case ipc::Command::kPageUp:
      case ipc::Command::kPageDown:
      case ipc::Command::kSetTraditional:
      case ipc::Command::kSetCandidatePageSize:
      case ipc::Command::kSetCandidateWindowPageCount:
      case ipc::Command::kSetChineseCandidatesOnly:
        return true;
      default:
        return false;
    }
  }

  [[nodiscard]] static bool Correlates(const ipc::Request& request,
                                      const ipc::Response& response) noexcept {
    return request.request_id != 0 && request.session_id != 0 &&
        response.status == ipc::Status::kOk && response.request_id == request.request_id &&
        response.session_id == request.session_id;
  }

  [[nodiscard]] static ContextRequestSnapshot WithEngineState(
      ContextRequestSnapshot field, const ipc::Response& response) noexcept {
    field.input_revision = response.input_revision;
    field.candidate_revision = response.candidate_revision;
    field.broker_instance = response.broker_instance;
    field.engine_session_id = response.session_id;
    field.dictionary_epoch = response.dictionary_epoch;
    return field;
  }

  ContextPrefixBinding binding_;
  std::uint64_t source_request_id_{};
};

}  // namespace ziliu::core
