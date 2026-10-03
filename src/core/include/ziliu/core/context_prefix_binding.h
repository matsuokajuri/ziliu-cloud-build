#pragma once

#include "ziliu/core/context_request_guard.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace ziliu::core {

// Binds one caller-provided UTF-16 suffix of the pre-caret field text to one
// request ticket. It is caller-thread-only: there is no timer, callback, or
// atomicity with a host read/apply operation, and text never establishes identity.
// The caller must supply verified identity/privacy metadata, call Expire(now)
// during timeout maintenance, and call Invalidate() on focus/privacy/lifecycle
// changes. The 128-unit bound is storage-only, not a model-context commitment.
// Selection offsets here are absolute UTF-16 field offsets; an unknown offset
// must not be replaced by a fabricated caret position to satisfy this contract.
class ContextPrefixBinding final {
 public:
  using TimePoint = ContextRequestGuard::TimePoint;
  static constexpr std::size_t kMaxPrefixUtf16Units = 128;

  explicit ContextPrefixBinding(ContextLifetimeToken owner) noexcept : guard_(owner) {}
  ~ContextPrefixBinding() noexcept { WipePrefix(); }

  ContextPrefixBinding(const ContextPrefixBinding&) = delete;
  ContextPrefixBinding& operator=(const ContextPrefixBinding&) = delete;
  ContextPrefixBinding(ContextPrefixBinding&&) = delete;
  ContextPrefixBinding& operator=(ContextPrefixBinding&&) = delete;

  [[nodiscard]] std::optional<ContextRequestTicket> Begin(
      const ContextRequestSnapshot& snapshot, std::u16string_view prefix,
      TimePoint now, TimePoint deadline) noexcept {
    // Every attempt replaces the previous binding, including denied attempts.
    ClearBinding();
    if (!guard_.Observe(snapshot)) return std::nullopt;

    if (snapshot.selection_start_utf16 != snapshot.selection_end_utf16 ||
        prefix.size() > snapshot.selection_start_utf16 ||
        !IsValidPrefix(prefix)) {
      guard_.Invalidate();
      return std::nullopt;
    }

    const auto ticket = guard_.BeginRequest(now, deadline);
    if (!ticket) {
      guard_.Invalidate();
      return std::nullopt;
    }

    for (std::size_t i = 0; i < prefix.size(); ++i) prefix_[i] = prefix[i];
    prefix_size_ = prefix.size();
    pending_ticket_ = ticket;
    return ticket;
  }

  [[nodiscard]] bool TryAccept(const ContextRequestTicket& ticket,
                               const ContextRequestSnapshot& fresh_snapshot,
                               std::u16string_view fresh_prefix,
                               TimePoint now) noexcept {
    // A stale/foreign ticket must not erase a newer request's prefix.
    if (!pending_ticket_ || ticket != *pending_ticket_) return false;

    if (fresh_snapshot.selection_start_utf16 != fresh_snapshot.selection_end_utf16 ||
        fresh_prefix.size() > fresh_snapshot.selection_start_utf16 ||
        !IsValidPrefix(fresh_prefix) || !PrefixMatches(fresh_prefix) ||
        !guard_.TryAccept(ticket, fresh_snapshot, now)) {
      FailCurrentBinding();
      return false;
    }

    ClearBinding();
    return true;
  }

  // Explicit caller maintenance: expiry and clock rollback fail closed and
  // invalidate the guard. No automatic erasure occurs at the deadline.
  [[nodiscard]] bool Expire(TimePoint now) noexcept {
    if (!pending_ticket_) return false;
    if (now >= pending_ticket_->deadline || now < pending_ticket_->issued_at) {
      FailCurrentBinding();
      return true;
    }
    return false;
  }

  void Invalidate() noexcept { FailCurrentBinding(); }

 private:
  [[nodiscard]] static bool IsValidPrefix(std::u16string_view prefix) noexcept {
    if (prefix.empty() || prefix.size() > kMaxPrefixUtf16Units) return false;

    for (std::size_t i = 0; i < prefix.size(); ++i) {
      const auto unit = prefix[i];
      if (unit == u'\0') return false;
      if (IsHighSurrogate(unit)) {
        if (i + 1 >= prefix.size() || !IsLowSurrogate(prefix[i + 1])) return false;
        ++i;
      } else if (IsLowSurrogate(unit)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static constexpr bool IsHighSurrogate(char16_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDBFF;
  }

  [[nodiscard]] static constexpr bool IsLowSurrogate(char16_t unit) noexcept {
    return unit >= 0xDC00 && unit <= 0xDFFF;
  }

  [[nodiscard]] bool PrefixMatches(std::u16string_view fresh_prefix) const noexcept {
    if (fresh_prefix.size() != prefix_size_) return false;
    for (std::size_t i = 0; i < prefix_size_; ++i) {
      if (fresh_prefix[i] != prefix_[i]) return false;
    }
    return true;
  }

  void WipePrefix() noexcept {
    volatile char16_t* const buffer = prefix_.data();
    for (std::size_t i = 0; i < prefix_.size(); ++i) buffer[i] = u'\0';
    prefix_size_ = 0;
  }

  void ClearBinding() noexcept {
    WipePrefix();
    pending_ticket_.reset();
  }

  void FailCurrentBinding() noexcept {
    ClearBinding();
    guard_.Invalidate();
  }

  ContextRequestGuard guard_;
  std::array<char16_t, kMaxPrefixUtf16Units> prefix_{};
  std::size_t prefix_size_{};
  std::optional<ContextRequestTicket> pending_ticket_;
};

}  // namespace ziliu::core
