#pragma once

// Preparatory, test-only reader. Not connected to TIP key handling or ranking.
// A successful read is NOT evidence of field identity or an absolute caret offset.
#include "context_metadata_probe.h"
#include "../include/ziliu/tsf/module_state.h"

#include <array>
#include <atomic>
#include <new>
#include <string_view>

namespace ziliu::tsf::detail {

class PrefixSnapshotReadSession;

// Owner-thread-only, short-lived suffix; never persist or log this buffer.
// A view is invalidated by Clear(), the next read, or destruction.
class ContextPrefixSnapshot final {
 public:
  static constexpr ULONG kCapacity = 128;
  ContextPrefixSnapshot() noexcept = default;
  ~ContextPrefixSnapshot() { Clear(); }
  ContextPrefixSnapshot(const ContextPrefixSnapshot&) = delete;
  ContextPrefixSnapshot& operator=(const ContextPrefixSnapshot&) = delete;
  ContextPrefixSnapshot(ContextPrefixSnapshot&&) = delete;
  ContextPrefixSnapshot& operator=(ContextPrefixSnapshot&&) = delete;

  void Clear() noexcept {
    SecureZeroMemory(buffer_.data(), sizeof(buffer_));
    count_ = 0;
    caret_.Reset();
    context_identity_.Reset();
  }
  [[nodiscard]] std::wstring_view text() const noexcept { return {buffer_.data(), count_}; }

 private:
  friend class PrefixSnapshotReadSession;
  std::array<WCHAR, kCapacity> buffer_{};
  ULONG count_ = 0;
  // Dynamic anchors, not an immutable offset or field identifier. Editing must
  // invalidate the request independently even if these anchors move together.
  Microsoft::WRL::ComPtr<ITfRange> caret_;
  Microsoft::WRL::ComPtr<IUnknown> context_identity_;
};

struct ContextPrefixReadResult {
  HRESULT request_hr = E_PENDING;
  HRESULT session_hr = E_PENDING;
  HRESULT read_hr = E_PENDING;
  LONG shifted = 0;  // Range-relative only; never an absolute caret position.
  ULONG count = 0;
  bool body_read = false;
  bool prefix_read_ok = false;
  bool caret_compared = false;
  bool caret_matches = false;
};

using PrefixSnapshotAllowed = bool (*)(void*);

class PrefixSnapshotReadSession final : public ITfEditSession {
 public:
  PrefixSnapshotReadSession(ITfContext* context, DWORD flags, PrefixSnapshotAllowed allowed,
                            void* gate, ContextPrefixSnapshot& output,
                            ContextPrefixReadResult& result,
                            const ContextPrefixSnapshot* previous)
      : context_(context), flags_(flags), allowed_(allowed), gate_(gate), output_(&output),
        result_(&result), thread_(GetCurrentThreadId()), compare_requested_(previous != nullptr) {
    if (previous && previous->count_ != 0) {
      previous_caret_ = previous->caret_;
      previous_context_ = previous->context_identity_;
    }
    AddModuleReference();
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfEditSession) return E_NOINTERFACE;
    *out = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --refs_;
    if (!remaining) delete this;
    return remaining;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    // Never touch caller-owned pointers in late/repeated/foreign callbacks.
    if (GetCurrentThreadId() != thread_) return E_ACCESSDENIED;
    if (!active_.load() || called_.exchange(true)) return E_ABORT;
    if (!Allowed() || !Ordinary(cookie)) return E_ACCESSDENIED;

    TF_SELECTION selection{};
    ULONG fetched = 0;
    const HRESULT hr = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1,
                                               &selection, &fetched);
    Microsoft::WRL::ComPtr<ITfRange> range;
    range.Attach(selection.range);
    if (hr != S_OK || fetched != 1 || !range || selection.style.fInterimChar != FALSE ||
        (selection.style.ase != TF_AE_NONE && selection.style.ase != TF_AE_START &&
         selection.style.ase != TF_AE_END)) return E_FAIL;
    BOOL empty = FALSE;
    if (range->IsEmpty(cookie, &empty) != S_OK || !empty ||
        range->Collapse(cookie, TF_ANCHOR_START) != S_OK) return E_FAIL;
    Microsoft::WRL::ComPtr<IUnknown> context_identity;
    if (context_->QueryInterface(IID_PPV_ARGS(&context_identity)) != S_OK ||
        !context_identity) return E_FAIL;
    if (compare_requested_) {
      if (!previous_caret_ || !previous_context_ ||
          previous_context_.Get() != context_identity.Get()) return E_ABORT;
      BOOL same_start = FALSE, same_end = FALSE;
      result_->caret_compared = true;
      if (previous_caret_->IsEqualStart(cookie, range.Get(), TF_ANCHOR_START, &same_start) != S_OK ||
          previous_caret_->IsEqualEnd(cookie, range.Get(), TF_ANCHOR_END, &same_end) != S_OK ||
          same_start != TRUE || same_end != TRUE) return E_ABORT;
      result_->caret_matches = true;
    }
    Microsoft::WRL::ComPtr<ITfRange> caret;
    if (range->Clone(&caret) != S_OK || !caret) return E_FAIL;
    if (range->ShiftStart(cookie, -static_cast<LONG>(ContextPrefixSnapshot::kCapacity),
                          &result_->shifted, nullptr) != S_OK ||
        result_->shifted >= 0 ||
        result_->shifted < -static_cast<LONG>(ContextPrefixSnapshot::kCapacity)) return E_FAIL;

    // A shorter shift may mean a region boundary, not the start of the field.
    // No full-field length or absolute selection offset is manufactured here.
    if (!Ordinary(cookie) || !Allowed()) return E_ABORT;
    ContextPrefixSnapshot temporary;
    result_->body_read = true;
    result_->read_hr = range->GetText(cookie, 0, temporary.buffer_.data(),
                                     ContextPrefixSnapshot::kCapacity, &result_->count);
    if (result_->read_hr != S_OK || result_->count != static_cast<ULONG>(-result_->shifted) ||
        result_->count > ContextPrefixSnapshot::kCapacity) return E_FAIL;
    temporary.count_ = result_->count;
    if (!ValidUtf16(temporary.text()) || !Ordinary(cookie) || !Allowed()) return E_ABORT;
    for (ULONG i = 0; i < temporary.count_; ++i) output_->buffer_[i] = temporary.buffer_[i];
    output_->count_ = temporary.count_;
    output_->caret_ = caret;
    output_->context_identity_ = context_identity;
    result_->prefix_read_ok = true;
    return S_OK;
  }
  void Deactivate() noexcept {
    active_.store(false);
    allowed_ = nullptr; gate_ = nullptr; output_ = nullptr; result_ = nullptr;
  }

 private:
  bool Allowed() const { return active_.load() && allowed_ && allowed_(gate_); }
  bool Ordinary(TfEditCookie cookie) const {
    return !ContextBlocksInput(context_.Get(), flags_) &&
        ClassifyScope(context_.Get(), cookie) == InputPrivacy::kOrdinary &&
        HasStrictOrdinaryProbeScope(context_.Get(), cookie);
  }
  static bool ValidUtf16(std::wstring_view text) noexcept {
    if (text.empty()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const auto c = static_cast<unsigned>(text[i]);
      if (!c) return false;
      if (c >= 0xD800 && c <= 0xDBFF) {
        if (++i == text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF) return false;
      } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
    }
    return true;
  }
  ~PrefixSnapshotReadSession() { ReleaseModuleReference(); }
  std::atomic<ULONG> refs_{1};
  std::atomic<bool> active_{true}, called_{false};
  Microsoft::WRL::ComPtr<ITfContext> context_;
  DWORD flags_;
  PrefixSnapshotAllowed allowed_;
  void* gate_;
  ContextPrefixSnapshot* output_;
  ContextPrefixReadResult* result_;
  DWORD thread_;
  bool compare_requested_;
  Microsoft::WRL::ComPtr<ITfRange> previous_caret_;
  Microsoft::WRL::ComPtr<IUnknown> previous_context_;
};

// Caller must opt in and check current focus, lifecycle/revision, and consent in
// allowed BEFORE requesting and throughout reading. No callback means no read.
// With previous, compare both caret anchors in this read lock, before GetText.
// Matching anchors are an extra veto only: clones track edits, and context COM
// equality does NOT distinguish fields sharing a text store. Never skip the
// caller's focus/edit revision gate. Old and new output must be distinct objects.
// Does not use titles, fixed expected text, ACP offsets, or fabricate identities.
// Caller clears the output immediately after use; it has no automatic expiry.
inline ContextPrefixReadResult ReadContextPrefixSnapshot(
    ITfContext* context, TfClientId client, DWORD flags, PrefixSnapshotAllowed allowed,
    void* gate, ContextPrefixSnapshot& output, const ContextPrefixSnapshot* previous = nullptr) {
  output.Clear();
  ContextPrefixReadResult result;
  if (&output == previous) return result;
  if (!context || !allowed || !allowed(gate) || ContextBlocksInput(context, flags)) return result;
  auto* session = new (std::nothrow) PrefixSnapshotReadSession(
      context, flags, allowed, gate, output, result, previous);
  if (!session) return result;
  result.request_hr = context->RequestEditSession(client, session, TF_ES_SYNC | TF_ES_READ,
                                                  &result.session_hr);
  session->Deactivate();
  if (result.request_hr != S_OK || result.session_hr != S_OK || !allowed(gate)) {
    result.prefix_read_ok = false;
  }
  if (!result.prefix_read_ok) {
    result.caret_matches = false;
    output.Clear();
  }
  session->Release();
  return result;
}

}  // namespace ziliu::tsf::detail
