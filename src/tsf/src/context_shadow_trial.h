#pragma once

#include "context_event_probe.h"
#include <array>
#include <chrono>

namespace ziliu::tsf::detail {

struct ShadowOutcome {
  // 0 unused, 1 armed, 2 delivered, 3 denied, 4 too early, 5 no pending.
  int phase = 0;
  unsigned sequence = 0;
  int scenario = -1;
  bool accepted = false;
  long long age_ms = 0;
};

// TEST ONLY. F8/F9 control delivery on the TSF apartment, without a worker,
// timer or edit session. This deliberately identical synthetic snapshot tests
// callback cancellation, NOT actual field identity or fresh document reads.
class ContextShadowTrial final {
 public:
  using TimePoint = core::ContextRequestGuard::TimePoint;

  ShadowOutcome Arm(ContextEventProbe& probe, int scenario, TimePoint now,
                    bool fresh_read_ok = true) {
    if (scenario < 0 || scenario > 7 || attempts_ >= 32) {
      probe.CancelSyntheticRequests();
      Clear();
      return {3, attempts_, scenario};
    }
    ++attempts_;
    if (!fresh_read_ok) {
      probe.CancelSyntheticRequests();
      Clear();
      return {3, attempts_, scenario};
    }
    if (scenario != scenario_) {
      probe.CancelSyntheticRequests();
      Clear();
      scenario_ = scenario;
    }
    if (slots_[1].ticket) {
      probe.CancelSyntheticRequests();
      Clear();
      return {3, attempts_, scenario};
    }
    if (!probe.ObserveSyntheticRequest(SyntheticState())) {
      Clear();
      return {3, attempts_, scenario};
    }
    const auto ticket = probe.BeginSyntheticRequest(now, now + std::chrono::seconds(30));
    if (!ticket) {
      Clear();
      return {3, attempts_, scenario};
    }
    auto& slot = slots_[0].ticket ? slots_[1] : slots_[0];
    slot = {ticket, attempts_};
    return {1, attempts_, scenario};
  }

  std::array<ShadowOutcome, 2> Deliver(ContextEventProbe& probe, int scenario, TimePoint now,
                                     bool fresh_read_ok = true) {
    std::array<ShadowOutcome, 2> result{};
    if (!fresh_read_ok) probe.CancelSyntheticRequests();
    if (scenario != scenario_ || (!slots_[0].ticket && !slots_[1].ticket)) {
      probe.CancelSyntheticRequests();
      Clear();
      result[0] = {5, 0, scenario};
      return result;
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (!slot.ticket) continue;
      const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - slot.ticket->issued_at).count();
      if (age >= 0 && age < 2000) {
        result[i] = {4, slot.sequence, scenario, false, age};
        continue;
      }
      auto snapshot = SyntheticState();
      // Fresh fixture reads are an additional veto, never proof of field identity.
      snapshot.fresh_read_ok = fresh_read_ok;
      const bool accepted = probe.TryAcceptSyntheticResult(*slot.ticket, snapshot, now);
      result[i] = {2, slot.sequence, scenario, accepted, age};
      slot = {};
    }
    return result;
  }

  void Clear() noexcept { slots_ = {}; scenario_ = -1; }

 private:
  static core::ContextRequestSnapshot SyntheticState() noexcept {
    // These true flags describe fabricated test data only. Never use this
    // factory in production or claim it validates a browser input field.
    return {.field_token = {0x534841444f57, 1}, .field_epoch = 1,
            .edit_revision = 1, .selection_start_utf16 = 57, .selection_end_utf16 = 57,
            .input_revision = 1, .candidate_revision = 1, .broker_instance = {1, 1},
            .engine_session_id = 1, .dictionary_epoch = 1,
            .privacy = core::ContextPrivacy::ordinary,
            .identity_verified = true, .fresh_read_ok = true};
  }
  struct Slot {
    std::optional<core::ContextRequestTicket> ticket;
    unsigned sequence = 0;
  };
  std::array<Slot, 2> slots_{};
  int scenario_ = -1;
  unsigned attempts_ = 0;
};

}  // namespace ziliu::tsf::detail
