#include <windows.h>

#include <shellapi.h>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Compile the actual production implementation. Only external transport,
// presentation and process-launch effects are replaced; no control-flow copy.
static HINSTANCE WINAPI ForbiddenShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) {
  throw std::runtime_error("Test attempted to launch a process");
}
static UINT WINAPI SyntheticSendInput(UINT, LPINPUT, int) {
  throw std::runtime_error("Test attempted to synthesize host input");
}
static BOOL WINAPI ForbiddenCreateProcess(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                          LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                          LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
  throw std::runtime_error("Test attempted to start a broker process");
}
// Keep window identity deterministic without taking control of the desktop.
// Qualified COM GetFocus calls still use the host double below.
namespace ziliu::tsf {
namespace {
HWND GetFocus() { return nullptr; }
HWND GetForegroundWindow() { return nullptr; }
}  // namespace
}  // namespace ziliu::tsf
#define ShellExecuteW ForbiddenShellExecute
#define SendInput SyntheticSendInput
#define CreateProcessW ForbiddenCreateProcess
#include "../src/tsf/src/text_service.cpp"
#undef ShellExecuteW
#undef SendInput
#undef CreateProcessW
#include "tsf_text_service_test_host.h"

namespace {
using ziliu::tsf::test::Event;
using ziliu::tsf::test::Expect;
namespace wire = ziliu::core::ipc;
std::function<void(const wire::Request&)> exchange_callback;
std::vector<wire::Request> exchanges;
wire::Response reply;
unsigned presentations = 0;
unsigned hidden = 0;
long modules = 0;
}  // namespace
namespace ziliu::tsf {
void AddModuleReference() noexcept { ++modules; }
void ReleaseModuleReference() noexcept { --modules; }
long ModuleReferenceCount() noexcept { return modules; }
HINSTANCE ModuleInstance() noexcept { return nullptr; }
}  // namespace ziliu::tsf
namespace ziliu::ipc {
PipeClient::PipeClient(std::wstring name, std::uint32_t timeout)
    : pipe_name_(std::move(name)), timeout_milliseconds_(timeout) {}
bool PipeClient::IsServerAvailable() const noexcept { return true; }
std::optional<core::ipc::Response> PipeClient::Exchange(const core::ipc::Request& request) const {
  exchanges.push_back(request);
  auto result = reply;
  result.request_id = request.request_id;
  result.session_id = request.command == wire::Command::kCreateSession ? 71 : request.session_id;
  auto callback = exchange_callback;
  if (callback) callback(request);
  return result;
}
}  // namespace ziliu::ipc
namespace ziliu::ui {
CandidateWindow::~CandidateWindow() = default;
bool CandidateWindow::Create(HWND) {
  Event("UI.Create", this);
  return true;
}
void CandidateWindow::Show(const core::CompositionSnapshot&, const RECT&, const core::Settings&,
                           std::size_t) {
  ++presentations;
  Event("UI.Show", this);
}
void CandidateWindow::Hide() { ++hidden; }
void CandidateWindow::SetExpanded(bool) {}
void CandidateWindow::SetQuickMenuAction(std::function<void(POINT)>) {}
void CandidateWindow::SetThemeResourceLoader(ThemeResourceLoader) {}
}  // namespace ziliu::ui

namespace ziliu::tsf {
struct TextServiceTestPeer {
  static void Prepare(TextService& service, ITfContext* context, std::uint64_t session = 17) {
    service.activation_flags_ = 0;
    service.state_->key_context = context;
    service.state_->key_privacy = detail::InputPrivacy::kOrdinary;
    service.state_->session_id = session;
    service.state_->broker_started = true;
    service.state_->focus_session_dirty = false;
    service.state_->snapshot.preedit = L"ni";
    service.state_->pending_response.commit = L"old";
  }
  static TextServiceState& State(TextService& service) { return *service.state_; }
  static bool Focused(TextService& service, ITfContext* context) {
    return service.IsFocusedContext(context);
  }
  static bool Allow(TextService& service, ITfContext* context) {
    const TfEditCookie cookie = 1;
    return service.AllowKeyInContext(context, &cookie);
  }
  static bool Ensure(TextService& service) { return service.EnsureSession(); }
  static HRESULT Apply(TextService& service, ITfContext* context) {
    return service.ApplyCompositionEdit(1, context);
  }
  static HRESULT ApplyKey(TextService& service, ITfContext* context) {
    BOOL eaten = FALSE;
    return service.ApplyKeyResponse(context, 'N', &eaten);
  }
  static HRESULT Caret(TextService& service, ITfContext* context) {
    return service.VerifyCommittedPairCaret(1, context);
  }
  static bool Capture(TextService& service, ITfContext* context) {
    return service.CaptureStartupTarget(1, context, true);
  }
  static bool Validate(TextService& service, ITfContext* context) {
    return service.ValidateStartupTarget(1, context);
  }
  static HRESULT Commit(TextService& service, ITfContext* context, BOOL* eaten) {
    return service.CommitText(context, L"old", eaten);
  }
  static void Show(TextService& service) { service.ShowCandidateWindow(); }
};
}  // namespace ziliu::tsf
namespace {
using namespace ziliu::tsf::test;
using ziliu::tsf::TextService;
using Peer = ziliu::tsf::TextServiceTestPeer;

struct View final : ITfContextView {
  ULONG refs = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfContextView) return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP GetRangeFromPoint(TfEditCookie, const POINT*, DWORD, ITfRange**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP GetTextExt(TfEditCookie, ITfRange*, RECT* rectangle, BOOL* clipped) override {
    Event("View.GetTextExt", this);
    *rectangle = {1, 2, 3, 4};
    *clipped = FALSE;
    return S_OK;
  }
  STDMETHODIMP GetScreenExt(RECT*) override { return E_NOTIMPL; }
  STDMETHODIMP GetWnd(HWND* hwnd) override {
    Event("View.GetWnd", this);
    *hwnd = nullptr;
    return S_OK;
  }
};
struct HostContext final : Context {
  View view;
  STDMETHODIMP GetActiveView(ITfContextView** out) override {
    Event("Context.GetActiveView", this);
    *out = &view;
    view.AddRef();
    return S_OK;
  }
};
struct Document final : ITfDocumentMgr {
  ULONG refs = 1;
  ITfContext* context = nullptr;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfDocumentMgr) return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP CreateContext(TfClientId, DWORD, IUnknown*, ITfContext**, TfEditCookie*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP Push(ITfContext*) override { return E_NOTIMPL; }
  STDMETHODIMP Pop(DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP GetTop(ITfContext** out) override {
    Event("Document.GetTop", this);
    *out = context;
    if (context) context->AddRef();
    return context ? S_OK : S_FALSE;
  }
  STDMETHODIMP GetBase(ITfContext**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumContexts(IEnumTfContexts**) override { return E_NOTIMPL; }
};
struct Manager final : ITfThreadMgr {
  ULONG refs = 1;
  Document* document = nullptr;
  bool check_lifetime = false;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfThreadMgr) return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
  STDMETHODIMP_(ULONG) Release() override { return --refs; }
  STDMETHODIMP Activate(TfClientId*) override { return E_NOTIMPL; }
  STDMETHODIMP Deactivate() override { return E_NOTIMPL; }
  STDMETHODIMP CreateDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumDocumentMgrs(IEnumTfDocumentMgrs**) override { return E_NOTIMPL; }
  STDMETHODIMP GetFocus(ITfDocumentMgr** out) override {
    Event("Manager.GetFocus", this);
    // Stack-owned lifetime sentinel: detect last Release during the call without
    // deliberately executing a use-after-free to make the regression fail.
    if (check_lifetime) Expect(refs > 0, "manager lost its last COM reference inside GetFocus");
    *out = document;
    if (document) document->AddRef();
    return document ? S_OK : S_FALSE;
  }
  STDMETHODIMP SetFocus(ITfDocumentMgr*) override { return E_NOTIMPL; }
  STDMETHODIMP AssociateFocus(HWND, ITfDocumentMgr*, ITfDocumentMgr**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP IsThreadFocus(BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetFunctionProvider(REFCLSID, ITfFunctionProvider**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumFunctionProviders(IEnumTfFunctionProviders**) override { return E_NOTIMPL; }
  STDMETHODIMP GetGlobalCompartment(ITfCompartmentMgr**) override { return E_NOTIMPL; }
};
struct Fixture {
  HostContext a, b;
  Scope scope;
  Document document;
  Manager manager;
  TextService* service = new TextService();
  Fixture() {
    exchanges.clear();
    exchange_callback = {};
    host_callback = {};
    reply = {};
    reply.consumed = true;
    reply.snapshot.preedit = L"old-reply";
    presentations = hidden = 0;
    scope.values = {IS_DEFAULT};
    for (auto* context : {&a, &b}) {
      context->property_present = true;
      context->property_result = S_OK;
      context->property.scope = &scope;
      context->range.empty = true;
      context->range.allow_text_read = true;
    }
    document.context = &a;
    manager.document = &document;
    Expect(service->ActivateEx(&manager, 1, TF_TMAE_SECUREMODE) == S_OK, "secure in-memory setup");
    Peer::Prepare(*service, &a);
  }
  ~Fixture() {
    host_callback = {};
    exchange_callback = {};
    service->Release();
  }
  void Switch() {
    service->OnKillThreadFocus();
    document.context = &b;
    Peer::Prepare(*service, &b, 88);
    Peer::State(*service).pending_response.commit = L"new";
    Peer::State(*service).snapshot.preedit = L"new";
  }
  void Once(std::string_view name, std::function<void()> callback) {
    host_callback = [name, callback = std::move(callback)](std::string_view event, void*) {
      if (event == name) {
        host_callback = {};
        callback();
      }
    };
  }
};

void CommitControl() {
  Fixture f;
  Expect(Peer::Apply(*f.service, &f.a) == S_OK, "ordinary commit control");
  Expect(f.a.range.writes == 1 && f.a.range.text == L"old" && f.a.selections == 1,
         "commit/selection control actually ran");
}
void PrivacyReentry() {
  Fixture f;
  f.Once("Scope.GetInputScopes", [&] { f.Switch(); });
  Expect(!Peer::Allow(*f.service, &f.a), "scope reentry rejects old key");
  Expect(Peer::State(*f.service).key_context.Get() == &f.b,
         "scope reentry preserves replacement context");
  Expect(f.a.range.writes == 0, "no write after privacy reentry");
}
void QueuedEdit() {
  Fixture f;
  auto* edit = new ziliu::tsf::CompositionEditSession(f.service, &f.a);
  f.Switch();
  Expect(edit->DoEditSession(1) == E_ABORT, "queued old edit rejected");
  edit->Release();
  Expect(f.a.selection_calls == 0 && f.a.range.writes == 0, "stale queued edit did not touch host");
}
void SelectionReentry() {
  Fixture f;
  unsigned count = 0;
  host_callback = [&](std::string_view name, void*) {
    // First selection is the privacy gate; second is the actual commit range.
    if (name == "Context.GetSelection" && ++count == 2) f.Switch();
  };
  Expect(Peer::Apply(*f.service, &f.a) == E_ABORT, "commit selection reentry aborts");
  Expect(count == 2 && f.a.range.writes == 0 && f.b.range.writes == 0,
         "actual commit range reached, no cross-field writes");
}
void WriteReentry() {
  Fixture f;
  f.Once("Range.SetText", [&] { f.Switch(); });
  Expect(Peer::Apply(*f.service, &f.a) == E_ABORT, "post-write focus changed");
  Expect(f.a.range.writes == 1 && f.a.selections == 0 && f.b.range.writes == 0,
         "accepted old write does not collapse or cross fields");
  Expect(Peer::State(*f.service).pending_response.commit == L"new",
         "old write does not overwrite new payload");
}
void ViewReentry(std::string_view call) {
  Fixture f;
  f.Once(call, [&] { f.Switch(); });
  Expect(Peer::Apply(*f.service, &f.a) == E_ABORT, "view reentry aborts old selection update");
  Expect(f.a.selections == 0 && presentations == 0, "no stale selection/presentation");
}
void CollapseReentry() {
  Fixture f;
  unsigned count = 0;
  host_callback = [&](std::string_view name, void*) {
    if (name == "Range.Collapse" && ++count == 2) f.Switch();
  };
  Expect(Peer::Apply(*f.service, &f.a) == E_ABORT, "commit collapse reentry aborts");
  Expect(f.a.selections == 0, "old collapse not published as selection");
}
void CreateSessionReentry() {
  Fixture f;
  Peer::State(*f.service).session_id = 0;
  exchange_callback = [&](const wire::Request& r) {
    if (r.command == wire::Command::kCreateSession) f.Switch();
  };
  Expect(!Peer::Ensure(*f.service), "stale created session rejected");
  Expect(Peer::State(*f.service).session_id == 88, "replacement session preserved");
  Expect(exchanges.size() == 2 && exchanges[1].command == wire::Command::kCloseSession &&
             exchanges[1].session_id == 71,
         "only abandoned response session closed");
}
void InputReplyReentry() {
  Fixture f;
  exchange_callback = [&](const wire::Request& r) {
    if (r.command == wire::Command::kInputLetter) f.Switch();
  };
  Expect(Peer::ApplyKey(*f.service, &f.a) == E_ABORT, "stale input reply rejected");
  Expect(Peer::State(*f.service).snapshot.preedit == L"new" && f.a.range.writes == 0,
         "late reply does not replace new state or commit");
}
void ManagerLifetime() {
  Fixture f;
  f.manager.Release();
  f.manager.check_lifetime = true;
  f.Once("Manager.GetFocus", [&] { f.service->Deactivate(); });
  Expect(!Peer::Focused(*f.service, &f.a), "deactivation rejects focused context");
  Expect(f.manager.refs == 0, "manager reference released after callback frame");
}
void ContextLifetime() {
  Fixture f;
  auto* edit = new ziliu::tsf::CompositionEditSession(f.service, &f.a);
  f.a.Release();
  f.Once("Scope.GetInputScopes", [&] {
    f.service->Deactivate();
    Expect(f.a.references > 0, "edit frame retains context through deactivation");
  });
  Expect(edit->DoEditSession(1) == E_ABORT, "deactivated edit aborts");
  edit->Release();
  Expect(f.a.references == 0, "context references balanced");
}
void CaretReentry() {
  Fixture f;
  auto& s = Peer::State(*f.service);
  s.committed_pair_range = &f.a.range;
  s.committed_pair_context = &f.a;
  s.committed_pair = L"()";
  f.a.range.text = L"()";
  f.Once("Range.GetText", [&] { f.Switch(); });
  Expect(Peer::Caret(*f.service, &f.a) == S_FALSE, "caret read reentry rejected");
  Expect(!s.committed_pair_needs_left && f.a.selection_calls == 0,
         "no stale caret action or followup selection");
}
void CaptureReentry() {
  Fixture f;
  f.Once("Context.GetSelection", [&] { f.Switch(); });
  Expect(!Peer::Capture(*f.service, &f.a), "startup capture reentry rejected");
  Expect(!Peer::State(*f.service).startup_selection, "no stale startup selection retained");
}

void CommitSessionControl() {
  Fixture f;
  BOOL eaten = FALSE;
  Expect(Peer::Commit(*f.service, &f.a, &eaten) == S_OK && eaten,
         "actual synchronous edit commits");
  Expect(f.a.nested_edit_requests == 1 && f.a.range.text == L"old" && f.a.selections == 1,
         "actual CompositionEditSession ran");
}
void SelectionPublishReentry() {
  Fixture f;
  BOOL eaten = FALSE;
  f.Once("Context.SetSelection", [&] { f.Switch(); });
  Expect(Peer::Commit(*f.service, &f.a, &eaten) == E_ABORT && !eaten,
         "outer commit rejects reentered selection publish");
  Expect(f.a.selections == 1 && f.b.range.writes == 0,
         "old host accepted selection without writing new host");
  Expect(Peer::State(*f.service).pending_response.commit == L"new",
         "outer commit preserves new pending state");
}
void ReusedContextEpoch() {
  Fixture f;
  auto* edit = new ziliu::tsf::CompositionEditSession(f.service, &f.a);
  f.Switch();
  f.service->OnKillThreadFocus();
  f.document.context = &f.a;
  Peer::Prepare(*f.service, &f.a, 99);
  Expect(edit->DoEditSession(1) == E_ABORT, "A to B to A still invalidates old edit");
  edit->Release();
  Expect(f.a.selection_calls == 0 && Peer::State(*f.service).session_id == 99,
         "context pointer reuse cannot revive stale edit");
}
void FocusLookupReentry(std::string_view call) {
  Fixture f;
  Expect(Peer::Focused(*f.service, &f.a), "focused context control");
  f.Once(call, [&] { f.Switch(); });
  Expect(!Peer::Focused(*f.service, &f.a), "reentered focus lookup is rejected");
}
void SessionOptionReentry(wire::Command command) {
  Fixture f;
  Peer::State(*f.service).session_id = 0;
  std::size_t boundary = 0;
  exchange_callback = [&](const wire::Request& request) {
    if (request.command == command) {
      boundary = exchanges.size();
      f.Switch();
    }
  };
  Expect(!Peer::Ensure(*f.service), "session options stop after reentry");
  Expect(boundary != 0, "option callback actually ran");
  // OnKillThreadFocus may close the originating session during the callback.
  // Only that cleanup is allowed; later options must never reach session 88.
  for (std::size_t i = boundary; i < exchanges.size(); ++i) {
    Expect(exchanges[i].command == wire::Command::kCloseSession && exchanges[i].session_id == 71,
           "only old-session cleanup may follow the option callback");
  }
  Expect(Peer::State(*f.service).session_id == 88 &&
             Peer::State(*f.service).snapshot.preedit == L"new",
         "session option does not replace new state");
}
void CandidateControl() {
  Fixture f;
  Peer::Show(*f.service);
  Expect(presentations == 1, "actual candidate presentation path ran");
}
void CandidateReentry(std::string_view call) {
  Fixture f;
  unsigned hidden_during_switch = 0;
  f.Once(call, [&] {
    f.Switch();
    hidden_during_switch = hidden;
  });
  Peer::Show(*f.service);
  Expect(presentations == (call == "UI.Create" ? 0U : 1U),
         "stale create cannot show; reentered show is retired");
  Expect(hidden > hidden_during_switch, "stale presentation hidden after callback returns");
  Expect(Peer::State(*f.service).snapshot.preedit == L"new",
         "presentation preserves replacement snapshot");
}
void PrepareCaret(Fixture& f) {
  auto& state = Peer::State(*f.service);
  state.committed_pair_range = &f.b.range;
  state.committed_pair_context = &f.a;
  state.committed_pair = L"()";
  f.b.range.text = L"()";
  f.b.range.end_position = 2;
  f.a.range.start_position = f.a.range.end_position = 2;
}
void CaretControl() {
  Fixture f;
  PrepareCaret(f);
  Expect(Peer::Caret(*f.service, &f.a) == S_OK && Peer::State(*f.service).committed_pair_needs_left,
         "unchanged pair and end caret verified");
}
void CaretCallbackReentry(std::string_view call) {
  Fixture f;
  PrepareCaret(f);
  f.Once(call, [&] { f.Switch(); });
  Expect(Peer::Caret(*f.service, &f.a) == S_FALSE, "reentered caret verification rejected");
  Expect(!Peer::State(*f.service).committed_pair_needs_left,
         "reentered caret cannot request synthetic correction");
}
void CaretRangeLifetime() {
  Fixture f;
  PrepareCaret(f);
  f.b.range.Release();
  f.Once("Range.GetText", [&] {
    f.Switch();
    Expect(f.b.range.references > 0, "caret frame retains range during state reset");
  });
  Expect(Peer::Caret(*f.service, &f.a) == S_FALSE, "stale retained caret range rejected");
  Expect(f.b.range.references == 0, "caret range reference released after callback frame");
}
void StartupControl() {
  Fixture f;
  Expect(Peer::Capture(*f.service, &f.a) && Peer::Validate(*f.service, &f.a),
         "startup capture and comparison control");
}
void StartupComparisonReentry(std::string_view call) {
  Fixture f;
  Expect(Peer::Capture(*f.service, &f.a), "startup target captured before comparison");
  f.a.range.Release();
  f.Once(call, [&] {
    f.Switch();
    Expect(f.a.range.references > 0, "startup comparison retains original range during reset");
  });
  Expect(!Peer::Validate(*f.service, &f.a), "reentered startup target comparison rejected");
  Expect(f.a.range.references == 0 && !Peer::State(*f.service).startup_selection,
         "startup range released after frame without stale restore");
}
}  // namespace
int main() {
  unsigned passed = 0, failed = 0;
  const auto run = [&](const char* name, const std::function<void()>& test) {
    try {
      test();
      Expect(modules == 0, "service/DLL references balanced");
      Expect(Range::active_clones == 0, "temporary host ranges released");
      ++passed;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& e) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << e.what() << '\n';
    }
  };
  run("ordinary commit control", CommitControl);
  run("synchronous edit-session control", CommitSessionControl);
  run("privacy query reentry", PrivacyReentry);
  run("queued edit invalidation", QueuedEdit);
  run("selection reentry", SelectionReentry);
  run("selection publish reentry", SelectionPublishReentry);
  run("A-B-A context reuse", ReusedContextEpoch);
  run("SetText reentry", WriteReentry);
  for (const auto name : {"Context.GetActiveView", "View.GetTextExt", "View.GetWnd"})
    run(name, [name] { ViewReentry(name); });
  run("collapse reentry", CollapseReentry);
  run("IPC create-session reentry", CreateSessionReentry);
  run("IPC input-response reentry", InputReplyReentry);
  for (const auto& [command, name] :
       {std::pair{wire::Command::kSetTraditional, "IPC traditional option reentry"},
        std::pair{wire::Command::kSetCandidatePageSize, "IPC page-size option reentry"},
        std::pair{wire::Command::kSetCandidateWindowPageCount, "IPC page-count option reentry"},
        std::pair{wire::Command::kSetChineseCandidatesOnly, "IPC candidate-filter option reentry"}})
    run(name, [command] { SessionOptionReentry(command); });
  for (const auto name : {"Manager.GetFocus", "Document.GetTop"})
    run(name, [name] { FocusLookupReentry(name); });
  run("manager COM lifetime", ManagerLifetime);
  run("edit context COM lifetime", ContextLifetime);
  run("caret read reentry", CaretReentry);
  run("caret verification control", CaretControl);
  for (const auto name :
       {"Range.GetText", "Context.GetSelection", "Range.CompareStart", "Range.CompareEnd"})
    run(name, [name] { CaretCallbackReentry(name); });
  run("caret range COM lifetime", CaretRangeLifetime);
  run("candidate presentation control", CandidateControl);
  for (const auto name : {"UI.Create", "UI.Show"}) run(name, [name] { CandidateReentry(name); });
  run("startup target capture reentry", CaptureReentry);
  run("startup comparison control", StartupControl);
  for (const auto name : {"Range.IsEqualStart", "Range.IsEqualEnd"})
    run(name, [name] { StartupComparisonReentry(name); });
  std::cout << passed << " passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
