#pragma once

#include "ziliu/tsf/module_state.h"
#include "ziliu/core/context_request_guard.h"

#include <msctf.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <new>

namespace ziliu::tsf::detail {

// Numeric, process-local diagnostics only. IDs never expose COM addresses.
enum class ContextEventKind : int {
  kStart, kAttach, kFocus, kPush, kPop, kUninit, kEndEdit, kStop, kBindFailure,
  kCheckpoint
};

struct ContextEvent {
  ContextEventKind kind = ContextEventKind::kStart;
  unsigned identity_epoch = 0;
  unsigned context_id = 0;
  unsigned previous_context_id = 0;
  unsigned document_id = 0;
  unsigned previous_document_id = 0;
  HRESULT hr = E_PENDING;
  HRESULT selection_hr = E_PENDING;
  bool selection_changed = false;
  HRESULT updates_hr = E_PENDING;
  HRESULT next_hr = E_PENDING;
  bool changed_range_present = false;
};

// Only compiled into the explicit test probe build. TSF callbacks and Start/Stop
// are expected on the same TSF apartment; the atomic count protects COM lifetime.
class ContextEventProbe final : public ITfThreadMgrEventSink, public ITfTextEditSink {
 public:
  using Allowed = bool (*)();
  using Emit = void (*)(const ContextEvent&);

  ContextEventProbe(Allowed allowed, Emit emit,
                    core::ContextLifetimeToken request_owner = {}) noexcept
      : allowed_(allowed), emit_(emit), identity_epoch_(++next_epoch_),
        request_guard_(request_owner) {
    AddModuleReference();
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfThreadMgrEventSink)) {
      *result = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(iid, IID_ITfTextEditSink)) {
      *result = static_cast<ITfTextEditSink*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --refs_;
    if (!count) delete this;
    return count;
  }

  HRESULT Start(ITfThreadMgr* manager) {
    RequestBlock block(request_blocked_);
    request_guard_.Invalidate();
    if (!manager) return E_INVALIDARG;
    if (active_ || manager_) return E_UNEXPECTED;
    Microsoft::WRL::ComPtr<ITfSource> source;
    HRESULT hr = manager->QueryInterface(IID_PPV_ARGS(&source));
    if (FAILED(hr)) return hr;
    manager_ = manager;
    thread_source_ = source;
    active_ = true;
    hr = source->AdviseSink(IID_ITfThreadMgrEventSink,
                            static_cast<ITfThreadMgrEventSink*>(this), &thread_cookie_);
    if (FAILED(hr)) {
      active_ = false;
      thread_cookie_ = TF_INVALID_COOKIE;
      thread_source_.Reset();
      manager_.Reset();
      return hr;
    }
    EmitEvent(ContextEvent{.kind = ContextEventKind::kStart, .hr = hr});
    if (!active_) return S_OK;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> document;
    hr = manager->GetFocus(&document);
    if (SUCCEEDED(hr)) Rebind(document.Get());
    else EmitEvent(ContextEvent{.kind = ContextEventKind::kBindFailure, .hr = hr});
    return S_OK;
  }

  HRESULT Stop() {
    RequestBlock block(request_blocked_);
    request_guard_.Invalidate();
    if (stopping_) return S_OK;
    if (!active_ && !manager_ && !thread_source_ && !text_source_) return S_OK;
    stopping_ = true;
    if (active_) EmitEvent(ContextEvent{.kind = ContextEventKind::kStop, .hr = S_OK});
    active_ = false;
    // Disable all owner callbacks before invoking either source: UnadviseSink may reenter.
    allowed_ = nullptr;
    emit_ = nullptr;
    HRESULT result = S_OK;
    if (text_source_ && text_cookie_ != TF_INVALID_COOKIE) {
      const HRESULT hr = text_source_->UnadviseSink(text_cookie_);
      if (SUCCEEDED(hr)) {
        text_cookie_ = TF_INVALID_COOKIE;
        text_source_.Reset();
        context_.Reset();
      } else {
        result = hr;
      }
    }
    if (thread_source_ && thread_cookie_ != TF_INVALID_COOKIE) {
      const HRESULT hr = thread_source_->UnadviseSink(thread_cookie_);
      if (SUCCEEDED(hr)) {
        thread_cookie_ = TF_INVALID_COOKIE;
        thread_source_.Reset();
      } else if (SUCCEEDED(result)) {
        result = hr;
      }
    }
    // On failure retain the source for a later Stop retry. TSF's registration
    // reference keeps this sink and its DLL alive; freeing it would be unsafe.
    if (SUCCEEDED(result)) {
      binding_valid_ = false;
      manager_.Reset();
      document_.Reset();
      context_.Reset();
      identities_ = {};
      identity_count_ = 0;
      next_identity_id_ = 1;
    }
    stopping_ = false;
    return result;
  }

  // Synthetic diagnostic requests only. No host field identity is inferred here:
  // the caller must supply verified, fresh evidence, including at acceptance.
  // A default owner permanently disables requests; production has no ranking path.
  [[nodiscard]] bool ObserveSyntheticRequest(const core::ContextRequestSnapshot& snapshot) {
    if (!RequestReady()) return false;
    return request_guard_.Observe(snapshot);
  }
  void CancelSyntheticRequests() noexcept { request_guard_.Invalidate(); }
  [[nodiscard]] std::optional<std::uint64_t> CaptureReadRevision() {
    if (!RequestReady() || request_guard_.closed()) return {};
    return request_guard_.revision();
  }
  [[nodiscard]] bool IsReadRevisionCurrent(std::uint64_t revision) {
    return RequestReady() && !request_guard_.closed() && request_guard_.revision() == revision;
  }
  [[nodiscard]] std::optional<core::ContextRequestTicket> BeginSyntheticRequest(
      core::ContextRequestGuard::TimePoint now, core::ContextRequestGuard::TimePoint deadline) {
    if (!RequestReady()) return {};
    return request_guard_.BeginRequest(now, deadline);
  }
  [[nodiscard]] bool TryAcceptSyntheticResult(
      const core::ContextRequestTicket& ticket, const core::ContextRequestSnapshot& snapshot,
      core::ContextRequestGuard::TimePoint now) {
    if (!RequestReady()) return false;
    return request_guard_.TryAccept(ticket, snapshot, now);
  }

  void Checkpoint() {
    if (!active_) return;
    SelfHold hold(this);
    RequestBlock block(request_blocked_);
    RebindFocus();
    if (!active_) return;
    EmitEvent(ContextEvent{.kind = ContextEventKind::kCheckpoint,
                           .context_id = Identity(context_.Get()),
                           .document_id = Identity(document_.Get()),
                           .hr = thread_cookie_ != TF_INVALID_COOKIE && binding_valid_ &&
                                     context_ && text_source_ && text_cookie_ != TF_INVALID_COOKIE
                                     ? S_OK : E_UNEXPECTED});
  }

  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* document) override {
    if (!active_) return S_OK;
    SelfHold hold(this);
    RequestBlock block(request_blocked_);
    request_guard_.Invalidate();
    EmitEvent(ContextEvent{.kind = ContextEventKind::kUninit,
                           .document_id = Identity(document), .hr = S_OK});
    if (!active_) return S_OK;
    if (Same(document_.Get(), document)) Rebind(nullptr);
    return S_OK;
  }
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* focused, ITfDocumentMgr* previous) override {
    if (!active_) return S_OK;
    SelfHold hold(this);
    RequestBlock block(request_blocked_);
    request_guard_.Invalidate();
    EmitEvent(ContextEvent{.kind = ContextEventKind::kFocus,
                           .document_id = Identity(focused),
                           .previous_document_id = Identity(previous), .hr = S_OK});
    if (!active_) return S_OK;
    Rebind(focused);
    return S_OK;
  }
  STDMETHODIMP OnPushContext(ITfContext* context) override {
    if (!active_) return S_OK;
    SelfHold hold(this);
    RequestBlock block(request_blocked_);
    request_guard_.Invalidate();
    EmitEvent(ContextEvent{.kind = ContextEventKind::kPush,
                           .context_id = Identity(context), .hr = S_OK});
    if (!active_) return S_OK;
    RebindFocus();
    return S_OK;
  }
  STDMETHODIMP OnPopContext(ITfContext* context) override {
    if (!active_) return S_OK;
    SelfHold hold(this);
    RequestBlock block(request_blocked_);
    request_guard_.Invalidate();
    EmitEvent(ContextEvent{.kind = ContextEventKind::kPop,
                           .context_id = Identity(context), .hr = S_OK});
    if (!active_) return S_OK;
    RebindFocus();
    return S_OK;
  }
  STDMETHODIMP OnEndEdit(ITfContext* context, TfEditCookie cookie,
                        ITfEditRecord* record) override {
    if (!active_) return S_OK;
    SelfHold hold(this);
    RequestBlock block(request_blocked_);
    // Fail closed before any diagnostic gate or COM query. Even an unclassified
    // or late edit may invalidate a pending result. Never read outside the gate.
    request_guard_.Invalidate();
    if (!AllowedNow() || !binding_valid_ || !context ||
        !Same(context_.Get(), context)) return S_OK;
    ContextEvent event{.kind = ContextEventKind::kEndEdit,
                       .context_id = Identity(context),
                       .document_id = Identity(document_.Get()), .hr = S_OK};
    if (record) {
      BOOL changed = FALSE;
      event.selection_hr = record->GetSelectionStatus(&changed);
      event.selection_changed = SUCCEEDED(event.selection_hr) && changed != FALSE;
      Microsoft::WRL::ComPtr<IEnumTfRanges> ranges;
      event.updates_hr = record->GetTextAndPropertyUpdates(
          TF_GTP_INCL_TEXT, nullptr, 0, &ranges);
      if (SUCCEEDED(event.updates_hr) && ranges) {
        Microsoft::WRL::ComPtr<ITfRange> range;
        ULONG fetched = 0;
        event.next_hr = ranges->Next(1, range.GetAddressOf(), &fetched);
        event.changed_range_present = event.next_hr == S_OK && fetched == 1 && range;
      }
    } else {
      event.hr = E_POINTER;
    }
    // The read-only cookie is deliberately unused; no text or offsets are read.
    static_cast<void>(cookie);
    EmitEvent(event);
    return S_OK;
  }

 private:
  ~ContextEventProbe() { ReleaseModuleReference(); }
  struct SelfHold {
    explicit SelfHold(ContextEventProbe* value) : value(value) { value->AddRef(); }
    ~SelfHold() { value->Release(); }
    ContextEventProbe* value;
  };
  struct IdentityEntry { Microsoft::WRL::ComPtr<IUnknown> identity; unsigned id = 0; };
  // COM and diagnostic callbacks may reenter on the same apartment. Do not let
  // them arm/accept a request partway through a focus/edit/rebinding transition.
  struct RequestBlock {
    explicit RequestBlock(bool& flag) : flag(flag), previous(flag) { flag = true; }
    ~RequestBlock() { flag = previous; }
    bool& flag;
    bool previous;
  };
  bool AllowedNow() const { return allowed_ && allowed_(); }
  bool RequestReady() {
    if (active_ && !stopping_ && !request_blocked_ && binding_valid_ && manager_ && context_ && document_ &&
        thread_source_ && thread_cookie_ != TF_INVALID_COOKIE && text_source_ &&
        text_cookie_ != TF_INVALID_COOKIE && AllowedNow()) return true;
    request_guard_.Invalidate();
    return false;
  }
  void EmitEvent(ContextEvent event) {
    // Logging eligibility (title/foreground/record cap) must never suppress
    // cancellation. Successful checkpoints observe enrollment, not an edit.
    if (event.kind != ContextEventKind::kCheckpoint || event.hr != S_OK)
      request_guard_.Invalidate();
    if (active_ && AllowedNow() && emit_) {
      event.identity_epoch = identity_epoch_;
      emit_(event);
    }
  }
  static bool Same(IUnknown* a, IUnknown* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    Microsoft::WRL::ComPtr<IUnknown> left, right;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&left))) &&
           SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&right))) && left.Get() == right.Get();
  }
  unsigned Identity(IUnknown* object) {
    if (!object) return 0;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
    for (unsigned i = 0; i < identity_count_; ++i)
      if (identities_[i].identity.Get() == identity.Get()) return identities_[i].id;
    if (identity_count_ == identities_.size()) return 0;
    const unsigned id = next_identity_id_++;
    identities_[identity_count_++] = {identity, id};
    return id;
  }
  void RebindFocus() {
    if (!manager_) return;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> focused;
    const HRESULT hr = manager_->GetFocus(&focused);
    if (FAILED(hr)) {
      binding_valid_ = false;
      EmitEvent(ContextEvent{.kind = ContextEventKind::kBindFailure, .hr = hr});
      return;
    }
    Rebind(focused.Get());
  }
  void Rebind(ITfDocumentMgr* document) {
    if (!active_) return;
    Microsoft::WRL::ComPtr<ITfContext> top;
    HRESULT hr = S_OK;
    if (document) hr = document->GetTop(&top);
    if (FAILED(hr)) {
      binding_valid_ = false;
      EmitEvent(ContextEvent{.kind = ContextEventKind::kBindFailure,
                             .document_id = Identity(document), .hr = hr});
      return;
    }
    if (binding_valid_ && Same(context_.Get(), top.Get()) &&
        (!top || (text_source_ && text_cookie_ != TF_INVALID_COOKIE))) {
      if (!Same(document_.Get(), document)) request_guard_.Invalidate();
      document_ = document;
      return;
    }
    request_guard_.Invalidate();
    binding_valid_ = false;
    const unsigned previous = Identity(context_.Get());
    if (text_source_ && text_cookie_ != TF_INVALID_COOKIE) {
      hr = text_source_->UnadviseSink(text_cookie_);
      if (FAILED(hr)) {
        binding_valid_ = false;
        EmitEvent(ContextEvent{.kind = ContextEventKind::kBindFailure,
                               .context_id = previous, .hr = hr});
        return;
      }
      if (!active_) return;
      text_cookie_ = TF_INVALID_COOKIE;
      text_source_.Reset();
    }
    context_ = top;
    document_ = document;
    if (top) {
      hr = top->QueryInterface(IID_PPV_ARGS(&text_source_));
      if (SUCCEEDED(hr)) {
        hr = text_source_->AdviseSink(IID_ITfTextEditSink,
                                     static_cast<ITfTextEditSink*>(this), &text_cookie_);
      }
      if (FAILED(hr)) {
        binding_valid_ = false;
        text_cookie_ = TF_INVALID_COOKIE;
        text_source_.Reset();
        EmitEvent(ContextEvent{.kind = ContextEventKind::kBindFailure,
                               .context_id = Identity(top.Get()), .hr = hr});
      }
    }
    binding_valid_ = SUCCEEDED(hr);
    EmitEvent(ContextEvent{.kind = ContextEventKind::kAttach,
                           .context_id = Identity(top.Get()),
                           .previous_context_id = previous,
                           .document_id = Identity(document), .hr = hr});
  }

  std::atomic<ULONG> refs_{1};
  inline static std::atomic<unsigned> next_epoch_{0};
  Allowed allowed_;
  Emit emit_;
  unsigned identity_epoch_;
  core::ContextRequestGuard request_guard_;
  bool request_blocked_ = false;
  bool active_ = false;
  bool stopping_ = false;
  bool binding_valid_ = false;
  Microsoft::WRL::ComPtr<ITfThreadMgr> manager_;
  Microsoft::WRL::ComPtr<ITfSource> thread_source_;
  Microsoft::WRL::ComPtr<ITfDocumentMgr> document_;
  Microsoft::WRL::ComPtr<ITfContext> context_;
  Microsoft::WRL::ComPtr<ITfSource> text_source_;
  DWORD thread_cookie_ = TF_INVALID_COOKIE;
  DWORD text_cookie_ = TF_INVALID_COOKIE;
  std::array<IdentityEntry, 32> identities_{};
  unsigned identity_count_ = 0;
  unsigned next_identity_id_ = 1;
};

}  // namespace ziliu::tsf::detail
