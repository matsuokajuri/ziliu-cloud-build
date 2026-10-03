#include "ziliu/core/context_prefix_binding.h"
#include "ziliu/core/context_response_binding.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <optional>
#include <string_view>
#include <type_traits>

namespace {

using ziliu::core::ContextLifetimeToken;
using ziliu::core::ContextPrefixBinding;
using ziliu::core::ContextPrivacy;
using ziliu::core::ContextRequestSnapshot;
using Binding = ContextPrefixBinding;

std::size_t expectation_count = 0;

void Expect(bool condition, std::string_view message) {
  ++expectation_count;
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
      .selection_start_utf16 = 256,
      .selection_end_utf16 = 256,
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

constexpr Binding::TimePoint At(std::chrono::seconds seconds) {
  return Binding::TimePoint{} + seconds;
}

std::optional<ziliu::core::ContextRequestTicket> Begin(
    Binding& binding, const ContextRequestSnapshot& snapshot, std::u16string_view prefix,
    std::chrono::seconds issue = std::chrono::seconds(1),
    std::chrono::seconds deadline = std::chrono::seconds(5)) {
  return binding.Begin(snapshot, prefix, At(issue), At(deadline));
}

void TestCopyAndValidUtf16Acceptance() {
  static_assert(!std::is_copy_constructible_v<Binding>);
  static_assert(!std::is_copy_assignable_v<Binding>);
  static_assert(!std::is_move_constructible_v<Binding>);
  static_assert(!std::is_move_assignable_v<Binding>);

  Binding binding(ContextLifetimeToken{10, 20});
  auto input = std::array<char16_t, 9>{u'a', u'r', u'b', u'i', u't', u'r', u'a', u'r', u'y'};
  const auto snapshot = ValidSnapshot();
  const auto ticket = Begin(binding, snapshot, std::u16string_view(input.data(), input.size()));
  Expect(ticket.has_value(), "arbitrary non-fixture UTF-16 prefix gets a ticket");
  input.fill(u'x');
  Expect(binding.TryAccept(*ticket, snapshot, u"arbitrary", At(std::chrono::seconds(2))),
         "request owns a copy independent of caller buffer mutation");
  Expect(!binding.TryAccept(*ticket, snapshot, u"arbitrary", At(std::chrono::seconds(2))),
         "accepted response consumes the binding exactly once");

  const char16_t with_pair[] = {u'c', u'a', u'f', u'e', u' ', 0xD83E, 0xDDEE};
  const auto pair_ticket = Begin(binding, snapshot,
                                 std::u16string_view(with_pair, std::size(with_pair)));
  Expect(pair_ticket.has_value(), "well-formed surrogate pair is accepted intact");
  Expect(binding.TryAccept(*pair_ticket, snapshot,
                           std::u16string_view(with_pair, std::size(with_pair)),
                           At(std::chrono::seconds(2))),
         "matching prefix containing a surrogate pair is accepted");
}

void TestExactLengthAndContentMatching() {
  const auto snapshot = ValidSnapshot();
  const auto reject_current = [&](std::u16string_view request, std::u16string_view fresh,
                                  std::string_view label) {
    Binding binding(ContextLifetimeToken{30, 40});
    const auto ticket = Begin(binding, snapshot, request);
    Expect(ticket.has_value(), "prefix comparison fixture starts");
    Expect(!binding.TryAccept(*ticket, snapshot, fresh, At(std::chrono::seconds(2))), label);
    Expect(!binding.TryAccept(*ticket, snapshot, request, At(std::chrono::seconds(2))),
           "mismatched current response consumes and clears its binding");
  };

  reject_current(u"abcdef", u"abcxef", "same-length different prefix is rejected");
  reject_current(u"abcdef", u"abcde", "shortened prefix is rejected");
  reject_current(u"abcdef", u"abcdefg", "longer prefix is rejected");
  reject_current(u"a", u"", "empty fresh prefix is rejected");
}

void TestBoundsAndInvalidUtf16Reject() {
  const auto snapshot = ValidSnapshot();
  Binding binding(ContextLifetimeToken{50, 60});
  std::array<char16_t, Binding::kMaxPrefixUtf16Units> maximum{};
  maximum.fill(u'm');
  const auto max_ticket = Begin(binding, snapshot,
                                std::u16string_view(maximum.data(), maximum.size()));
  Expect(max_ticket.has_value(), "exactly 128 UTF-16 units are accepted");
  Expect(binding.TryAccept(*max_ticket, snapshot,
                           std::u16string_view(maximum.data(), maximum.size()),
                           At(std::chrono::seconds(2))),
         "exact maximum prefix compares and accepts");

  std::array<char16_t, Binding::kMaxPrefixUtf16Units + 1> oversized{};
  oversized.fill(u'o');
  Expect(!Begin(binding, snapshot,
                std::u16string_view(oversized.data(), oversized.size())),
         "129 UTF-16 units reject instead of truncating");
  Expect(!Begin(binding, snapshot, u""), "empty request prefix is rejected");

  const char16_t nul[] = {u'a', u'\0', u'b'};
  Expect(!Begin(binding, snapshot, std::u16string_view(nul, std::size(nul))),
         "embedded NUL is rejected");
  const char16_t high_only[] = {u'a', 0xD800};
  Expect(!Begin(binding, snapshot, std::u16string_view(high_only, std::size(high_only))),
         "unpaired high surrogate is rejected");
  const char16_t low_only[] = {0xDC00};
  Expect(!Begin(binding, snapshot, std::u16string_view(low_only, std::size(low_only))),
         "unpaired low surrogate is rejected");
  const char16_t wrong_pair[] = {0xD800, u'x'};
  Expect(!Begin(binding, snapshot, std::u16string_view(wrong_pair, std::size(wrong_pair))),
         "high surrogate followed by non-low surrogate is rejected");
}

void TestSnapshotPrivacyIdentityAndCaretContract() {
  const auto valid = ValidSnapshot();
  const auto denied_begin = [&](ContextRequestSnapshot denied, std::string_view label) {
    Binding binding(ContextLifetimeToken{70, 80});
    Expect(!Begin(binding, denied, u"prefix"), label);
  };

  auto changed = valid;
  changed.privacy = ContextPrivacy::unknown;
  denied_begin(changed, "unknown request privacy fails closed");
  changed = valid;
  changed.privacy = ContextPrivacy::blocked;
  denied_begin(changed, "blocked request privacy fails closed");
  changed = valid;
  changed.privacy = ContextPrivacy::restricted;
  denied_begin(changed, "restricted request privacy fails closed");
  changed = valid;
  changed.identity_verified = false;
  denied_begin(changed, "unverified identity is not inferred from matching text");
  changed = valid;
  changed.fresh_read_ok = false;
  denied_begin(changed, "failed fresh request read is rejected");
  changed = valid;
  changed.input_revision = 0;
  denied_begin(changed, "unavailable input revision rejects prefix binding");
  changed = valid;
  changed.candidate_revision = 0;
  denied_begin(changed, "unavailable candidate revision rejects prefix binding");
  changed = valid;
  changed.selection_end_utf16 = 257;
  Binding selection_binding(ContextLifetimeToken{71, 81});
  Expect(!Begin(selection_binding, changed, u"prefix"),
         "nonempty request selection is rejected by caret-only contract");

  auto zero_caret = valid;
  zero_caret.selection_start_utf16 = 0;
  zero_caret.selection_end_utf16 = 0;
  Binding zero_caret_binding(ContextLifetimeToken{73, 83});
  Expect(!Begin(zero_caret_binding, zero_caret, u"prefix"),
         "nonempty prefix cannot be bound at the start of the field");

  auto short_caret = valid;
  short_caret.selection_start_utf16 = 3;
  short_caret.selection_end_utf16 = 3;
  Binding short_caret_binding(ContextLifetimeToken{74, 84});
  Expect(!Begin(short_caret_binding, short_caret, u"prefix"),
         "prefix longer than absolute caret offset is rejected");

  Binding binding(ContextLifetimeToken{72, 82});
  const auto ticket = Begin(binding, valid, u"prefix");
  Expect(ticket.has_value(), "fresh-state fixture starts");
  changed = valid;
  changed.edit_revision += 1;
  Expect(!binding.TryAccept(*ticket, changed, u"prefix", At(std::chrono::seconds(2))),
         "fresh metadata change rejects matching prefix");

  const auto selection_ticket = Begin(binding, valid, u"prefix");
  Expect(selection_ticket.has_value(), "fresh selection fixture starts");
  changed = valid;
  changed.selection_start_utf16 = 100;
  changed.selection_end_utf16 = 100;
  Expect(!binding.TryAccept(*selection_ticket, changed, u"prefix", At(std::chrono::seconds(2))),
         "fresh selection movement rejects matching prefix");
}

void TestFreshPrivacyAndPrefixFailuresConsumeCurrentTicket() {
  const auto valid = ValidSnapshot();
  const auto reject_fresh_snapshot = [&](ContextRequestSnapshot fresh,
                                         std::string_view label) {
    Binding binding(ContextLifetimeToken{75, 85});
    const auto ticket = Begin(binding, valid, u"prefix");
    Expect(ticket.has_value(), "fresh evidence failure fixture starts");
    Expect(!binding.TryAccept(*ticket, fresh, u"prefix", At(std::chrono::seconds(2))), label);
    Expect(!binding.TryAccept(*ticket, valid, u"prefix", At(std::chrono::seconds(2))),
           "failed fresh evidence consumes and clears current ticket");
  };

  auto fresh = valid;
  fresh.privacy = ContextPrivacy::restricted;
  reject_fresh_snapshot(fresh, "restricted fresh privacy rejects matching prefix");
  fresh = valid;
  fresh.privacy = ContextPrivacy::blocked;
  reject_fresh_snapshot(fresh, "blocked fresh privacy rejects matching prefix");
  fresh = valid;
  fresh.privacy = ContextPrivacy::unknown;
  reject_fresh_snapshot(fresh, "unknown fresh privacy rejects matching prefix");
  fresh = valid;
  fresh.identity_verified = false;
  reject_fresh_snapshot(fresh, "unverified fresh identity rejects matching prefix");
  fresh = valid;
  fresh.fresh_read_ok = false;
  reject_fresh_snapshot(fresh, "failed fresh read rejects matching prefix");
  fresh = valid;
  fresh.input_revision = 0;
  fresh.candidate_revision = 0;
  reject_fresh_snapshot(fresh, "unavailable fresh revisions reject matching prefix");
  fresh = valid;
  fresh.broker_instance.low += 1;
  reject_fresh_snapshot(fresh, "restarted broker rejects matching prefix and revisions");
  fresh = valid;
  fresh.engine_session_id += 1;
  reject_fresh_snapshot(fresh, "recreated engine session rejects matching prefix and revisions");
  fresh = valid;
  fresh.selection_start_utf16 = 256;
  fresh.selection_end_utf16 = 257;
  reject_fresh_snapshot(fresh, "nonempty fresh selection rejects matching prefix");

  Binding invalid_prefix_binding(ContextLifetimeToken{76, 86});
  const auto invalid_prefix_ticket = Begin(invalid_prefix_binding, valid, u"prefix");
  const char16_t malformed[] = {0xD800};
  Expect(invalid_prefix_ticket &&
             !invalid_prefix_binding.TryAccept(*invalid_prefix_ticket, valid,
                                               std::u16string_view(malformed, 1),
                                               At(std::chrono::seconds(2))),
         "malformed fresh UTF-16 rejects matching ticket");

  Binding nul_binding(ContextLifetimeToken{77, 87});
  const auto nul_ticket = Begin(nul_binding, valid, u"prefix");
  const char16_t with_nul[] = {u'p', u'\0'};
  Expect(nul_ticket && !nul_binding.TryAccept(*nul_ticket, valid,
                                              std::u16string_view(with_nul, 2),
                                              At(std::chrono::seconds(2))),
         "NUL in fresh prefix rejects matching ticket");

  Binding oversized_binding(ContextLifetimeToken{78, 88});
  const auto oversized_ticket = Begin(oversized_binding, valid, u"prefix");
  std::array<char16_t, Binding::kMaxPrefixUtf16Units + 1> oversized{};
  oversized.fill(u'x');
  Expect(oversized_ticket &&
             !oversized_binding.TryAccept(*oversized_ticket, valid,
                                          std::u16string_view(oversized.data(), oversized.size()),
                                          At(std::chrono::seconds(2))),
         "oversized fresh prefix rejects matching ticket");
}

void TestExpiryRollbackAndNoPendingMaintenance() {
  const auto snapshot = ValidSnapshot();
  Binding before_deadline(ContextLifetimeToken{90, 100});
  const auto live = Begin(before_deadline, snapshot, u"prefix");
  Expect(live.has_value(), "expiry fixture starts");
  Expect(!before_deadline.Expire(At(std::chrono::seconds(4))),
         "maintenance before deadline leaves request pending");
  Expect(before_deadline.TryAccept(*live, snapshot, u"prefix", At(std::chrono::seconds(4))),
         "request survives maintenance before deadline");
  Expect(!before_deadline.Expire(At(std::chrono::seconds(5))),
         "maintenance with no pending request is a no-op");

  Binding at_deadline(ContextLifetimeToken{91, 101});
  const auto expiring = Begin(at_deadline, snapshot, u"prefix");
  Expect(expiring.has_value(), "deadline fixture starts");
  Expect(at_deadline.Expire(At(std::chrono::seconds(5))),
         "maintenance at deadline expires pending request");
  Expect(!at_deadline.TryAccept(*expiring, snapshot, u"prefix", At(std::chrono::seconds(4))),
         "expired request cannot be accepted");

  Binding direct_deadline(ContextLifetimeToken{93, 103});
  const auto direct_deadline_ticket = Begin(direct_deadline, snapshot, u"prefix");
  Expect(direct_deadline_ticket.has_value(), "direct deadline fixture starts");
  Expect(!direct_deadline.TryAccept(*direct_deadline_ticket, snapshot, u"prefix",
                                    At(std::chrono::seconds(5))),
         "direct accept at deadline fails closed");

  Binding direct_rollback(ContextLifetimeToken{94, 104});
  const auto direct_rollback_ticket = Begin(direct_rollback, snapshot, u"prefix");
  Expect(direct_rollback_ticket.has_value(), "direct rollback fixture starts");
  Expect(!direct_rollback.TryAccept(*direct_rollback_ticket, snapshot, u"prefix",
                                    At(std::chrono::seconds(0))),
         "direct accept before issue time fails closed");

  Binding rollback(ContextLifetimeToken{92, 102});
  const auto rollback_ticket = Begin(rollback, snapshot, u"prefix");
  Expect(rollback_ticket.has_value(), "rollback fixture starts");
  Expect(rollback.Expire(At(std::chrono::seconds(0))),
         "maintenance before issue time fails closed");
  Expect(!rollback.TryAccept(*rollback_ticket, snapshot, u"prefix", At(std::chrono::seconds(2))),
         "clock rollback invalidates pending request");
}

void TestDeniedReplacementInvalidatesPreviousBinding() {
  const auto snapshot = ValidSnapshot();
  Binding binding(ContextLifetimeToken{110, 120});
  const auto previous = Begin(binding, snapshot, u"prior");
  Expect(previous.has_value(), "replacement fixture starts");
  Expect(!Begin(binding, snapshot, u"", std::chrono::seconds(2), std::chrono::seconds(5)),
         "denied replacement attempt has no ticket");
  Expect(!binding.TryAccept(*previous, snapshot, u"prior", At(std::chrono::seconds(3))),
         "denied replacement clears older request and prefix");
  const auto current = Begin(binding, snapshot, u"current", std::chrono::seconds(3),
                             std::chrono::seconds(6));
  Expect(current && binding.TryAccept(*current, snapshot, u"current", At(std::chrono::seconds(4))),
         "fresh request after denied replacement works");

  const auto deadline_prior = Begin(binding, snapshot, u"prior2", std::chrono::seconds(5),
                                    std::chrono::seconds(8));
  Expect(deadline_prior.has_value(), "deadline replacement fixture starts");
  Expect(!Begin(binding, snapshot, u"valid", std::chrono::seconds(6), std::chrono::seconds(6)),
         "nonpositive replacement deadline is denied");
  Expect(!binding.TryAccept(*deadline_prior, snapshot, u"prior2", At(std::chrono::seconds(7))),
         "invalid-deadline replacement clears prior binding");
}

void TestStaleForeignAndAbaResponsesPreserveCurrentBinding() {
  const auto snapshot_a = ValidSnapshot();
  auto snapshot_b = snapshot_a;
  snapshot_b.field_token = {2, 3};
  Binding binding(ContextLifetimeToken{130, 140});
  const auto first_a = Begin(binding, snapshot_a, u"prefix-A");
  const auto ticket_b = Begin(binding, snapshot_b, u"prefix-B", std::chrono::seconds(2),
                              std::chrono::seconds(7));
  const auto second_a = Begin(binding, snapshot_a, u"prefix-A", std::chrono::seconds(3),
                              std::chrono::seconds(8));
  Expect(first_a && ticket_b && second_a, "A-B-A request sequence starts each request");
  Expect(!binding.TryAccept(*first_a, snapshot_a, u"prefix-A", At(std::chrono::seconds(4))),
         "original A response is stale after A-B-A");

  auto foreign = *second_a;
  foreign.request_id += 100;
  const char16_t malformed[] = {0xD800};
  Expect(!binding.TryAccept(foreign, snapshot_a, std::u16string_view(malformed, 1),
                            At(std::chrono::seconds(4))),
         "foreign ticket with invalid fresh data is rejected before inspecting data");
  Expect(!binding.TryAccept(*first_a, snapshot_a, std::u16string_view(malformed, 1),
                            At(std::chrono::seconds(4))),
         "stale ticket with invalid fresh data is rejected before inspecting data");
  Expect(binding.TryAccept(*second_a, snapshot_a, u"prefix-A", At(std::chrono::seconds(4))),
         "stale and foreign tickets do not erase current A binding");

  Binding owner_a(ContextLifetimeToken{150, 160});
  Binding owner_b(ContextLifetimeToken{151, 161});
  const auto a_ticket = Begin(owner_a, snapshot_a, u"owner-A");
  const auto b_ticket = Begin(owner_b, snapshot_a, u"owner-B");
  Expect(a_ticket && b_ticket, "two independent bindings start");
  Expect(!owner_b.TryAccept(*a_ticket, snapshot_a, u"owner-A", At(std::chrono::seconds(2))),
         "ticket from another binding cannot accept or clear this binding");
  Expect(owner_b.TryAccept(*b_ticket, snapshot_a, u"owner-B", At(std::chrono::seconds(2))),
         "foreign owner response preserves this binding");
}

void TestInvalidatePreventsTicketRevival() {
  const auto snapshot_a = ValidSnapshot();
  auto snapshot_b = snapshot_a;
  snapshot_b.field_token = {7, 8};
  Binding binding(ContextLifetimeToken{170, 180});
  const auto old_a = Begin(binding, snapshot_a, u"same");
  Expect(old_a.has_value(), "invalidation fixture starts");
  binding.Invalidate();
  Expect(!binding.TryAccept(*old_a, snapshot_a, u"same", At(std::chrono::seconds(2))),
         "explicit invalidation rejects old ticket");
  const auto ticket_b = Begin(binding, snapshot_b, u"other", std::chrono::seconds(2),
                              std::chrono::seconds(6));
  const auto new_a = Begin(binding, snapshot_a, u"same", std::chrono::seconds(3),
                           std::chrono::seconds(7));
  Expect(ticket_b && new_a, "fresh B then A requests are permitted");
  Expect(!binding.TryAccept(*old_a, snapshot_a, u"same", At(std::chrono::seconds(4))),
         "old A ticket cannot revive after B-A observations");
  Expect(binding.TryAccept(*new_a, snapshot_a, u"same", At(std::chrono::seconds(4))),
         "new A ticket accepts after explicit re-observation");
}

void TestEngineResponseCorrelation() {
  using namespace ziliu::core;
  using ipc::Command;
  const ipc::Request input{10, 7, Command::kInputLetter, L'o'};
  ipc::Response candidates;
  candidates.request_id = input.request_id;
  candidates.session_id = input.session_id;
  candidates.snapshot.preedit = L"ni'hao";
  candidates.snapshot.candidates.push_back({L"你好", L"", 0});
  candidates.input_revision = 4;
  candidates.candidate_revision = 5;
  candidates.broker_instance = {6, 7};
  candidates.dictionary_epoch = 8;
  const ipc::Request query{11, 7, Command::kGetSessionState, 0};
  auto current = candidates;
  current.request_id = query.request_id;
  current.snapshot = {};
  const auto field = ValidSnapshot();
  ContextResponseBinding binding({1000, 2000});
  const auto begin = [&](const ipc::Response& response) {
    return binding.Begin(field, u"prefix", input, response,
                         At(std::chrono::seconds(1)), At(std::chrono::seconds(5)));
  };
  const auto accept = [&](const ContextRequestTicket& ticket, const ipc::Request& request,
                          const ipc::Response& response) {
    return binding.TryAccept(ticket, field, u"prefix", request, response,
                             At(std::chrono::seconds(2)));
  };
  const auto first = begin(candidates);
  Expect(first && accept(*first, query, current),
         "wire metadata replaces caller-cached engine state and matches a fresh query");
  Expect(!accept(*first, query, current), "wire binding consumes a successful response once");

  const auto rejected_response = [&](ipc::Response bad, std::string_view reason) {
    const auto ticket = begin(candidates);
    Expect(ticket && !accept(*ticket, query, bad), reason);
    Expect(!accept(*ticket, query, current), "bad current reply permanently consumes its ticket");
  };
  auto bad = current;
  ++bad.dictionary_epoch;
  rejected_response(bad, "another session's dictionary change rejects identical text");
  bad = current;
  bad.dictionary_epoch = 0;
  rejected_response(bad, "unknown dictionary never falls back to caller's cached nonzero epoch");
  bad = current;
  ++bad.input_revision;
  rejected_response(bad, "changed input revision rejects identical prefix");
  bad = current;
  ++bad.candidate_revision;
  rejected_response(bad, "changed candidate revision rejects identical prefix");
  bad = current;
  ++bad.broker_instance.low;
  rejected_response(bad, "broker restart with reused session id rejects");
  bad = current;
  ++bad.session_id;
  rejected_response(bad, "wrong-session reply rejects");
  bad = current;
  ++bad.request_id;
  rejected_response(bad, "wrong-request reply rejects");
  bad = current;
  bad.status = ipc::Status::kSessionNotFound;
  rejected_response(bad, "closed session reply rejects even with stale metadata attached");
  bad = current;
  bad.commit = L"你好";
  rejected_response(bad, "state query must not carry a commit");
  bad = current;
  bad.snapshot = candidates.snapshot;
  rejected_response(bad, "candidate response cannot masquerade as a state-only reply");

  auto wrong_query = query;
  wrong_query.command = Command::kPing;
  auto ticket = begin(candidates);
  Expect(ticket && !accept(*ticket, wrong_query, current), "ping cannot certify engine freshness");
  wrong_query = query;
  wrong_query.request_id = input.request_id;
  bad = current;
  bad.request_id = wrong_query.request_id;
  ticket = begin(candidates);
  Expect(ticket && !accept(*ticket, wrong_query, bad), "old query id cannot certify a new request");
  wrong_query = query;
  wrong_query.value = 1;
  ticket = begin(candidates);
  Expect(ticket && !accept(*ticket, wrong_query, current), "invalid query arguments reject");

  bad = candidates;
  bad.dictionary_epoch = 0;
  Expect(!begin(bad), "production-unavailable epoch cannot start a binding");
  bad = candidates;
  bad.request_id = 9;
  Expect(!begin(bad), "mismatched candidate response cannot start a binding");
  bad = candidates;
  bad.commit = L"你好";
  Expect(!begin(bad), "response with committed text cannot start ranking");

  const auto stale = begin(candidates);
  const auto newer = begin(candidates);
  bad = current;
  bad.request_id = 999;
  Expect(stale && newer && !accept(*stale, query, bad) && accept(*newer, query, current),
         "malformed response for an old ticket does not erase a newer binding");
  ticket = begin(candidates);
  auto changed_field = field;
  changed_field.field_token = {900, 901};
  Expect(ticket && !binding.TryAccept(*ticket, changed_field, u"prefix", query, current,
                                      At(std::chrono::seconds(2))),
         "matching engine metadata cannot bridge a field switch");
  ticket = begin(candidates);
  Expect(ticket && !binding.TryAccept(*ticket, field, u"edited", query, current,
                                      At(std::chrono::seconds(2))),
         "matching engine metadata cannot override a changed prefix");
  for (const auto privacy : {ContextPrivacy::restricted, ContextPrivacy::unknown,
                            ContextPrivacy::blocked}) {
    ticket = begin(candidates);
    changed_field = field;
    changed_field.privacy = privacy;
    Expect(ticket && !binding.TryAccept(*ticket, changed_field, u"prefix", query, current,
                                        At(std::chrono::seconds(2))),
           "valid engine state cannot override fresh privacy rejection");
  }
  ticket = begin(candidates);
  changed_field = field;
  changed_field.identity_verified = false;
  Expect(ticket && !binding.TryAccept(*ticket, changed_field, u"prefix", query, current,
                                      At(std::chrono::seconds(2))),
         "engine session identity is not proof of field identity");
  ticket = begin(candidates);
  changed_field = field;
  changed_field.fresh_read_ok = false;
  Expect(ticket && !binding.TryAccept(*ticket, changed_field, u"prefix", query, current,
                                      At(std::chrono::seconds(2))),
         "successful state query cannot substitute for a fresh field read");
  ticket = begin(candidates);
  Expect(ticket && binding.Expire(At(std::chrono::seconds(5))) &&
             !accept(*ticket, query, current), "expired binding cannot accept a wire response");
}

}  // namespace

int main() {
  TestEngineResponseCorrelation();
  TestCopyAndValidUtf16Acceptance();
  TestExactLengthAndContentMatching();
  TestBoundsAndInvalidUtf16Reject();
  TestSnapshotPrivacyIdentityAndCaretContract();
  TestFreshPrivacyAndPrefixFailuresConsumeCurrentTicket();
  TestExpiryRollbackAndNoPendingMaintenance();
  TestDeniedReplacementInvalidatesPreviousBinding();
  TestStaleForeignAndAbaResponsesPreserveCurrentBinding();
  TestInvalidatePreventsTicketRevival();
  std::cout << "context prefix binding assertions: " << expectation_count << '\n';
  return EXIT_SUCCESS;
}
