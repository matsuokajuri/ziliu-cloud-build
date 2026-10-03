#include "ziliu/core/context_request_guard.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using ziliu::core::ContextLifetimeToken;
using ziliu::core::ContextPrivacy;
using ziliu::core::ContextRequestGuard;
using ziliu::core::ContextRequestSnapshot;
using Guard = ContextRequestGuard;

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

ContextRequestSnapshot ValidSnapshot() {
  return ContextRequestSnapshot{
      .field_token = ContextLifetimeToken{1, 2},
      .field_epoch = 3,
      .edit_revision = 0,
      .selection_start_utf16 = 0,
      .selection_end_utf16 = 0,
      .input_revision = 1,
      .candidate_revision = 1,
      .broker_instance = {4, 5},
      .engine_session_id = 1,
      .dictionary_epoch = 5,
      .privacy = ContextPrivacy::ordinary,
      .identity_verified = true,
      .fresh_read_ok = true,
  };
}

constexpr Guard::TimePoint At(std::chrono::seconds seconds) {
  return Guard::TimePoint{} + seconds;
}

void TestInitialAndInvalidStateFailClosed() {
  Guard guard(ContextLifetimeToken{});
  Expect(guard.closed(), "zero owner token closes the guard");
  Expect(!guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(2))),
         "closed guard cannot issue tickets");

  Guard valid_guard(ContextLifetimeToken{10, 0});
  Expect(!valid_guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(2))),
         "guard without an observed context cannot issue tickets");
  auto unknown = ValidSnapshot();
  unknown.privacy = ContextPrivacy::unknown;
  Expect(!valid_guard.Observe(unknown), "unknown privacy is rejected");
  Expect(!valid_guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(2))),
         "rejected observation does not leave a usable context");
}

void TestHappyPathAndDuplicate() {
  Guard guard(ContextLifetimeToken{10, 20});
  const auto snapshot = ValidSnapshot();
  Expect(guard.Observe(snapshot), "valid fresh ordinary context is observed");
  const auto ticket = guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  Expect(ticket.has_value(), "valid request gets a ticket");
  Expect(guard.TryAccept(*ticket, snapshot, At(std::chrono::seconds(2))),
         "exact current response is accepted");
  Expect(!guard.TryAccept(*ticket, snapshot, At(std::chrono::seconds(2))),
         "successful ticket is consumed and duplicate rejected");
}

void TestLifetimeAndFieldAba() {
  Guard guard(ContextLifetimeToken{10, 20});
  auto before = ValidSnapshot();
  Expect(guard.Observe(before), "initial field state observed");
  const auto ticket = guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  Expect(ticket.has_value(), "request for field A issued");

  auto other_lifetime_same_numeric_state = before;
  other_lifetime_same_numeric_state.field_token = {9, 9};
  Expect(!guard.TryAccept(*ticket, other_lifetime_same_numeric_state, At(std::chrono::seconds(2))),
         "same field epoch and text-like revisions do not bridge field lifetime");
  Expect(!guard.BeginRequest(At(std::chrono::seconds(2)), At(std::chrono::seconds(6))),
         "A to B to A-style mismatch invalidates until a fresh observation");

  Expect(guard.Observe(before), "fresh A observation restores a valid context");
  const auto next = guard.BeginRequest(At(std::chrono::seconds(3)), At(std::chrono::seconds(6)));
  Expect(next.has_value(), "ticket after A re-observation is issued");
  Expect(!guard.TryAccept(*next, other_lifetime_same_numeric_state, At(std::chrono::seconds(4))),
         "reused context numerics cannot defeat the opaque lifetime token");

  Guard aba_guard(ContextLifetimeToken{10, 20});
  Expect(aba_guard.Observe(before), "A-B-A fixture observes field A");
  const auto old_a = aba_guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  auto field_b = before;
  field_b.field_token = {8, 8};
  Expect(old_a && aba_guard.Observe(field_b), "A-B-A fixture observes distinct field B");
  Expect(aba_guard.Observe(before), "A-B-A fixture observes original field A again");
  Expect(!aba_guard.TryAccept(*old_a, before, At(std::chrono::seconds(2))),
         "A-B-A observation sequence rejects the original A ticket");
  const auto fresh_a = aba_guard.BeginRequest(At(std::chrono::seconds(2)), At(std::chrono::seconds(6)));
  Expect(fresh_a && aba_guard.TryAccept(*fresh_a, before, At(std::chrono::seconds(3))),
         "fresh A ticket is accepted after the caller re-observes A");
}

void TestObserveIdenticalStateInvalidatesPreviousTicket() {
  Guard guard(ContextLifetimeToken{10, 20});
  const auto snapshot = ValidSnapshot();
  Expect(guard.Observe(snapshot), "initial observation succeeds");
  const auto stale = guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  Expect(stale.has_value(), "initial ticket issued");
  Expect(guard.Observe(snapshot), "identical observation is accepted");
  Expect(!guard.TryAccept(*stale, snapshot, At(std::chrono::seconds(2))),
         "every observation invalidates any pending ticket, including identical state");
  const auto fresh = guard.BeginRequest(At(std::chrono::seconds(2)), At(std::chrono::seconds(6)));
  Expect(fresh && guard.TryAccept(*fresh, snapshot, At(std::chrono::seconds(3))),
         "fresh ticket after identical observation works");
}

void TestSnapshotMutationInvalidates() {
  const auto base = ValidSnapshot();
  const auto now = At(std::chrono::seconds(1));
  const auto deadline = At(std::chrono::seconds(5));
  const auto check = [&](const ContextRequestSnapshot& changed, std::string_view label) {
    Guard guard(ContextLifetimeToken{30, 40});
    Expect(guard.Observe(base), "mutation fixture observed");
    const auto ticket = guard.BeginRequest(now, deadline);
    Expect(ticket.has_value(), "mutation fixture ticket issued");
    Expect(!guard.TryAccept(*ticket, changed, At(std::chrono::seconds(2))), label);
    Expect(!guard.BeginRequest(At(std::chrono::seconds(2)), At(std::chrono::seconds(6))),
           "mismatch clears context until a new observation");
  };

  auto changed = base;
  changed.edit_revision = 1;
  check(changed, "edit revision change rejects response");
  changed = base;
  changed.selection_end_utf16 = 2;
  check(changed, "selection change rejects response");
  changed = base;
  changed.selection_start_utf16 = 3;
  changed.selection_end_utf16 = 2;
  check(changed, "cleared/reversed invalid selection rejects response");
  changed = base;
  changed.input_revision += 1;
  check(changed, "input version change rejects response");
  changed = base;
  changed.candidate_revision += 1;
  check(changed, "candidate version change rejects response");
  changed = base;
  changed.broker_instance.high += 1;
  check(changed, "broker high-half change rejects response even with identical counters");
  changed = base;
  changed.broker_instance.low += 1;
  check(changed, "broker low-half change rejects response even with identical counters");
  changed = base;
  changed.broker_instance = {};
  check(changed, "missing fresh broker identity rejects response");
  changed = base;
  changed.engine_session_id += 1;
  check(changed, "recreated engine session rejects otherwise identical broker state");
  changed = base;
  changed.dictionary_epoch = 6;
  check(changed, "dictionary epoch change rejects response");

  changed = base;
  changed.field_epoch = 4;
  check(changed, "field epoch change rejects response");
  changed = base;
  changed.selection_start_utf16 = 1;
  changed.selection_end_utf16 = 2;
  check(changed, "valid moved selection rejects response");

  // These observations model caller-supplied clear/delete/undo events. The gate
  // does not detect editing; the caller must advance edit_revision even if the
  // restored text and selection numerics look the same.
  Guard aba_guard(ContextLifetimeToken{30, 40});
  Expect(aba_guard.Observe(base), "ABA fixture observed");
  const auto aba_ticket = aba_guard.BeginRequest(now, deadline);
  Expect(aba_ticket.has_value(), "ABA fixture ticket issued");
  auto cleared_or_edited = base;
  cleared_or_edited.edit_revision = 1;
  cleared_or_edited.selection_end_utf16 = 2;
  Expect(aba_guard.Observe(cleared_or_edited), "caller reports an edit/selection transition");
  auto restored_after_undo = base;
  restored_after_undo.edit_revision = 2;
  Expect(aba_guard.Observe(restored_after_undo), "caller reports restored content after undo");
  Expect(!aba_guard.TryAccept(*aba_ticket, restored_after_undo, At(std::chrono::seconds(2))),
         "caller-revisioned clear/edit/undo sequence rejects original ticket");
  const auto post_undo = aba_guard.BeginRequest(At(std::chrono::seconds(2)), At(std::chrono::seconds(6)));
  Expect(post_undo && aba_guard.TryAccept(*post_undo, restored_after_undo, At(std::chrono::seconds(3))),
         "fresh ticket accepts the caller's restored state");
}

void TestPrivacyReadAndIdentityFailClosed() {
  const auto valid = ValidSnapshot();
  const auto denied = [&](ContextRequestSnapshot changed, std::string_view label) {
    Guard guard(ContextLifetimeToken{30, 40});
    Expect(!guard.Observe(changed), label);
    Expect(!guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5))),
           "failed verification cannot issue request");
  };
  auto changed = valid;
  changed.privacy = ContextPrivacy::restricted;
  denied(changed, "restricted privacy is rejected");
  changed = valid;
  changed.privacy = ContextPrivacy::blocked;
  denied(changed, "blocked privacy is rejected");
  changed = valid;
  changed.privacy = ContextPrivacy::unknown;
  denied(changed, "unknown privacy is rejected");
  changed = valid;
  changed.fresh_read_ok = false;
  denied(changed, "failed fresh read is rejected");
  changed = valid;
  changed.identity_verified = false;
  denied(changed, "unverified focus/field identity is rejected");

  const auto reject_changed_fresh = [&](ContextRequestSnapshot fresh, std::string_view label) {
    Guard changed_guard(ContextLifetimeToken{31, 41});
    Expect(changed_guard.Observe(valid), "fresh-state fixture observed");
    const auto changed_ticket = changed_guard.BeginRequest(At(std::chrono::seconds(1)),
                                                            At(std::chrono::seconds(5)));
    Expect(changed_ticket.has_value(), "fresh-state fixture ticket issued");
    Expect(!changed_guard.TryAccept(*changed_ticket, fresh, At(std::chrono::seconds(2))), label);
    Expect(!changed_guard.BeginRequest(At(std::chrono::seconds(2)), At(std::chrono::seconds(6))),
           "changed fresh evidence clears context until re-observed");
  };
  changed = valid;
  changed.privacy = ContextPrivacy::restricted;
  reject_changed_fresh(changed, "restricted fresh privacy rejects response");
  changed = valid;
  changed.privacy = ContextPrivacy::blocked;
  reject_changed_fresh(changed, "blocked fresh privacy rejects response");
  changed = valid;
  changed.privacy = ContextPrivacy::unknown;
  reject_changed_fresh(changed, "unknown fresh privacy rejects response");
  changed = valid;
  changed.fresh_read_ok = false;
  reject_changed_fresh(changed, "failed fresh read rejects response");
  changed = valid;
  changed.identity_verified = false;
  reject_changed_fresh(changed, "lost field/focus identity rejects response");
  changed = valid;
  changed.input_revision = 0;
  reject_changed_fresh(changed, "unavailable fresh input revision rejects response");
  changed = valid;
  changed.candidate_revision = 0;
  reject_changed_fresh(changed, "unavailable fresh candidate revision rejects response");
  changed = valid;
  changed.dictionary_epoch = 0;
  reject_changed_fresh(changed, "lost dictionary version rejects a pending enhancement");

  Guard guard(ContextLifetimeToken{30, 40});
  Expect(guard.Observe(valid), "focus fixture observed");
  const auto ticket = guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  Expect(ticket.has_value(), "focus fixture ticket issued");
  guard.Invalidate();
  Expect(!guard.TryAccept(*ticket, valid, At(std::chrono::seconds(2))),
         "explicit focus/context invalidation rejects pending response");
}

void TestOutOfOrderAndForeignTickets() {
  const auto snapshot = ValidSnapshot();
  Guard guard(ContextLifetimeToken{10, 20});
  Expect(guard.Observe(snapshot), "out-of-order context observed");
  const auto first = guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(8)));
  const auto second = guard.BeginRequest(At(std::chrono::seconds(2)), At(std::chrono::seconds(8)));
  Expect(first && second && *first != *second, "new request supersedes older ticket");
  Expect(!guard.TryAccept(*first, snapshot, At(std::chrono::seconds(3))),
         "late first response is rejected");
  Expect(guard.TryAccept(*second, snapshot, At(std::chrono::seconds(3))),
         "current second response remains acceptable");

  Guard owner_a(ContextLifetimeToken{10, 20});
  Guard owner_b(ContextLifetimeToken{11, 21});
  Expect(owner_a.Observe(snapshot) && owner_b.Observe(snapshot), "two owners observe state");
  const auto owner_a_ticket = owner_a.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  const auto owner_b_ticket = owner_b.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  Expect(owner_a_ticket && owner_b_ticket, "both owners issue tickets");
  Expect(!owner_b.TryAccept(*owner_a_ticket, snapshot, At(std::chrono::seconds(2))),
         "foreign owner's ticket is rejected");
  Expect(owner_b.TryAccept(*owner_b_ticket, snapshot, At(std::chrono::seconds(2))),
         "foreign ticket does not cancel this owner's current request");

  Guard recreated(ContextLifetimeToken{12, 22});
  Expect(recreated.Observe(snapshot), "recreated guard observes state");
  const auto recreated_ticket = recreated.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  Expect(recreated_ticket.has_value(), "recreated guard issues its own ticket");
  Expect(!recreated.TryAccept(*owner_a_ticket, snapshot, At(std::chrono::seconds(2))),
         "reconstructed gate with a new owner token rejects old ticket");
}

void TestDeadlinesRollbackAndDeniedBegin() {
  const auto snapshot = ValidSnapshot();
  const auto now = At(std::chrono::seconds(3));
  Guard at_deadline(ContextLifetimeToken{10, 20});
  Expect(at_deadline.Observe(snapshot), "deadline context observed");
  const auto ticket = at_deadline.BeginRequest(now, At(std::chrono::seconds(5)));
  Expect(ticket.has_value(), "deadline ticket issued");
  Expect(!at_deadline.TryAccept(*ticket, snapshot, At(std::chrono::seconds(5))),
         "exact deadline is expired");
  Expect(!at_deadline.BeginRequest(now, At(std::chrono::seconds(6))),
         "timeout clears context and requires fresh observation");

  Guard rollback(ContextLifetimeToken{10, 20});
  Expect(rollback.Observe(snapshot), "rollback context observed");
  const auto rollback_ticket = rollback.BeginRequest(now, At(std::chrono::seconds(8)));
  Expect(rollback_ticket.has_value(), "rollback ticket issued");
  Expect(!rollback.TryAccept(*rollback_ticket, snapshot, At(std::chrono::seconds(2))),
         "clock before issue time is rejected");

  Guard denied(ContextLifetimeToken{10, 20});
  Expect(denied.Observe(snapshot), "invalid-begin context observed");
  const auto previous = denied.BeginRequest(now, At(std::chrono::seconds(8)));
  Expect(previous.has_value(), "prior ticket issued");
  Expect(!denied.BeginRequest(now, now), "non-positive deadline is denied");
  Expect(!denied.TryAccept(*previous, snapshot, At(std::chrono::seconds(4))),
         "denied begin clears prior pending ticket");
  const auto new_ticket = denied.BeginRequest(At(std::chrono::seconds(4)), At(std::chrono::seconds(8)));
  Expect(new_ticket && denied.TryAccept(*new_ticket, snapshot, At(std::chrono::seconds(5))),
         "valid begin after denial works without re-observation");
}

void TestInvalidSnapshotsAndCounterBoundary() {
  const auto base = ValidSnapshot();
  auto invalid = base;
  invalid.field_token = {};
  Expect(!Guard::CanAdvance(std::numeric_limits<std::uint64_t>::max()),
         "checked counter rule rejects uint64 maximum instead of wrapping");
  Expect(Guard::CanAdvance(std::numeric_limits<std::uint64_t>::max() - 1),
         "checked counter rule allows final safe increment");

  const auto invalid_observation = [&](ContextRequestSnapshot changed, std::string_view label) {
    Guard guard(ContextLifetimeToken{50, 60});
    Expect(!guard.Observe(changed), label);
    Expect(!guard.BeginRequest(At(std::chrono::seconds(1)), At(std::chrono::seconds(5))),
           "invalid identifiers or ranges fail closed");
  };
  invalid_observation(invalid, "all-zero field token rejected");
  invalid = base;
  invalid.field_epoch = 0;
  invalid_observation(invalid, "zero field epoch rejected");
  invalid = base;
  invalid.broker_instance = {};
  invalid_observation(invalid, "unavailable broker instance rejected");
  invalid = base;
  invalid.engine_session_id = 0;
  invalid_observation(invalid, "missing engine session id rejected");
  invalid = base;
  invalid.dictionary_epoch = 0;
  invalid_observation(invalid, "zero dictionary epoch rejected");
  invalid = base;
  invalid.input_revision = 0;
  invalid_observation(invalid, "unavailable input revision rejected");
  invalid = base;
  invalid.candidate_revision = 0;
  invalid_observation(invalid, "unavailable candidate revision rejected");
  invalid.input_revision = 0;
  invalid_observation(invalid, "legacy or exhausted revision pair rejected");
  invalid = base;
  invalid.selection_start_utf16 = 2;
  invalid.selection_end_utf16 = 1;
  invalid_observation(invalid, "unordered selection rejected");

  Guard zero_positions(ContextLifetimeToken{50, 60});
  Expect(zero_positions.Observe(base), "zero selection and edit revision remain valid");
  auto partial_instance = base;
  partial_instance.broker_instance = {1, 0};
  Expect(zero_positions.Observe(partial_instance), "high-only broker id is nonzero");
  partial_instance.broker_instance = {0, 1};
  Expect(zero_positions.Observe(partial_instance), "low-only broker id is nonzero");
}

}  // namespace

int main() {
  TestInitialAndInvalidStateFailClosed();
  TestHappyPathAndDuplicate();
  TestLifetimeAndFieldAba();
  TestObserveIdenticalStateInvalidatesPreviousTicket();
  TestSnapshotMutationInvalidates();
  TestPrivacyReadAndIdentityFailClosed();
  TestOutOfOrderAndForeignTickets();
  TestDeadlinesRollbackAndDeniedBegin();
  TestInvalidSnapshotsAndCounterBoundary();
  return EXIT_SUCCESS;
}
