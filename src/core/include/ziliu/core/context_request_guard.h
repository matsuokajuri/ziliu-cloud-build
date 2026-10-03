#pragma once

#include "ziliu/core/broker_instance.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace ziliu::core {

// Opaque identity supplied by the owner of a gate. Callers must create a fresh,
// nonzero value for each gate lifetime/recreation, including process restarts;
// do not derive it from COM objects, HWNDs, hashes, or titles. This type is not
// a cryptographic or unforgeable security capability.
struct ContextLifetimeToken {
  std::uint64_t high{};
  std::uint64_t low{};

  [[nodiscard]] constexpr bool valid() const noexcept { return high != 0 || low != 0; }
  friend constexpr bool operator==(const ContextLifetimeToken&,
                                   const ContextLifetimeToken&) = default;
};

enum class ContextPrivacy : std::uint8_t {
  ordinary,
  restricted,
  blocked,
  unknown,
};

// The caller's authoritative, fresh evidence for one text-field state. No text,
// strings, or content hashes are retained here. Revisions/epochs must change
// whenever their represented state changes; identical text is not field identity.
struct ContextRequestSnapshot {
  ContextLifetimeToken field_token{};
  std::uint64_t field_epoch{};
  std::uint64_t edit_revision{};
  std::uint32_t selection_start_utf16{};
  std::uint32_t selection_end_utf16{};
  // Zero input/candidate revision means unavailable (legacy protocol or overflow).
  std::uint64_t input_revision{};
  std::uint64_t candidate_revision{};
  BrokerInstanceId broker_instance{};
  std::uint64_t engine_session_id{};  // Only unique within this broker instance.
  std::uint64_t dictionary_epoch{};
  ContextPrivacy privacy{ContextPrivacy::unknown};
  bool identity_verified{};
  bool fresh_read_ok{};

  friend constexpr bool operator==(const ContextRequestSnapshot&,
                                   const ContextRequestSnapshot&) = default;
};

struct ContextRequestTicket {
  ContextLifetimeToken owner{};
  std::uint64_t generation{};
  std::uint64_t request_id{};
  std::chrono::steady_clock::time_point issued_at{};
  std::chrono::steady_clock::time_point deadline{};

  friend constexpr bool operator==(const ContextRequestTicket&,
                                   const ContextRequestTicket&) = default;
};

// Single-owner-thread request gate for rejecting stale asynchronous ranking
// results. It neither reads UI state nor makes callbacks atomic. The caller must
// verify field identity/fresh reads and advance the relevant revisions/epochs,
// including when candidates are published/frozen/selected or a composition is
// cancelled. Same text is never evidence of the same field.
class ContextRequestGuard final {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit ContextRequestGuard(ContextLifetimeToken owner) noexcept : owner_(owner) {
    if (!owner_.valid()) closed_ = true;
  }

  ContextRequestGuard(const ContextRequestGuard&) = delete;
  ContextRequestGuard& operator=(const ContextRequestGuard&) = delete;
  ContextRequestGuard(ContextRequestGuard&&) = delete;
  ContextRequestGuard& operator=(ContextRequestGuard&&) = delete;

  [[nodiscard]] bool Observe(const ContextRequestSnapshot& snapshot) noexcept {
    if (!AdvanceGeneration()) return false;
    pending_.reset();
    current_.reset();
    if (!closed_ && IsValid(snapshot)) current_ = snapshot;
    return current_.has_value();
  }

  void Invalidate() noexcept {
    pending_.reset();
    current_.reset();
    static_cast<void>(AdvanceGeneration());
  }

  [[nodiscard]] std::optional<ContextRequestTicket> BeginRequest(
      TimePoint now, TimePoint deadline) noexcept {
    // Every begin attempt supersedes an earlier ticket, including denied begins.
    pending_.reset();
    if (closed_ || !current_ || deadline <= now || !CanAdvance(request_id_)) {
      if (!CanAdvance(request_id_)) ClosePermanently();
      return std::nullopt;
    }

    ++request_id_;
    pending_ = ContextRequestTicket{owner_, generation_, request_id_, now, deadline};
    return pending_;
  }

  [[nodiscard]] bool TryAccept(const ContextRequestTicket& ticket,
                               const ContextRequestSnapshot& fresh_snapshot,
                               TimePoint now) noexcept {
    // A stale or foreign ticket must not disturb a newer outstanding request.
    if (closed_ || !pending_ || ticket != *pending_) return false;

    if (now < ticket.issued_at || now >= ticket.deadline ||
        !IsValid(fresh_snapshot) || !current_ || fresh_snapshot != *current_) {
      Invalidate();
      return false;
    }

    pending_.reset();
    return true;
  }

  [[nodiscard]] bool closed() const noexcept { return closed_; }

  // Revision of this gate's observations/invalidation only, never field identity.
  // An owner can bracket a synchronous read and reject reentrant state changes.
  [[nodiscard]] std::uint64_t revision() const noexcept { return generation_; }

  // Shared checked increment rule, kept public so the terminal boundary can be
  // tested without test-only constructors or forcing private state corruption.
  [[nodiscard]] static constexpr bool CanAdvance(std::uint64_t value) noexcept {
    return value != std::numeric_limits<std::uint64_t>::max();
  }

 private:
  [[nodiscard]] static constexpr bool IsValid(
      const ContextRequestSnapshot& snapshot) noexcept {
    return snapshot.field_token.valid() && snapshot.field_epoch != 0 &&
           snapshot.selection_start_utf16 <= snapshot.selection_end_utf16 &&
           snapshot.input_revision != 0 && snapshot.candidate_revision != 0 &&
           snapshot.broker_instance.valid() && snapshot.engine_session_id != 0 &&
           snapshot.dictionary_epoch != 0 &&
           snapshot.privacy == ContextPrivacy::ordinary && snapshot.identity_verified &&
           snapshot.fresh_read_ok;
  }

  [[nodiscard]] bool AdvanceGeneration() noexcept {
    if (closed_ || !CanAdvance(generation_)) {
      ClosePermanently();
      return false;
    }
    ++generation_;
    return true;
  }

  void ClosePermanently() noexcept {
    closed_ = true;
    current_.reset();
    pending_.reset();
  }

  ContextLifetimeToken owner_{};
  std::optional<ContextRequestSnapshot> current_;
  std::optional<ContextRequestTicket> pending_;
  std::uint64_t generation_{};
  std::uint64_t request_id_{};
  bool closed_{};
};

}  // namespace ziliu::core
