#pragma once

// TEST ONLY. Reads one fixed Edge synthetic-fixture prefix; this is not field identity.
#include "context_metadata_probe.h"
#include "../include/ziliu/tsf/module_state.h"

#include <array>
#include <algorithm>

namespace ziliu::tsf::detail {

struct ContextFreshProbeResult {
  HRESULT request_hr = E_PENDING;
  HRESULT session_hr = E_PENDING;
  HRESULT selection_hr = E_PENDING;
  HRESULT read_hr = E_PENDING;
  ULONG count = 0;
  bool body_read = false;
  bool fresh_read_ok = false;
};

using FreshProbeAllowed = bool (*)(void*);

class FreshFixtureReadSession final : public ITfEditSession {
 public:
  FreshFixtureReadSession(ITfContext* context, DWORD activation_flags,
                          FreshProbeAllowed allowed, void* allowed_context,
                          ContextFreshProbeResult* result)
      : context_(context), activation_flags_(activation_flags), allowed_(allowed),
        allowed_context_(allowed_context), result_(result), owner_thread_(GetCurrentThreadId()) {
    ziliu::tsf::AddModuleReference();
  }

  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (out == nullptr) return E_INVALIDARG;
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfEditSession) return E_NOINTERFACE;
    *out = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    // The callback and result are caller-stack scoped; reject before touching either
    // from any apartment other than the requester's.
    if (GetCurrentThreadId() != owner_thread_) return E_ACCESSDENIED;
    if (!active_.load()) return E_ABORT;
    bool expected_call = false;
    if (!called_.compare_exchange_strong(expected_call, true)) return E_ABORT;
    auto& result = *result_;
    if (!allowed_ || !allowed_(allowed_context_)) return E_ABORT;
    if (ContextBlocksInput(context_.Get(), activation_flags_) ||
        ClassifyScope(context_.Get(), cookie) != InputPrivacy::kOrdinary ||
        !HasStrictOrdinaryProbeScope(context_.Get(), cookie)) return E_ACCESSDENIED;

    TF_SELECTION selection{};
    ULONG fetched = 0;
    result.selection_hr = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1,
                                                  &selection, &fetched);
    Microsoft::WRL::ComPtr<ITfRange> range;
    range.Attach(selection.range);
    if (result.selection_hr != S_OK || fetched != 1 || !range ||
        selection.style.fInterimChar != FALSE) return E_FAIL;
    if (selection.style.ase != TF_AE_NONE && selection.style.ase != TF_AE_START &&
        selection.style.ase != TF_AE_END) return E_FAIL;
    BOOL empty = FALSE;
    if (range->IsEmpty(cookie, &empty) != S_OK || empty == FALSE) return E_FAIL;
    if (range->Collapse(cookie, TF_ANCHOR_START) != S_OK) return E_FAIL;

    constexpr std::wstring_view expected =
        L"Ziliu synthetic context probe A. No personal information.";
    static_assert(expected.size() == 57);
    LONG shifted = 0;
    if (range->ShiftStart(cookie, -128, &shifted, nullptr) != S_OK ||
        shifted != -static_cast<LONG>(expected.size())) return E_FAIL;

    // The allowed predicate includes the fixture marker/title, focused context and
    // generation checks. Recheck at the final boundary before exposing text.
    if (!active_.load() || !allowed_ || !allowed_(allowed_context_)) return E_ABORT;
    std::array<WCHAR, 128> buffer{};
    ULONG count = 0;
    result.body_read = true;
    result.read_hr = range->GetText(cookie, 0, buffer.data(),
                                    static_cast<ULONG>(buffer.size()), &count);
    result.count = count;
    const bool complete = result.read_hr == S_OK && count == expected.size() &&
        std::equal(expected.begin(), expected.end(), buffer.begin());
    const bool still_allowed = active_.load() && allowed_ && allowed_(allowed_context_);
    result.fresh_read_ok = complete && still_allowed;
    SecureZeroMemory(buffer.data(), sizeof(buffer));
    return result.fresh_read_ok ? S_OK : E_ABORT;
  }

  void Deactivate() noexcept {
    active_.store(false);
    allowed_ = nullptr;
    allowed_context_ = nullptr;
    result_ = nullptr;
  }

 private:
  std::atomic<ULONG> references_{1};
  std::atomic<bool> active_{true};
  std::atomic<bool> called_{false};
  Microsoft::WRL::ComPtr<ITfContext> context_;
  DWORD activation_flags_;
  FreshProbeAllowed allowed_;
  void* allowed_context_;
  ContextFreshProbeResult* result_;
  DWORD owner_thread_;

  ~FreshFixtureReadSession() { ziliu::tsf::ReleaseModuleReference(); }
};

inline ContextFreshProbeResult ReadFreshFixturePrefix(
    ITfContext* context, TfClientId client_id, DWORD activation_flags,
    FreshProbeAllowed allowed, void* allowed_context) {
  ContextFreshProbeResult result;
  if (context == nullptr || allowed == nullptr || !allowed(allowed_context) ||
      ContextBlocksInput(context, activation_flags)) return result;
  auto* session = new (std::nothrow) FreshFixtureReadSession(
      context, activation_flags, allowed, allowed_context, &result);
  if (session == nullptr) return result;
  HRESULT session_hr = E_PENDING;
  result.request_hr = context->RequestEditSession(
      client_id, session, TF_ES_SYNC | TF_ES_READ, &session_hr);
  session->Deactivate();
  result.session_hr = session_hr;
  if (result.request_hr != S_OK || session_hr != S_OK) result.fresh_read_ok = false;
  if (!allowed(allowed_context)) result.fresh_read_ok = false;
  session->Release();
  return result;
}

}  // namespace ziliu::tsf::detail
