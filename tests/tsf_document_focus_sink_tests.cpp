#include "../src/tsf/src/document_focus_sink.h"
#include "ziliu/core/input_focus_epoch.h"
#include "../src/tsf/src/startup_key_queue.h"
#include <cstdlib>

namespace ziliu::tsf {
static int module_refs = 0;
void AddModuleReference() noexcept { ++module_refs; }
void ReleaseModuleReference() noexcept { --module_refs; }
HINSTANCE ModuleInstance() noexcept { return nullptr; }
}
namespace {
using namespace ziliu::tsf::detail;
void Check(bool value) { if (!value) std::abort(); }
struct Registration {
  IUnknown* sink = nullptr;
  DWORD cookie = 0;
  unsigned advises = 0;
  unsigned unadvises = 0;
  HRESULT advise_hr = S_OK;
  HRESULT unadvise_hr = S_OK;
  void (*during_advise)(IUnknown*) = nullptr;
  void (*during_unadvise)(IUnknown*) = nullptr;
  HRESULT Advise(REFIID, IUnknown* value, DWORD* out) {
    ++advises;
    if (FAILED(advise_hr)) return advise_hr;
    Check(!sink);
    sink = value;
    sink->AddRef();
    *out = ++cookie;
    if (during_advise) during_advise(sink);
    return S_OK;
  }
  HRESULT Unadvise(DWORD value) {
    ++unadvises;
    Check(value == cookie && sink);
    if (during_unadvise) during_unadvise(sink);
    if (FAILED(unadvise_hr)) return unadvise_hr;
    sink->Release();
    sink = nullptr;
    return S_OK;
  }
  template <typename Interface>
  Interface* GetSink(REFIID iid) {
    if (!sink) return nullptr;
    Interface* result = nullptr;
    Check(SUCCEEDED(sink->QueryInterface(iid, reinterpret_cast<void**>(&result))));
    Check(result != nullptr);
    return result;
  }
};

struct Manager final : ITfThreadMgr, ITfSource {
  ULONG refs = 1;
  
  Registration registration;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_ITfThreadMgr) *out = static_cast<ITfThreadMgr*>(this);
    else if (id == IID_ITfSource) *out = static_cast<ITfSource*>(this);
    if (!*out) return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP AdviseSink(REFIID id, IUnknown* sink, DWORD* cookie) override {
    Check(id == IID_ITfThreadMgrEventSink); return registration.Advise(id, sink, cookie);
  }
  STDMETHODIMP UnadviseSink(DWORD cookie) override { return registration.Unadvise(cookie); }
  STDMETHODIMP Activate(TfClientId*) override { return E_NOTIMPL; }
  STDMETHODIMP Deactivate() override { return E_NOTIMPL; }
  STDMETHODIMP CreateDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumDocumentMgrs(IEnumTfDocumentMgrs**) override { return E_NOTIMPL; }
  STDMETHODIMP GetFocus(ITfDocumentMgr** out) override {
    *out = nullptr; return E_NOTIMPL;
  }
  STDMETHODIMP SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
  STDMETHODIMP AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP IsThreadFocus(BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetFunctionProvider(REFCLSID, ITfFunctionProvider**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumFunctionProviders(IEnumTfFunctionProviders**) override { return E_NOTIMPL; }
  STDMETHODIMP GetGlobalCompartment(ITfCompartmentMgr**) override { return E_NOTIMPL; }
};


struct Owner {
  ziliu::core::InputFocusEpoch epoch;
  StartupKeyQueue queue;
  unsigned calls = 0;
  bool switch_down = true;
};
void Invalidate(void* value) {
  auto& owner = *static_cast<Owner*>(value);
  owner.epoch.Invalidate(); owner.queue.Cancel(); owner.switch_down = false;
  ++owner.calls;
}
void LateFocus(IUnknown* sink) {
  ITfThreadMgrEventSink* event = nullptr;
  Check(SUCCEEDED(sink->QueryInterface(IID_PPV_ARGS(&event))));
  event->OnSetFocus(nullptr,nullptr); event->Release();
}
DocumentFocusSink* retiring = nullptr;
void RetireOwner(void*) {
  Check(retiring->Stop() == S_OK);
  retiring->Release(); retiring = nullptr;
  Check(ziliu::tsf::module_refs == 1); // Callback frame still owns the sink/DLL.
}
void ReenterStop(IUnknown*) { RetireOwner(nullptr); }
}
int main() {
  Owner owner;
  Manager manager;
  auto* sink = new DocumentFocusSink();
  Check(ziliu::tsf::module_refs == 1);
  Check(sink->Start(nullptr,&owner,Invalidate) == E_INVALIDARG);
  manager.registration.advise_hr = E_FAIL;
  Check(sink->Start(&manager,&owner,Invalidate) == E_FAIL);
  Check(manager.refs == 1 && !manager.registration.sink);
  manager.registration.advise_hr = S_OK;
  manager.registration.during_advise = LateFocus;
  Check(sink->Start(&manager,&owner,Invalidate) == S_OK);
  Check(owner.calls == 0); // Synchronous enrollment is not an old field transition.
  Check(sink->Start(&manager,&owner,Invalidate) == E_UNEXPECTED);
  const auto before = owner.epoch.Capture();
  const auto generation = owner.queue.generation();
  Check(owner.queue.Push('N',false,StartupKeyEffect::kCompose));
  auto* event = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  // Empty document and back: no field text, selection, HWND or pointer comparison.
  Check(event->OnSetFocus(nullptr,nullptr) == S_OK);
  Check(event->OnSetFocus(nullptr,nullptr) == S_OK);
  Check(owner.calls == 2 && !owner.epoch.IsCurrent(before));
  Check(owner.queue.empty() && owner.queue.generation() != generation && !owner.switch_down);
  Check(owner.queue.Take(generation).empty());
  const auto current = owner.epoch.Capture();
  Check(event->OnInitDocumentMgr(nullptr) == S_OK);
  Check(event->OnPushContext(nullptr) == S_OK);
  Check(event->OnPopContext(nullptr) == S_OK);
  Check(owner.epoch.IsCurrent(current));
  manager.registration.during_unadvise = LateFocus;
  manager.registration.unadvise_hr = E_FAIL;
  Check(sink->Stop() == E_FAIL);
  Check(owner.calls == 2 && manager.registration.sink && manager.refs == 2);
  event->OnSetFocus(nullptr,nullptr);
  Check(owner.calls == 2);
  manager.registration.unadvise_hr = S_OK;
  Check(sink->Stop() == S_OK);
  Check(manager.refs == 1 && !manager.registration.sink);
  event->OnSetFocus(nullptr,nullptr);
  Check(owner.calls == 2);
  event->Release();
  Check(sink->Stop() == S_OK);
  sink->Release();
  Check(ziliu::tsf::module_refs == 0);

  Manager callback_manager;
  retiring = new DocumentFocusSink();
  Check(retiring->Start(&callback_manager,&owner,RetireOwner) == S_OK);
  retiring->OnSetFocus(nullptr,nullptr); // Callback releases owner and registration.
  Check(!retiring && callback_manager.refs == 1 && ziliu::tsf::module_refs == 0);

  Manager reentry_manager;
  retiring = new DocumentFocusSink();
  Check(retiring->Start(&reentry_manager,&owner,Invalidate) == S_OK);
  reentry_manager.registration.during_unadvise = ReenterStop;
  Check(retiring->Stop() == S_OK); // Nested Stop plus owner release inside Unadvise.
  Check(!retiring && reentry_manager.refs == 1 && ziliu::tsf::module_refs == 0);
}
