#include "../src/tsf/src/context_event_probe.h"
#include "../src/tsf/src/context_event_trace.h"
#include "../src/tsf/src/context_shadow_trial.h"

#include <cstdlib>
#include <chrono>
#include <vector>

namespace ziliu::tsf {
static int module_refs = 0;
void AddModuleReference() noexcept { ++module_refs; }
void ReleaseModuleReference() noexcept { --module_refs; }
HINSTANCE ModuleInstance() noexcept { return nullptr; }
}

namespace {
using namespace ziliu::tsf::detail;
void Check(bool condition) { if (!condition) std::abort(); }
bool allowed = true;
std::vector<ContextEvent> events;
bool Allowed() { return allowed; }
void Emit(const ContextEvent& event) { events.push_back(event); }

struct Registration {
  IUnknown* sink = nullptr;
  DWORD cookie = 0;
  unsigned advises = 0;
  unsigned unadvises = 0;
  HRESULT advise_hr = S_OK;
  HRESULT unadvise_hr = S_OK;
  HRESULT Advise(REFIID, IUnknown* value, DWORD* out) {
    ++advises;
    if (FAILED(advise_hr)) return advise_hr;
    Check(!sink);
    sink = value;
    sink->AddRef();
    *out = ++cookie;
    return S_OK;
  }
  HRESULT Unadvise(DWORD value) {
    ++unadvises;
    Check(value == cookie && sink);
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

struct Context final : ITfContext, ITfSource {
  ULONG refs = 1;
  Registration registration;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_ITfContext) *out = static_cast<ITfContext*>(this);
    else if (id == IID_ITfSource) *out = static_cast<ITfSource*>(this);
    if (!*out) return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP AdviseSink(REFIID id, IUnknown* sink, DWORD* cookie) override {
    Check(id == IID_ITfTextEditSink); return registration.Advise(id, sink, cookie);
  }
  STDMETHODIMP UnadviseSink(DWORD cookie) override { return registration.Unadvise(cookie); }
  STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession*, DWORD, HRESULT*) override { return E_NOTIMPL; }
  STDMETHODIMP InWriteSession(TfClientId, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION*, ULONG*) override { return E_NOTIMPL; }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return E_NOTIMPL; }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEnd(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetActiveView(ITfContextView**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
  STDMETHODIMP GetStatus(TF_STATUS*) override { return E_NOTIMPL; }
  STDMETHODIMP GetProperty(REFGUID, ITfProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                               ITfReadOnlyProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumProperties(IEnumTfProperties**) override { return E_NOTIMPL; }
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup**) override { return E_NOTIMPL; }
};

struct Document final : ITfDocumentMgr {
  ULONG refs = 1;
  Context* top = nullptr;
  HRESULT top_hr = S_OK;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    *out = id == IID_IUnknown || id == IID_ITfDocumentMgr ? static_cast<ITfDocumentMgr*>(this) : nullptr;
    if (!*out) return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP CreateContext(TfClientId, DWORD, IUnknown*, ITfContext**, TfEditCookie*) override { return E_NOTIMPL; }
  STDMETHODIMP Push(ITfContext*) override { return E_NOTIMPL; }
  STDMETHODIMP Pop(DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP GetTop(ITfContext** out) override {
    *out = nullptr;
    if (FAILED(top_hr)) return top_hr;
    if (top) { *out = top; top->AddRef(); }
    return S_OK;
  }
  STDMETHODIMP GetBase(ITfContext**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumContexts(IEnumTfContexts**) override { return E_NOTIMPL; }
};

struct Manager final : ITfThreadMgr, ITfSource {
  ULONG refs = 1;
  Document* focus = nullptr;
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
    *out = focus;
    if (focus) focus->AddRef();
    return S_OK;
  }
  STDMETHODIMP SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
  STDMETHODIMP AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP IsThreadFocus(BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetFunctionProvider(REFCLSID, ITfFunctionProvider**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumFunctionProviders(IEnumTfFunctionProviders**) override { return E_NOTIMPL; }
  STDMETHODIMP GetGlobalCompartment(ITfCompartmentMgr**) override { return E_NOTIMPL; }
};

struct NoTextRange final : ITfRange {
  ULONG refs = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = id == IID_IUnknown || id == IID_ITfRange ? static_cast<ITfRange*>(this) : nullptr;
    if (!*out) return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP GetText(TfEditCookie, DWORD, WCHAR*, ULONG, ULONG*) override { std::abort(); }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
  STDMETHODIMP Clone(ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor) override { return E_NOTIMPL; }
};

struct Ranges final : IEnumTfRanges {
  ULONG refs = 1;
  unsigned calls = 0;
  HRESULT next_hr = S_FALSE;
  NoTextRange range;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = id == IID_IUnknown || id == IID_IEnumTfRanges ? static_cast<IEnumTfRanges*>(this) : nullptr;
    if (!*out) return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP Clone(IEnumTfRanges**) override { return E_NOTIMPL; }
  STDMETHODIMP Next(ULONG count, ITfRange** out, ULONG* fetched) override {
    Check(count == 1); ++calls; *out = nullptr; *fetched = 0;
    if (next_hr == S_OK) { *out = &range; range.AddRef(); *fetched = 1; }
    return next_hr;
  }
  STDMETHODIMP Reset() override { return E_NOTIMPL; }
  STDMETHODIMP Skip(ULONG) override { return E_NOTIMPL; }
};

struct EditRecord final : ITfEditRecord {
  ULONG refs = 1;
  unsigned selection_calls = 0;
  unsigned update_calls = 0;
  BOOL changed = TRUE;
  HRESULT selection_hr = S_OK;
  HRESULT update_hr = S_OK;
  Ranges ranges;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = id == IID_IUnknown || id == IID_ITfEditRecord ? static_cast<ITfEditRecord*>(this) : nullptr;
    if (!*out) return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP GetSelectionStatus(BOOL* out) override {
    ++selection_calls; *out = changed; return selection_hr;
  }
  STDMETHODIMP GetTextAndPropertyUpdates(DWORD flags, const GUID** properties,
                                         ULONG count, IEnumTfRanges** out) override {
    ++update_calls;
    Check(flags == TF_GTP_INCL_TEXT && !properties && count == 0);
    *out = nullptr;
    if (SUCCEEDED(update_hr)) { *out = &ranges; ranges.AddRef(); }
    return update_hr;
  }
};

void TestLifecycle() {
  events.clear(); allowed = true;
  Context a, b;
  Document first, second;
  first.top = &a; second.top = &b;
  Manager manager; manager.focus = &first;
  auto* probe = new ContextEventProbe(Allowed, Emit);
  Check(ziliu::tsf::module_refs == 1);
  Check(probe->Start(&manager) == S_OK);
  Check(events.size() == 2 && events[0].kind == ContextEventKind::kStart &&
        events[1].kind == ContextEventKind::kAttach && events[1].context_id != 0);
  Check(a.registration.advises == 1 && manager.registration.advises == 1);
  manager.focus = &second;
  Check(probe->OnSetFocus(&second, &first) == S_OK);
  Check(events.back().kind == ContextEventKind::kAttach &&
        events.back().previous_context_id != events.back().context_id);
  Check(a.registration.unadvises == 1 && b.registration.advises == 1);
  const auto size = events.size();
  probe->OnSetFocus(&second, &first);
  Check(events.size() == size + 1 && events.back().kind == ContextEventKind::kFocus);
  Check(b.registration.advises == 1);
  EditRecord record;
  probe->OnEndEdit(&a, 1, &record);
  Check(record.selection_calls == 0 && record.update_calls == 0);
  probe->OnEndEdit(&b, 1, &record);
  Check(record.selection_calls == 1 && record.update_calls == 1 && record.ranges.calls == 1);
  Check(events.back().kind == ContextEventKind::kEndEdit && events.back().selection_changed &&
        events.back().next_hr == S_FALSE && !events.back().changed_range_present);
  record.ranges.next_hr = S_OK;
  probe->OnEndEdit(&b, 1, &record);
  Check(events.back().changed_range_present && events.back().next_hr == S_OK &&
        record.ranges.range.refs == 1);
  record.selection_hr = E_FAIL;
  record.update_hr = E_FAIL;
  probe->OnEndEdit(&b, 1, &record);
  Check(events.back().selection_hr == E_FAIL && !events.back().selection_changed &&
        events.back().updates_hr == E_FAIL && events.back().next_hr == E_PENDING &&
        !events.back().changed_range_present);
  record.selection_hr = S_OK;
  record.update_hr = S_OK;
  record.ranges.next_hr = E_FAIL;
  probe->OnEndEdit(&b, 1, &record);
  Check(events.back().next_hr == E_FAIL && !events.back().changed_range_present);
  allowed = false;
  probe->OnEndEdit(&b, 1, &record);
  Check(record.selection_calls == 4 && record.update_calls == 4);
  allowed = true;
  probe->OnPushContext(&b);
  probe->OnPopContext(&b);
  Check(events[events.size() - 2].kind == ContextEventKind::kPush &&
        events.back().kind == ContextEventKind::kPop);
  probe->OnUninitDocumentMgr(&second);
  Check(b.registration.unadvises == 1 && events.back().kind == ContextEventKind::kAttach);
  Check(b.refs > 1); // Identity map owns its canonical IUnknown until Stop.
  Check(probe->Stop() == S_OK && probe->Stop() == S_OK);
  Check(manager.registration.unadvises == 1);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0);
  Check(a.refs == 1 && b.refs == 1 && first.refs == 1 && second.refs == 1 && manager.refs == 1);
}

void TestFailures() {
  events.clear(); allowed = true;
  Manager manager;
  manager.registration.advise_hr = E_FAIL;
  auto* probe = new ContextEventProbe(Allowed, Emit);
  Check(probe->Start(&manager) == E_FAIL);
  Check(probe->Stop() == S_OK && manager.refs == 1);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0);
  manager.registration.advise_hr = S_OK;
  probe = new ContextEventProbe(Allowed, Emit);
  Check(probe->Start(&manager) == S_OK);
  probe->Checkpoint();
  Check(events.back().kind == ContextEventKind::kCheckpoint && events.back().hr == E_UNEXPECTED);
  manager.registration.unadvise_hr = E_FAIL;
  Check(probe->Stop() == E_FAIL);
  const auto count = events.size();
  probe->OnSetFocus(nullptr, nullptr);
  Check(events.size() == count);
  manager.registration.unadvise_hr = S_OK;
  Check(probe->Stop() == S_OK);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0 && manager.refs == 1);
}

void TestBindingRecovery() {
  events.clear(); allowed = false;
  Context a, b;
  Document first, second;
  first.top = &a; second.top = &b;
  Manager manager; manager.focus = &first;
  auto* probe = new ContextEventProbe(Allowed, Emit);
  Check(probe->Start(&manager) == S_OK && events.empty());
  Check(a.registration.advises == 1); // Binding proceeds despite log gate.
  manager.focus = &second;
  probe->OnSetFocus(&second, &first);
  Check(a.registration.unadvises == 1 && b.registration.advises == 1 && events.empty());
  allowed = true;
  EditRecord record;
  probe->OnEndEdit(&a, 1, &record);
  Check(record.selection_calls == 0);
  probe->OnEndEdit(&b, 1, &record);
  Check(record.selection_calls == 1);
  const auto epoch = events.back().identity_epoch;
  Check(epoch != 0);

  // A failed GetTop invalidates the former edit binding until a retry.
  second.top_hr = E_FAIL;
  probe->OnPushContext(&b);
  probe->OnEndEdit(&b, 1, &record);
  Check(record.selection_calls == 1);
  Check(events.back().kind == ContextEventKind::kBindFailure);
  second.top_hr = S_OK;
  probe->Checkpoint();
  Check(events.back().kind == ContextEventKind::kCheckpoint && events.back().hr == S_OK);
  probe->OnEndEdit(&b, 1, &record);
  Check(record.selection_calls == 2);

  // A failed context Advise is retried on the same context, not skipped.
  b.registration.unadvise_hr = S_OK;
  manager.focus = &first;
  probe->OnSetFocus(&first, &second);
  a.registration.advise_hr = E_FAIL;
  manager.focus = &first;
  probe->Checkpoint();
  Check(events.back().kind == ContextEventKind::kCheckpoint && events.back().hr == S_OK);
  // Force a different top, then return to A with Advise failure.
  first.top = &b;
  probe->OnPushContext(&b);
  first.top = &a;
  probe->OnPopContext(&b);
  Check(a.registration.advises >= 2);
  Check(events.back().kind == ContextEventKind::kBindFailure ||
        events.back().kind == ContextEventKind::kAttach);
  a.registration.advise_hr = S_OK;
  const unsigned attempts = a.registration.advises;
  probe->Checkpoint();
  Check(a.registration.advises == attempts + 1 &&
        events.back().kind == ContextEventKind::kCheckpoint && events.back().hr == S_OK);
  Check(probe->Stop() == S_OK);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0 && a.refs == 1 && b.refs == 1);

  auto* second_probe = new ContextEventProbe(Allowed, Emit);
  Check(second_probe->Start(&manager) == S_OK);
  Check(events.back().identity_epoch != epoch);
  Check(second_probe->Stop() == S_OK);
  second_probe->Release();
  Check(ziliu::tsf::module_refs == 0);
}

void TestTitleParser() {
  using ziliu::tsf::detail::ParseContextEventStep;
  using ziliu::tsf::detail::ParseContextShadowCase;
  Check(ParseContextEventStep(L"Ziliu CONTEXT EVENTS [0]") == 0);
  Check(ParseContextEventStep(L"Ziliu CONTEXT EVENTS [9] - Microsoft Edge") == 9);
  Check(!ParseContextEventStep(L"Ziliu CONTEXT EVENTS [10]"));
  Check(!ParseContextEventStep(L"Ziliu CONTEXT EVENTS [0]forged"));
  Check(!ParseContextEventStep(L"Ziliu CONTEXT EVENTS [x]"));
  Check(!ParseContextEventStep(L"Not Ziliu CONTEXT EVENTS [1]"));
  Check(ParseContextShadowCase(L"Ziliu CONTEXT SHADOW [6] - Microsoft Edge") == 6);
  Check(!ParseContextShadowCase(L"Ziliu CONTEXT SHADOW [7]"));
  Check(!ParseContextShadowCase(L"Ziliu CONTEXT SHADOW [6]forged"));
  Check(ParseContextFreshCase(L"Ziliu CONTEXT FRESH [7] - Microsoft Edge") == 7);
  Check(!ParseContextFreshCase(L"Ziliu CONTEXT FRESH [8]"));
  Check(!ParseContextFreshCase(L"Ziliu CONTEXT FRESH [0]forged"));
  Check(!ParseContextFreshCase(L"Ziliu CONTEXT SHADOW [0]"));
}

void TestFailedRebindUnadvise() {
  events.clear(); allowed = true;
  Context a, b;
  Document first, second;
  first.top = &a; second.top = &b;
  Manager manager; manager.focus = &first;
  auto* probe = new ContextEventProbe(Allowed, Emit);
  Check(probe->Start(&manager) == S_OK);
  a.registration.unadvise_hr = E_FAIL;
  manager.focus = &second;
  probe->OnSetFocus(&second, &first);
  EditRecord record;
  probe->OnEndEdit(&a, 1, &record);
  Check(record.selection_calls == 0 && b.registration.advises == 0);
  Check(events.back().kind == ContextEventKind::kBindFailure && events.back().hr == E_FAIL);
  a.registration.unadvise_hr = S_OK;
  probe->Checkpoint();
  Check(a.registration.unadvises == 2 && b.registration.advises == 1 &&
        events.back().kind == ContextEventKind::kCheckpoint && events.back().hr == S_OK);
  b.registration.unadvise_hr = E_FAIL;
  Check(probe->Stop() == E_FAIL);
  const auto count = events.size();
  probe->OnEndEdit(&b, 1, &record);
  Check(record.selection_calls == 0 && events.size() == count);
  b.registration.unadvise_hr = S_OK;
  Check(probe->Stop() == S_OK && b.registration.unadvises == 2);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0);
}

using GuardClock = ziliu::core::ContextRequestGuard::Clock;
using GuardTime = ziliu::core::ContextRequestGuard::TimePoint;

ziliu::core::ContextRequestSnapshot Snapshot(std::uint64_t field_epoch = 1,
                                             std::uint64_t edit_revision = 1) {
  return {.field_token = {0x11, 0x22}, .field_epoch = field_epoch,
          .edit_revision = edit_revision, .selection_start_utf16 = 0,
          .selection_end_utf16 = 0, .input_revision = 1, .candidate_revision = 1,
          .broker_instance = {1, 1}, .engine_session_id = 1, .dictionary_epoch = 1,
          .privacy = ziliu::core::ContextPrivacy::ordinary,
          .identity_verified = true, .fresh_read_ok = true};
}

ContextEventProbe* reentrant_probe = nullptr;
ziliu::core::ContextRequestSnapshot reentrant_snapshot{};
std::optional<ziliu::core::ContextRequestTicket> reentrant_ticket;
GuardTime reentrant_now{};
bool reentrant_observe_result = true;
bool reentrant_begin_result = true;
bool reentrant_accept_result = true;
unsigned reentrant_calls = 0;

void EmitReentrant(const ContextEvent& event) {
  events.push_back(event);
  if (!reentrant_probe) return;
  ++reentrant_calls;
  reentrant_observe_result = reentrant_probe->ObserveSyntheticRequest(reentrant_snapshot);
  reentrant_begin_result = reentrant_probe->BeginSyntheticRequest(
      reentrant_now, reentrant_now + std::chrono::seconds(5)).has_value();
  reentrant_accept_result = reentrant_ticket && reentrant_probe->TryAcceptSyntheticResult(
      *reentrant_ticket, reentrant_snapshot, reentrant_now + std::chrono::seconds(1));
}

void TestReentrantEmitCannotArmRequests() {
  using ziliu::core::ContextLifetimeToken;
  events.clear(); allowed = true;
  Context a, b;
  Document first, second;
  first.top = &a; second.top = &b;
  Manager manager; manager.focus = &first;
  auto* probe = new ContextEventProbe(Allowed, EmitReentrant, ContextLifetimeToken{0x77, 0x88});
  Check(probe->Start(&manager) == S_OK);
  reentrant_probe = probe;
  reentrant_snapshot = Snapshot();
  reentrant_now = GuardTime{} + std::chrono::seconds(30);
  Check(probe->ObserveSyntheticRequest(reentrant_snapshot));
  reentrant_ticket = probe->BeginSyntheticRequest(
      reentrant_now, reentrant_now + std::chrono::seconds(5));
  Check(reentrant_ticket.has_value());

  auto* sink = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  manager.focus = &second;
  const unsigned before = reentrant_calls;
  Check(sink->OnSetFocus(&second, &first) == S_OK);
  sink->Release();
  Check(reentrant_calls == before + 2); // focus event and the resulting attach event
  Check(!reentrant_observe_result && !reentrant_begin_result && !reentrant_accept_result);

  // Once the outer callback transition has unwound, explicit fresh observation works.
  Check(probe->ObserveSyntheticRequest(reentrant_snapshot));
  auto fresh = probe->BeginSyntheticRequest(
      reentrant_now, reentrant_now + std::chrono::seconds(5));
  Check(fresh.has_value());
  Check(probe->TryAcceptSyntheticResult(*fresh, reentrant_snapshot,
                                        reentrant_now + std::chrono::seconds(1)));
  reentrant_probe = nullptr;
  reentrant_ticket.reset();
  Check(probe->Stop() == S_OK);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0);
}

void TestSyntheticRequestBoundaries() {
  using ziliu::core::ContextLifetimeToken;
  events.clear(); allowed = true;
  Context a, b;
  Document first, second;
  first.top = &a; second.top = &a; // Distinct documents, same COM context identity.
  Manager manager; manager.focus = &first;
  auto* probe = new ContextEventProbe(Allowed, Emit, ContextLifetimeToken{0x33, 0x44});
  Check(probe->Start(&manager) == S_OK);
  auto* thread_sink = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  auto* text_sink = a.registration.GetSink<ITfTextEditSink>(IID_ITfTextEditSink);
  Check(thread_sink && text_sink);

  const auto now = GuardTime{} + std::chrono::seconds(10);
  const auto deadline = now + std::chrono::seconds(5);
  const auto state_a = Snapshot();
  Check(probe->ObserveSyntheticRequest(state_a));
  auto old = probe->BeginSyntheticRequest(now, deadline);
  Check(old.has_value());
  // A-B-A with identical synthetic text/state cannot preserve work across focus.
  manager.focus = &second;
  Check(thread_sink->OnSetFocus(&second, &first) == S_OK);
  manager.focus = &first;
  Check(thread_sink->OnSetFocus(&first, &second) == S_OK);
  Check(!probe->TryAcceptSyntheticResult(*old, state_a, now + std::chrono::seconds(1)));
  thread_sink->Release();
  Check(probe->ObserveSyntheticRequest(state_a));
  auto push_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(push_ticket.has_value());
  thread_sink = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  Check(thread_sink->OnPushContext(&a) == S_OK);
  thread_sink->Release();
  Check(!probe->TryAcceptSyntheticResult(*push_ticket, state_a, now + std::chrono::seconds(1)));
  Check(probe->ObserveSyntheticRequest(state_a));
  auto pop_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(pop_ticket.has_value());
  thread_sink = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  Check(thread_sink->OnPopContext(&a) == S_OK);
  thread_sink->Release();
  Check(!probe->TryAcceptSyntheticResult(*pop_ticket, state_a, now + std::chrono::seconds(1)));
  Check(probe->ObserveSyntheticRequest(state_a));
  auto uninit_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(uninit_ticket.has_value());
  thread_sink = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  Check(thread_sink->OnUninitDocumentMgr(&first) == S_OK);
  thread_sink->Release();
  Check(!probe->TryAcceptSyntheticResult(*uninit_ticket, state_a, now + std::chrono::seconds(1)));
  probe->Checkpoint();
  Check(probe->ObserveSyntheticRequest(state_a));
  text_sink->Release();

  // A fresh caller observation is required; no callback authorizes a snapshot.
  Check(probe->ObserveSyntheticRequest(state_a));
  auto ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(ticket.has_value());
  // A successful checkpoint which leaves the binding intact is not an edit.
  probe->Checkpoint();
  Check(probe->TryAcceptSyntheticResult(*ticket, state_a, now + std::chrono::seconds(1)));

  // Old results cannot cancel a newer in-flight request.
  Check(probe->ObserveSyntheticRequest(state_a));
  auto earlier = probe->BeginSyntheticRequest(now, deadline);
  Check(earlier.has_value());
  auto newer = probe->BeginSyntheticRequest(now, deadline);
  Check(newer.has_value());
  Check(!probe->TryAcceptSyntheticResult(*earlier, state_a, now + std::chrono::seconds(1)));
  Check(probe->TryAcceptSyntheticResult(*newer, state_a, now + std::chrono::seconds(1)));

  // Edit notifications invalidate before inspecting any record, including null.
  Check(probe->ObserveSyntheticRequest(state_a));
  auto edit_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(edit_ticket.has_value());
  EditRecord record;
  auto* active_text_sink = a.registration.GetSink<ITfTextEditSink>(IID_ITfTextEditSink);
  Check(active_text_sink->OnEndEdit(&a, 7, &record) == S_OK);
  active_text_sink->Release();
  Check(record.selection_calls == 1 && record.update_calls == 1);
  Check(!probe->TryAcceptSyntheticResult(*edit_ticket, state_a, now + std::chrono::seconds(1)));

  Check(probe->ObserveSyntheticRequest(state_a));
  auto null_record_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(null_record_ticket.has_value());
  active_text_sink = a.registration.GetSink<ITfTextEditSink>(IID_ITfTextEditSink);
  Check(active_text_sink->OnEndEdit(&a, 8, nullptr) == S_OK);
  active_text_sink->Release();
  Check(!probe->TryAcceptSyntheticResult(*null_record_ticket, state_a, now + std::chrono::seconds(1)));

  // With logging disabled, the record must not be read, but pending work dies.
  Check(probe->ObserveSyntheticRequest(state_a));
  auto denied_log_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(denied_log_ticket.has_value());
  allowed = false;
  record.selection_calls = record.update_calls = 0;
  active_text_sink = a.registration.GetSink<ITfTextEditSink>(IID_ITfTextEditSink);
  Check(active_text_sink->OnEndEdit(&a, 9, &record) == S_OK);
  active_text_sink->Release();
  allowed = true;
  Check(record.selection_calls == 0 && record.update_calls == 0);
  Check(!probe->TryAcceptSyntheticResult(*denied_log_ticket, state_a, now + std::chrono::seconds(1)));

  // An edit on an unbound/stale context still invalidates but is not read.
  Check(probe->ObserveSyntheticRequest(state_a));
  auto stale_context_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(stale_context_ticket.has_value());
  record.selection_calls = record.update_calls = 0;
  active_text_sink = a.registration.GetSink<ITfTextEditSink>(IID_ITfTextEditSink);
  Check(active_text_sink->OnEndEdit(&b, 10, &record) == S_OK);
  active_text_sink->Release();
  Check(record.selection_calls == 0 && record.update_calls == 0);
  Check(!probe->TryAcceptSyntheticResult(*stale_context_ticket, state_a, now + std::chrono::seconds(1)));

  // Selection/text changes, including deletion and undo, use caller revisions;
  // the probe rejects the old ticket after each active edit notification.
  for (std::uint64_t revision : {2ull, 3ull, 4ull}) {
    Check(probe->ObserveSyntheticRequest(Snapshot(1, revision)));
    auto changed_ticket = probe->BeginSyntheticRequest(now, deadline);
    Check(changed_ticket.has_value());
    active_text_sink = a.registration.GetSink<ITfTextEditSink>(IID_ITfTextEditSink);
    Check(active_text_sink->OnEndEdit(&a, 11, &record) == S_OK);
    active_text_sink->Release();
    Check(!probe->TryAcceptSyntheticResult(*changed_ticket, Snapshot(1, revision),
                                            now + std::chrono::seconds(1)));
  }

  Check(probe->ObserveSyntheticRequest(state_a));
  auto privacy_ticket = probe->BeginSyntheticRequest(now, deadline);
  Check(privacy_ticket.has_value());
  auto restricted = state_a;
  restricted.privacy = ziliu::core::ContextPrivacy::restricted;
  Check(!probe->TryAcceptSyntheticResult(*privacy_ticket, restricted,
                                         now + std::chrono::seconds(1)));
  auto unread = state_a;
  unread.fresh_read_ok = false;
  Check(!probe->ObserveSyntheticRequest(unread));
  Check(!probe->BeginSyntheticRequest(now, deadline).has_value());
  auto unverified = state_a;
  unverified.identity_verified = false;
  Check(!probe->ObserveSyntheticRequest(unverified));

  Check(probe->Stop() == S_OK);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0);
}

void TestSyntheticFailureAndClosedOwner() {
  using ziliu::core::ContextLifetimeToken;
  const auto now = GuardTime{} + std::chrono::seconds(20);
  const auto deadline = now + std::chrono::seconds(5);
  const auto state = Snapshot();
  events.clear(); allowed = true;
  Context a;
  Document document; document.top = &a;
  Manager manager; manager.focus = &document;
  auto* probe = new ContextEventProbe(Allowed, Emit, ContextLifetimeToken{0x55, 0x66});
  Check(probe->Start(&manager) == S_OK);
  auto* thread_sink = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  Check(probe->ObserveSyntheticRequest(state));
  auto pending = probe->BeginSyntheticRequest(now, deadline);
  Check(pending.has_value());
  document.top_hr = E_FAIL;
  Check(thread_sink->OnPushContext(reinterpret_cast<ITfContext*>(&a)) == S_OK);
  Check(!probe->TryAcceptSyntheticResult(*pending, state, now + std::chrono::seconds(1)));
  document.top_hr = S_OK;
  probe->Checkpoint(); // Rebind succeeds; caller still has to observe afresh.
  Check(probe->ObserveSyntheticRequest(state));
  pending = probe->BeginSyntheticRequest(now, deadline);
  Check(pending.has_value());
  manager.registration.unadvise_hr = E_FAIL;
  Check(probe->Stop() == E_FAIL);
  Check(!probe->TryAcceptSyntheticResult(*pending, state, now + std::chrono::seconds(1)));
  manager.registration.unadvise_hr = S_OK;
  Check(probe->Stop() == S_OK);
  thread_sink->Release();
  probe->Release();

  // The legacy/default owner is intentionally closed even with valid snapshots.
  auto* closed = new ContextEventProbe(Allowed, Emit);
  Check(closed->Start(&manager) == S_OK);
  Check(!closed->ObserveSyntheticRequest(state));
  Check(!closed->BeginSyntheticRequest(now, deadline).has_value());
  Check(closed->Stop() == S_OK);
  closed->Release();
  Check(ziliu::tsf::module_refs == 0);
}

void TestContextShadowTrial() {
  using ziliu::core::ContextLifetimeToken;
  using TimePoint = ContextShadowTrial::TimePoint;
  const TimePoint start = TimePoint{} + std::chrono::seconds(100);
  events.clear(); allowed = true;
  Context a;
  Document first, second;
  first.top = &a; second.top = &a;
  Manager manager; manager.focus = &first;
  auto* probe = new ContextEventProbe(Allowed, Emit, ContextLifetimeToken{0x71, 0x82});
  Check(probe->Start(&manager) == S_OK);
  ContextShadowTrial trial;

  auto armed = trial.Arm(*probe, 0, start);
  Check(armed.phase == 1 && armed.sequence == 1 && armed.scenario == 0 && !armed.accepted);
  auto delivered = trial.Deliver(*probe, 0, start + std::chrono::seconds(2));
  Check(delivered[0].phase == 2 && delivered[0].accepted && delivered[0].age_ms == 2000);
  delivered = trial.Deliver(*probe, 0, start + std::chrono::seconds(2));
  Check(delivered[0].phase == 5 && !delivered[0].accepted);

  // The delay gate must retain the ticket, allowing delivery once mature.
  armed = trial.Arm(*probe, 0, start + std::chrono::seconds(3));
  Check(armed.phase == 1);
  delivered = trial.Deliver(*probe, 0, start + std::chrono::seconds(4));
  Check(delivered[0].phase == 4 && delivered[0].age_ms == 1000);
  delivered = trial.Deliver(*probe, 0, start + std::chrono::seconds(5));
  Check(delivered[0].phase == 2 && delivered[0].accepted);

  // A second same-scenario arm supersedes the core ticket without dropping the
  // shadow slot: only the newer request can be accepted.
  const auto pair_start = start + std::chrono::seconds(6);
  const auto older = trial.Arm(*probe, 0, pair_start);
  const auto newer = trial.Arm(*probe, 0, pair_start + std::chrono::milliseconds(10));
  Check(older.phase == 1 && newer.phase == 1 && older.sequence + 1 == newer.sequence);
  delivered = trial.Deliver(*probe, 0, pair_start + std::chrono::milliseconds(2010));
  Check(delivered[0].phase == 2 && !delivered[0].accepted);
  Check(delivered[1].phase == 2 && delivered[1].accepted);

  // Focus invalidates the old ticket even if the newly focused document has
  // the same synthetic context identity; a fresh arm is accepted.
  auto* thread_sink = manager.registration.GetSink<ITfThreadMgrEventSink>(IID_ITfThreadMgrEventSink);
  Check(thread_sink != nullptr);
  const auto focus_start = start + std::chrono::seconds(9);
  Check(trial.Arm(*probe, 0, focus_start).phase == 1);
  manager.focus = &second;
  Check(thread_sink->OnSetFocus(&second, &first) == S_OK);
  Check(trial.Arm(*probe, 0, focus_start + std::chrono::milliseconds(10)).phase == 1);
  delivered = trial.Deliver(*probe, 0, focus_start + std::chrono::milliseconds(2010));
  Check(delivered[0].phase == 2 && !delivered[0].accepted);
  Check(delivered[1].phase == 2 && delivered[1].accepted);
  thread_sink->Release();

  // EndEdit also cancels outstanding work before inspecting the record.
  const auto edit_start = start + std::chrono::seconds(12);
  Check(trial.Arm(*probe, 0, edit_start).phase == 1);
  EditRecord record;
  auto* text_sink = a.registration.GetSink<ITfTextEditSink>(IID_ITfTextEditSink);
  Check(text_sink && text_sink->OnEndEdit(&a, 12, &record) == S_OK);
  text_sink->Release();
  delivered = trial.Deliver(*probe, 0, edit_start + std::chrono::seconds(2));
  Check(delivered[0].phase == 2 && !delivered[0].accepted);

  // The core deadline is 30 seconds and expired tickets are rejected.
  Check(trial.Arm(*probe, 0, start + std::chrono::seconds(15)).phase == 1);
  delivered = trial.Deliver(*probe, 0, start + std::chrono::seconds(46));
  Check(delivered[0].phase == 2 && !delivered[0].accepted && delivered[0].age_ms == 31000);

  // Third retained slot fails closed and cancels both in-flight core tickets.
  const auto overflow_start = start + std::chrono::seconds(47);
  Check(trial.Arm(*probe, 0, overflow_start).phase == 1);
  Check(trial.Arm(*probe, 0, overflow_start + std::chrono::milliseconds(1)).phase == 1);
  armed = trial.Arm(*probe, 0, overflow_start + std::chrono::milliseconds(2));
  Check(armed.phase == 3);
  delivered = trial.Deliver(*probe, 0, overflow_start + std::chrono::seconds(2));
  Check(delivered[0].phase == 5 && delivered[1].phase == 0);

  // Scenario mismatch cancels; changing scenario clears old slots and allows
  // fresh work. Invalid scenario is denied as well.
  Check(trial.Arm(*probe, 1, start + std::chrono::seconds(50)).phase == 1);
  delivered = trial.Deliver(*probe, 2, start + std::chrono::seconds(52));
  Check(delivered[0].phase == 5 && delivered[1].phase == 0);
  Check(trial.Arm(*probe, 2, start + std::chrono::seconds(53)).phase == 1);
  Check(trial.Arm(*probe, 99, start + std::chrono::seconds(54)).phase == 3);
  delivered = trial.Deliver(*probe, 2, start + std::chrono::seconds(55));
  Check(delivered[0].phase == 5);

  // Real fixed-fixture prefix read is an extra veto, not a field identity.
  Check(trial.Arm(*probe, 0, start + std::chrono::seconds(56), false).phase == 3);
  Check(trial.Deliver(*probe, 0, start + std::chrono::seconds(58))[0].phase == 5);
  Check(trial.Arm(*probe, 0, start + std::chrono::seconds(59), true).phase == 1);
  delivered = trial.Deliver(*probe, 0, start + std::chrono::seconds(61), false);
  Check(delivered[0].phase == 2 && !delivered[0].accepted);
  Check(trial.Arm(*probe, 0, start + std::chrono::seconds(62)).phase == 1);
  Check(trial.Deliver(*probe, 0, start + std::chrono::seconds(63), false)[0].phase == 4);
  // Even a premature failed read permanently invalidates that ticket.
  Check(!trial.Deliver(*probe, 0, start + std::chrono::seconds(64), true)[0].accepted);

  const auto read_revision = probe->CaptureReadRevision();
  Check(read_revision.has_value() && probe->IsReadRevisionCurrent(*read_revision));
  probe->Checkpoint();
  Check(probe->IsReadRevisionCurrent(*read_revision));
  probe->OnEndEdit(&a, 13, nullptr);
  Check(!probe->IsReadRevisionCurrent(*read_revision));
  const auto second_revision = probe->CaptureReadRevision();
  Check(second_revision.has_value());
  probe->OnSetFocus(&second, &first);
  Check(!probe->IsReadRevisionCurrent(*second_revision));

  Check(probe->Stop() == S_OK);
  probe->Release();
  Check(ziliu::tsf::module_refs == 0);
}
}

int main() {
  {
    using namespace ziliu::tsf::detail;
    ContextEvent event{};
    event.kind = ContextEventKind::kCheckpoint;
    event.hr = S_OK;
    for (int i = 0; i < 1000; ++i) Check(!ShouldTraceContextEvent(event));
    event.hr = E_UNEXPECTED;
    Check(ShouldTraceContextEvent(event));
    event.hr = S_OK;
    for (const auto kind : {ContextEventKind::kFocus, ContextEventKind::kAttach,
                            ContextEventKind::kEndEdit, ContextEventKind::kStop}) {
      event.kind = kind;
      Check(ShouldTraceContextEvent(event));
    }
    Check(ContextKeyCategory(L'N') == 1 && ContextKeyCategory(L'O') == 1);
    Check(ContextKeyCategory(VK_ESCAPE) == 2 && ContextKeyCategory(VK_BACK) == 3);
    Check(ContextKeyCategory(VK_CONTROL) == 0 && ContextKeyCategory(VK_SPACE) == 0);
    Check(!ContextKeyStateProbeAllowed());  // No Edge/marker opt-in in unit tests.
  }
  TestLifecycle();
  TestFailures();
  TestBindingRecovery();
  TestFailedRebindUnadvise();
  TestReentrantEmitCannotArmRequests();
  TestSyntheticRequestBoundaries();
  TestSyntheticFailureAndClosedOwner();
  TestContextShadowTrial();
  TestTitleParser();
  return 0;
}
