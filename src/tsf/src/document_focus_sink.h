#pragma once

#include "ziliu/tsf/module_state.h"

#include <msctf.h>
#include <wrl/client.h>

#include <atomic>

namespace ziliu::tsf::detail {

// A separate sink keeps late callbacks harmless if TSF refuses UnadviseSink.
// Callbacks invalidate local state only; they never query fields or call IPC.
class DocumentFocusSink final : public ITfThreadMgrEventSink {
 public:
  using Invalidate = void (*)(void*);
  DocumentFocusSink() noexcept { AddModuleReference(); }
  STDMETHODIMP QueryInterface(REFIID iid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_ITfThreadMgrEventSink) return E_NOINTERFACE;
    *result = static_cast<ITfThreadMgrEventSink*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG count = --refs_;
    if (!count) delete this;
    return count;
  }
  HRESULT Start(ITfThreadMgr* manager, void* owner, Invalidate invalidate) {
    SelfHold hold(this);
    if (!manager || !owner || !invalidate) return E_INVALIDARG;
    if (source_) return E_UNEXPECTED;
    HRESULT result = manager->QueryInterface(IID_PPV_ARGS(&source_));
    if (FAILED(result)) return result;
    result = source_->AdviseSink(IID_ITfThreadMgrEventSink, this, &cookie_);
    if (FAILED(result)) {
      owner_ = nullptr;
      invalidate_ = nullptr;
      cookie_ = TF_INVALID_COOKIE;
      source_.Reset();
    } else {
      // Enrollment can synchronously notify focus before its cookie exists.
      // Activation has no prior input session; publish the owner only afterward.
      owner_ = owner;
      invalidate_ = invalidate;
    }
    return result;
  }
  HRESULT Stop() {
    SelfHold hold(this);
    // Detach before the potentially reentrant call, including the failure path.
    owner_ = nullptr;
    invalidate_ = nullptr;
    if (stopping_ || !source_) return S_OK;
    stopping_ = true;
    const HRESULT result = source_->UnadviseSink(cookie_);
    if (SUCCEEDED(result)) {
      cookie_ = TF_INVALID_COOKIE;
      source_.Reset();
    }
    // Keep the source/cookie on failure for a later Stop retry. The registration
    // reference keeps this sink and the DLL alive without retaining its owner.
    stopping_ = false;
    return result;
  }
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
  STDMETHODIMP OnPushContext(ITfContext*) override { return S_OK; }
  STDMETHODIMP OnPopContext(ITfContext*) override { return S_OK; }
  STDMETHODIMP OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) override {
    SelfHold hold(this);
    if (invalidate_) invalidate_(owner_);
    return S_OK;
  }

 private:
  class SelfHold final {
   public:
    explicit SelfHold(DocumentFocusSink* sink) : sink_(sink) { sink_->AddRef(); }
    ~SelfHold() { sink_->Release(); }
   private:
    DocumentFocusSink* sink_;
  };
  ~DocumentFocusSink() { ReleaseModuleReference(); }
  std::atomic<ULONG> refs_{1};
  Microsoft::WRL::ComPtr<ITfSource> source_;
  DWORD cookie_ = TF_INVALID_COOKIE;
  void* owner_ = nullptr;
  Invalidate invalidate_ = nullptr;
  bool stopping_ = false;
};

}  // namespace ziliu::tsf::detail
