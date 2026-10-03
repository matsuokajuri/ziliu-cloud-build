#include "../src/tsf/src/input_privacy.h"
#include "../src/tsf/src/context_metadata_probe.h"
#include "../src/tsf/src/context_fresh_probe.h"
#include "../src/tsf/src/context_prefix_snapshot.h"
#include "../src/core/include/ziliu/core/context_prefix_binding.h"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ziliu::tsf {
namespace { std::atomic<long> test_module_references{0}; }
void AddModuleReference() noexcept { ++test_module_references; }
void ReleaseModuleReference() noexcept { --test_module_references; }
long TestModuleReferenceCount() noexcept { return test_module_references.load(); }
}  // namespace ziliu::tsf

namespace {
DWORD fake_foreground_process_id = 10;
DWORD FakeForegroundProcessId() { return fake_foreground_process_id; }
struct AllowedGate { bool allowed = true; int calls = 0; int deny_on_call = 0; };
bool CheckAllowed(void* value) {
  auto& gate = *static_cast<AllowedGate*>(value);
  ++gate.calls;
  return gate.allowed && (gate.deny_on_call == 0 || gate.calls < gate.deny_on_call);
}
void DenyAfterRead(void* value) { static_cast<AllowedGate*>(value)->allowed = false; }

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

// Stack-owned COM doubles. No TSF activation, registration, windows, broker or
// user dictionary is involved in these contract-level tests.
class Compartment final : public ITfCompartment {
 public:
  VARTYPE type = VT_EMPTY;
  LONG value = 0;
  HRESULT result = S_OK;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfCompartment) return E_NOINTERFACE;
    *out = static_cast<ITfCompartment*>(this);
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 1; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP SetValue(TfClientId, const VARIANT*) override { return E_NOTIMPL; }
  STDMETHODIMP GetValue(VARIANT* out) override {
    out->vt = type;
    out->lVal = value;
    return result;
  }
};

class Range final : public ITfRange {
 public:
  inline static LONG active_clones = 0;
  inline static HRESULT equal_start_result = S_OK;
  inline static HRESULT equal_end_result = S_OK;
  inline static TfAnchor last_equal_start_anchor = TF_ANCHOR_START;
  inline static TfAnchor last_equal_end_anchor = TF_ANCHOR_END;
  bool collapsed = false;
  bool empty = false;
  bool heap_allocated = false;
  TfAnchor anchor = TF_ANCHOR_END;
  HRESULT collapse_result = S_OK;
  HRESULT clone_result = S_OK;
  int collapse_calls = 0;
  int clone_calls = 0;
  int equal_start_calls = 0;
  int equal_end_calls = 0;
  int special_collapse_call = 0;
  HRESULT special_collapse_result = S_OK;
  HRESULT empty_result = S_OK;
  HRESULT shift_result = S_OK;
  HRESULT text_result = S_OK;
  std::wstring text;
  ULONG reported_text_count = 0;
  ULONG requested_text_count = 0;
  DWORD text_flags = 0;
  int get_text_calls = 0;
  LONG start_position = 0;
  LONG end_position = 0;
  bool allow_text_read = false;
  void (*after_get_text)(void*) = nullptr;
  void* after_get_text_context = nullptr;
  LONG shifted_amount = 0;
  LONG reported_shifted = -999;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfRange) return E_NOINTERFACE;
    *out = static_cast<ITfRange*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references;
    if (remaining == 0 && heap_allocated) {
      --active_clones;
      delete this;
    }
    return remaining;
  }
  STDMETHODIMP Collapse(TfEditCookie, TfAnchor selected) override {
    ++collapse_calls;
    anchor = selected;
    const HRESULT current_result = collapse_calls == special_collapse_call
        ? special_collapse_result : collapse_result;
    collapsed = SUCCEEDED(current_result);
    return current_result;
  }
  STDMETHODIMP GetText(TfEditCookie, DWORD flags, WCHAR* buffer, ULONG requested,
                       ULONG* count) override {
    ++get_text_calls;
    Expect(allow_text_read, "field text is read only by an explicitly opted-in synthetic fixture");
    text_flags = flags;
    requested_text_count = requested;
    const ULONG copied = static_cast<ULONG>(std::min<std::size_t>(text.size(), requested));
    if (copied != 0) std::copy_n(text.data(), copied, buffer);
    *count = reported_text_count == 0 ? copied : reported_text_count;
    if (after_get_text != nullptr) after_get_text(after_get_text_context);
    return text_result;
  }
  STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR*, LONG) override { return E_NOTIMPL; }
  STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown**) override { return E_NOTIMPL; }
  STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStart(TfEditCookie, LONG count, LONG* shifted,
                         const TF_HALTCOND*) override {
    shifted_amount = count;
    if (shifted != nullptr) *shifted = reported_shifted == -999 ? count : reported_shifted;
    return shift_result;
  }
  STDMETHODIMP ShiftEnd(TfEditCookie, LONG, LONG*, const TF_HALTCOND*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange*, TfAnchor) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP IsEmpty(TfEditCookie, BOOL* result) override {
    *result = empty ? TRUE : FALSE;
    return empty_result;
  }
  STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange* other, TfAnchor selected_anchor,
                            BOOL* same) override {
    ++equal_start_calls;
    last_equal_start_anchor = selected_anchor;
    if (FAILED(equal_start_result)) return equal_start_result;
    const auto* other_range = static_cast<const Range*>(other);
    *same = start_position == other_range->start_position ? TRUE : FALSE;
    return equal_start_result;
  }
  STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange* other, TfAnchor selected_anchor,
                          BOOL* same) override {
    ++equal_end_calls;
    last_equal_end_anchor = selected_anchor;
    if (FAILED(equal_end_result)) return equal_end_result;
    const auto* other_range = static_cast<const Range*>(other);
    *same = end_position == other_range->end_position ? TRUE : FALSE;
    return equal_end_result;
  }
  STDMETHODIMP CompareStart(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP CompareEnd(TfEditCookie, ITfRange*, TfAnchor, LONG*) override { return E_NOTIMPL; }
  STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetGravity(TfGravity*, TfGravity*) override { return E_NOTIMPL; }
  STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override { return E_NOTIMPL; }
  STDMETHODIMP Clone(ITfRange** out) override {
    ++clone_calls;
    *out = nullptr;
    if (clone_result != S_OK) return clone_result;
    auto* copy = new (std::nothrow) Range();
    if (copy == nullptr) return E_OUTOFMEMORY;
    copy->heap_allocated = true;
    copy->start_position = start_position;
    copy->end_position = end_position;
    ++active_clones;
    *out = copy;
    return S_OK;
  }
  STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }
};

class Property final : public ITfReadOnlyProperty {
 public:
  IUnknown* scope = nullptr;
  HRESULT result = S_OK;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_ITfReadOnlyProperty) return E_NOINTERFACE;
    *out = static_cast<ITfReadOnlyProperty*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override { return --references; }
  STDMETHODIMP GetType(GUID*) override { return E_NOTIMPL; }
  STDMETHODIMP EnumRanges(TfEditCookie, IEnumTfRanges**, ITfRange*) override { return E_NOTIMPL; }
  STDMETHODIMP GetValue(TfEditCookie, ITfRange* range, VARIANT* value) override {
    if (!static_cast<Range*>(range)->collapsed) {
      value->vt = VT_EMPTY;
      return S_FALSE;  // Simulate a selection containing multiple scope values.
    }
    value->vt = VT_UNKNOWN;
    value->punkVal = scope;
    if (scope != nullptr) scope->AddRef();
    return result;
  }
  STDMETHODIMP GetContext(ITfContext**) override { return E_NOTIMPL; }
};

class Context final : public ITfContext, public ITfCompartmentMgr {
 public:
  Compartment disabled;
  Compartment empty;
  HRESULT property_result = S_FALSE;
  bool compartment_failure = false;
  bool property_present = false;
  bool selection_failure = false;
  HRESULT selection_result = S_OK;
  ULONG selection_fetched = 1;
  int selection_calls = 0;
  int special_selection_call = 0;
  HRESULT special_selection_result = S_OK;
  TfActiveSelEnd active_end = TF_AE_END;
  BOOL interim_character = FALSE;
  Range range;
  Property property;
  int nested_edit_requests = 0;
  HRESULT request_result = S_OK;
  bool defer_edit_session = false;
  bool foreign_thread_edit_session = false;
  bool repeat_edit_session = false;
  HRESULT secondary_session_result = E_PENDING;
  ITfEditSession* deferred_session = nullptr;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_ITfContext) *out = static_cast<ITfContext*>(this);
    if (id == IID_ITfCompartmentMgr) *out = static_cast<ITfCompartmentMgr*>(this);
    if (*out == nullptr) return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override { return --references; }
  STDMETHODIMP GetCompartment(REFGUID id, ITfCompartment** out) override {
    *out = nullptr;
    if (compartment_failure) return E_FAIL;
    if (id == GUID_COMPARTMENT_KEYBOARD_DISABLED) *out = &disabled;
    if (id == GUID_COMPARTMENT_EMPTYCONTEXT) *out = &empty;
    return *out != nullptr ? S_OK : E_INVALIDARG;
  }
  STDMETHODIMP ClearCompartment(TfClientId, REFGUID) override { return E_NOTIMPL; }
  STDMETHODIMP EnumCompartments(IEnumGUID**) override { return E_NOTIMPL; }
  STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession* session, DWORD flags,
                                  HRESULT* result) override {
    ++nested_edit_requests;
    Expect(flags == (TF_ES_SYNC | TF_ES_READ), "scope read requests a synchronous read lock");
    if (request_result != S_OK) {
      *result = E_UNEXPECTED;
      return request_result;
    }
    if (defer_edit_session) {
      deferred_session = session;
      deferred_session->AddRef();
      *result = E_PENDING;
      return S_OK;
    }
    if (foreign_thread_edit_session) {
      std::thread worker([&] { *result = session->DoEditSession(1); });
      worker.join();
      return S_OK;
    }
    *result = session->DoEditSession(1);
    if (repeat_edit_session) {
      secondary_session_result = session->DoEditSession(1);
      *result = secondary_session_result;
    }
    return S_OK;
  }
  STDMETHODIMP GetAppProperty(REFGUID id, ITfReadOnlyProperty** out) override {
    Expect(id == GUID_PROP_INPUTSCOPE, "only read input-scope metadata");
    *out = nullptr;
    if (property_present) { *out = &property; property.AddRef(); }
    return property_result;
  }
  STDMETHODIMP InWriteSession(TfClientId, BOOL*) override { return E_NOTIMPL; }
  STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION* selection, ULONG* count) override {
    *count = 0;
    ++selection_calls;
    if (selection_failure) return E_FAIL;
    const HRESULT current_result = selection_calls == special_selection_call
        ? special_selection_result : selection_result;
    if (FAILED(current_result)) return current_result;
    selection->range = &range;
    selection->style.ase = active_end;
    selection->style.fInterimChar = interim_character;
    range.AddRef();
    *count = selection_fetched;
    return current_result;
  }
  STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return E_NOTIMPL; }
  STDMETHODIMP GetStart(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetEnd(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
  STDMETHODIMP GetActiveView(ITfContextView**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
  STDMETHODIMP GetStatus(TF_STATUS*) override { return E_NOTIMPL; }
  STDMETHODIMP GetProperty(REFGUID, ITfProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG,
                               ITfReadOnlyProperty**) override { return E_NOTIMPL; }
  STDMETHODIMP EnumProperties(IEnumTfProperties**) override { return E_NOTIMPL; }
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
  STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup**) override { return E_NOTIMPL; }
};

class Scope final : public ITfInputScope {
 public:
  std::vector<InputScope> values;
  HRESULT result = S_OK;
  bool malformed = false;
  ULONG references = 1;
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != __uuidof(ITfInputScope)) return E_NOINTERFACE;
    *out = static_cast<ITfInputScope*>(this);
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
  STDMETHODIMP_(ULONG) Release() override { return --references; }
  STDMETHODIMP GetInputScopes(InputScope** scopes, UINT* count) override {
    *scopes = nullptr;
    *count = static_cast<UINT>(values.size());
    if (!values.empty() && !malformed) {
      *scopes = static_cast<InputScope*>(CoTaskMemAlloc(values.size() * sizeof(InputScope)));
      if (*scopes == nullptr) return E_OUTOFMEMORY;
      for (std::size_t i = 0; i < values.size(); ++i) (*scopes)[i] = values[i];
    }
    return result;
  }
  STDMETHODIMP GetPhrase(BSTR**, UINT*) override { return E_NOTIMPL; }
  STDMETHODIMP GetRegularExpression(BSTR*) override { return E_NOTIMPL; }
  STDMETHODIMP GetSRGS(BSTR*) override { return E_NOTIMPL; }
  STDMETHODIMP GetXML(BSTR*) override { return E_NOTIMPL; }
};
}

int main() {
  using namespace ziliu::tsf::detail;
  const auto edge_target = MatchContextProbeTarget(
      L"msedge.exe", L"Ziliu CONTEXT METADATA [ordinary] - Microsoft Edge");
  Expect(edge_target && edge_target->host == ContextProbeHost::kEdge && edge_target->field == 0,
         "Edge ordinary fixture maps to host and field ordinals");
  const auto edge_prefix_a = MatchContextProbeTarget(
      L"msedge.exe", L"Ziliu SYNTHETIC PREFIX [ordinary] - Microsoft Edge");
  const auto edge_prefix_b = MatchContextProbeTarget(
      L"msedge.exe", L"Ziliu SYNTHETIC PREFIX [ordinarySecond]");
  Expect(edge_prefix_a && edge_prefix_a->host == ContextProbeHost::kEdge &&
         edge_prefix_a->field == 0 && edge_prefix_b &&
         edge_prefix_b->host == ContextProbeHost::kEdge && edge_prefix_b->field == 1,
         "Edge synthetic-prefix fixtures map to the ordinary field ordinals");
  Expect(!MatchContextProbeTarget(
             L"chrome.exe", L"Ziliu SYNTHETIC PREFIX [ordinary]") &&
         !MatchContextProbeTarget(
             L"msedge.exe", L"Ziliu SYNTHETIC PREFIX [ordinary]forged"),
         "Edge synthetic-prefix target titles are host-specific and reject forged suffixes");
  Expect(IsEdgeSyntheticPrefixFixture(
             ContextProbeHost::kEdge, 0, L"Ziliu SYNTHETIC PREFIX [ordinary] - Microsoft Edge") &&
         IsEdgeSyntheticPrefixFixture(
             ContextProbeHost::kEdge, 1, L"Ziliu SYNTHETIC PREFIX [ordinarySecond] window"),
         "Edge synthetic-prefix allowlist accepts exact fixture prefixes and window suffixes");
  Expect(!IsEdgeSyntheticPrefixFixture(
             ContextProbeHost::kChrome, 0, L"Ziliu SYNTHETIC PREFIX [ordinary]") &&
         !IsEdgeSyntheticPrefixFixture(
             ContextProbeHost::kEdge, 2, L"Ziliu SYNTHETIC PREFIX [ordinary]") &&
         !IsEdgeSyntheticPrefixFixture(
             ContextProbeHost::kEdge, 0, L"Ziliu SYNTHETIC PREFIX [ordinarySecond]") &&
         !IsEdgeSyntheticPrefixFixture(
             ContextProbeHost::kEdge, 0, L"Ziliu SYNTHETIC PREFIX [ordinary]forged"),
         "Edge synthetic-prefix allowlist rejects wrong host/field, mismatches, and forged suffixes");
  const auto chrome_target = MatchContextProbeTarget(
      L"CHROME.EXE", L"Ziliu CONTEXT METADATA [ordinarySecond]");
  Expect(chrome_target && chrome_target->host == ContextProbeHost::kChrome &&
         chrome_target->field == 1, "Chrome second fixture maps case-insensitively");
  const auto chrome_password = MatchContextProbeTarget(
      L"chrome.exe", L"Ziliu CONTEXT METADATA [password]");
  const auto chrome_pin = MatchContextProbeTarget(
      L"chrome.exe", L"Ziliu CONTEXT METADATA [pinLike]");
  Expect(chrome_password && chrome_password->field == 2 && chrome_pin &&
         chrome_pin->field == 3, "Chrome secret fixture titles keep fixed field ordinals");
  const auto notepad_target = MatchContextProbeTarget(
      L"notepad.exe", L"*ZiliuContextProbe-A.txt - Notepad");
  Expect(notepad_target && notepad_target->host == ContextProbeHost::kNotepad &&
         notepad_target->field == 0, "Notepad matches only a fixed filename at title start");
  const auto code_target = MatchContextProbeTarget(
      L"Code.exe", L"• ZiliuContextProbe-B.txt - workspace - Visual Studio Code");
  Expect(code_target && code_target->host == ContextProbeHost::kCode &&
         code_target->field == 1, "Code matches fixed filename with its explicit title marker");
  Expect(!MatchContextProbeTarget(L"notepad.exe", L"notes ZiliuContextProbe-A.txt") &&
         !MatchContextProbeTarget(L"notepad.exe", L"ZiliuContextProbe-C.txt - Notepad") &&
         !MatchContextProbeTarget(L"code.exe", L"ZiliuContextProbe-A.txtx - Code") &&
         !MatchContextProbeTarget(L"other.exe", L"Ziliu CONTEXT METADATA [ordinary]") &&
         !MatchContextProbeTarget(L"chrome.exe", L"Unrelated browser title"),
         "target matcher rejects wrong process, title, filename, and non-prefix matches");
  Context context;
  const auto privacy = [&context](DWORD flags = 0) {
    return ClassifyInputContext(&context, 7, flags);
  };
  Expect(ClassifyInputContext(nullptr, 7, 0) == InputPrivacy::kBlocked,
         "null context bypasses input method");
  Expect(privacy(TF_TMAE_SECUREMODE) == InputPrivacy::kBlocked,
         "secure activation bypasses engine");
  Expect(context.nested_edit_requests == 0,
          "blocked contexts never request a scope read session");
  Expect(privacy() == InputPrivacy::kRestricted,
         "absent optional scope permits restricted local composition");
  context.disabled.type = VT_I4;
  context.disabled.value = 1;
  Expect(privacy() == InputPrivacy::kBlocked, "disabled context blocks test routing");
  Expect(context.nested_edit_requests == 1,
          "disabled context does not request an edit session");
  context.disabled.value = 0;
  context.empty.type = VT_I4;
  context.empty.value = 1;
  Expect(privacy() == InputPrivacy::kBlocked, "empty context blocks test routing");
  context.empty.value = 0;
  Expect(privacy() == InputPrivacy::kRestricted,
         "return to an unknown-scope context re-enables restricted routing");
  context.disabled.result = E_FAIL;
  Expect(privacy() == InputPrivacy::kBlocked, "failed compartment read is not permission");
  context.disabled.result = S_OK;
  context.disabled.type = VT_UI4;
  Expect(privacy() == InputPrivacy::kBlocked, "unexpected compartment type is rejected");
  context.disabled.type = VT_EMPTY;
  context.compartment_failure = true;
  Expect(privacy() == InputPrivacy::kBlocked, "missing compartment interface fails closed");
  context.compartment_failure = false;
  context.property_result = E_NOTIMPL;
  Expect(privacy() == InputPrivacy::kRestricted,
         "legacy host without input scope composes without learning");
  for (HRESULT failure : {E_FAIL, TF_E_DISCONNECTED, E_OUTOFMEMORY, S_OK}) {
    context.property_result = failure;
    Expect(privacy() == InputPrivacy::kRestricted,
           "failed or absent optional scope remains restricted rather than blocked");
  }

  VARIANT value;
  VariantInit(&value);
  Expect(ClassifyScopeValue(value) == InputPrivacy::kRestricted,
         "unset input scope is restricted rather than assumed safe");
  value.vt = VT_I4;
  Expect(ClassifyScopeValue(value) == InputPrivacy::kRestricted,
         "invalid scope property type is restricted");
  value.vt = VT_UNKNOWN;
  value.punkVal = nullptr;
  Expect(ClassifyScopeValue(value) == InputPrivacy::kRestricted,
         "null scope object is restricted");
  Scope scope;
  value.punkVal = &scope;
  for (InputScope ordinary : {IS_DEFAULT, IS_TEXT, IS_SEARCH, IS_CHAT, IS_NUMBER}) {
    scope.values = {ordinary};
    Expect(ClassifyScopeValue(value) == InputPrivacy::kOrdinary,
           "ordinary scopes retain learning-capable input-method support");
  }
  scope.values = {static_cast<InputScope>(9999)};
  Expect(ClassifyScopeValue(value) == InputPrivacy::kRestricted,
         "unknown numeric scope is not assumed ordinary");
  scope.values = {IS_DEFAULT, IS_PRIVATE, IS_TEXT};
  Expect(ClassifyScopeValue(value) == InputPrivacy::kRestricted,
         "private scope permits only restricted local composition");
  for (InputScope sensitive : {IS_PASSWORD, IS_NUMERIC_PASSWORD, IS_NUMERIC_PIN,
                              IS_ALPHANUMERIC_PIN, IS_ALPHANUMERIC_PIN_SET}) {
    scope.values = {IS_DEFAULT, sensitive, IS_TEXT};
    Expect(ClassifyScopeValue(value) == InputPrivacy::kBlocked,
           "any password or PIN scope blocks the entire key");
  }
  scope.values = {IS_TEXT};
  scope.result = E_FAIL;
  Expect(ClassifyScopeValue(value) == InputPrivacy::kRestricted,
         "scope enumeration failure stays restricted");
  scope.result = S_OK;
  scope.malformed = true;
  Expect(ClassifyScopeValue(value) == InputPrivacy::kRestricted,
         "null array with nonzero count stays restricted");

  scope.malformed = false;
  context.property_result = S_OK;
  context.property_present = true;
  context.property.scope = &scope;
  for (TfActiveSelEnd end : {TF_AE_START, TF_AE_END}) {
    context.active_end = end;
    context.range.collapsed = false;
    scope.values = {IS_PRIVATE};
    Expect(privacy() == InputPrivacy::kRestricted,
           "mixed selection checks private insertion-end scope in the actual edit");
    Expect(context.range.collapsed, "range is collapsed before scope GetValue");
    Expect(context.range.anchor == (end == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END),
           "query the correct active end of forward/backward selection");
    scope.values = {IS_TEXT};
    Expect(privacy() == InputPrivacy::kOrdinary,
            "real property/range path accepts ordinary text");
  }
  context.range.collapse_result = E_FAIL;
  Expect(privacy() == InputPrivacy::kBlocked, "failed range collapse rejects key");
  context.range.collapse_result = S_OK;
  context.interim_character = TRUE;
  Expect(privacy() == InputPrivacy::kBlocked,
         "interim-character range has no safe insertion point");
  context.interim_character = FALSE;
  context.active_end = TF_AE_NONE;
  Expect(privacy() == InputPrivacy::kBlocked,
         "no active selection end rejects a nonempty selection");
  context.range.empty = true;
  context.range.collapsed = false;
  scope.values = {IS_SEARCH};
  Expect(privacy() == InputPrivacy::kOrdinary,
         "collapsed search caret without active end reads its ordinary scope");
  Expect(context.range.collapsed && context.range.anchor == TF_ANCHOR_END,
         "collapsed selection queries the caret location");
  scope.values = {IS_NUMERIC_PIN};
  Expect(privacy() == InputPrivacy::kBlocked,
         "collapsed no-active-end selection never bypasses a PIN scope");
  scope.values = {IS_PRIVATE};
  Expect(privacy() == InputPrivacy::kRestricted,
         "collapsed no-active-end selection retains private restrictions");
  context.range.empty_result = E_FAIL;
  Expect(privacy() == InputPrivacy::kBlocked,
         "unverifiable no-active-end selection fails closed");
  context.range.empty_result = S_OK;
  context.range.empty = false;
  context.active_end = static_cast<TfActiveSelEnd>(99);
  Expect(privacy() == InputPrivacy::kBlocked, "unknown active selection end rejects key");
  context.active_end = TF_AE_END;
  context.selection_failure = true;
  Expect(privacy() == InputPrivacy::kBlocked, "unavailable selection rejects key");
  context.selection_failure = false;
  context.property.result = E_FAIL;
  Expect(privacy() == InputPrivacy::kRestricted,
         "GetValue failure permits only restricted composition and releases its variant");
  Expect(ShouldResetInputSession(InputPrivacy::kRestricted, InputPrivacy::kOrdinary, false),
         "private-to-normal transition resets the prior composition");
  Expect(ShouldResetInputSession(InputPrivacy::kOrdinary, InputPrivacy::kRestricted, false),
         "normal-to-unknown transition resets the prior composition");
  Expect(ShouldResetInputSession(InputPrivacy::kRestricted, InputPrivacy::kRestricted, true),
         "focus context change resets a restricted composition");
  Expect(!ShouldResetInputSession(InputPrivacy::kRestricted, InputPrivacy::kRestricted, false),
         "stable restricted context keeps its current composition");
  Expect(context.range.references == 1 && context.property.references == 1 && scope.references == 1,
         "range, property and input scope references are balanced across allowed/denied/error paths");
  context.property.result = S_OK;
  scope.values = {IS_TEXT};
  auto metadata = ReadScopeMetadata(&context, 1);
  Expect(metadata.value == S_OK && metadata.enumeration == S_OK &&
         metadata.scope_count == 1 && metadata.scopes[0] == IS_TEXT,
         "diagnostic records scope metadata without retrieving any text");
  context.property.result = E_FAIL;
  metadata = ReadScopeMetadata(&context, 1);
  Expect(metadata.value == E_FAIL && metadata.enumeration == E_PENDING,
         "diagnostic preserves a failed property read rather than inventing scope");
  context.selection_failure = true;
  metadata = ReadScopeMetadata(&context, 1);
  Expect(metadata.selection == E_FAIL && metadata.property == E_PENDING,
         "diagnostic stops on failed selection");
  Expect(context.range.references == 1 && context.property.references == 1 && scope.references == 1,
         "diagnostic releases metadata COM objects on every path");

  // The only text-read tests are explicit fake-fixture opt-ins. A matching line
  // demonstrates this synthetic oracle only; it is not general field identity.
  context.property.scope = &scope;
  context.property.result = S_OK;
  context.property_present = true;
  context.selection_failure = false;
  scope.result = S_OK;
  scope.values = {IS_TEXT};
  context.range.allow_text_read = true;
  constexpr std::wstring_view fixture_a =
      L"Ziliu synthetic context probe A. No personal information.";
  constexpr std::wstring_view fixture_b =
      L"Ziliu synthetic context probe B. No personal information.";
  context.range.text = fixture_a;
  auto prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                           FakeForegroundProcessId);
  Expect(prefix.attempted && prefix.body_read && prefix.read_hr == S_OK &&
         prefix.count == fixture_a.size() && prefix.match_a && !prefix.match_b,
         "explicit synthetic fixture reads and matches the exact A line");
  Expect(context.range.anchor == TF_ANCHOR_START && context.range.shifted_amount == -128 &&
         context.range.requested_text_count == 128 && context.range.text_flags == 0,
         "prefix probe collapses to START and bounds its read to 128 UTF-16 units");
  context.range.text = fixture_b;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kNotepad, 1, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.body_read && prefix.read_hr == S_OK && prefix.match_b && !prefix.match_a,
         "explicit synthetic fixture reads and matches the exact B line");
  context.range.text = L"wrongprefix Ziliu synthetic context probe A. No personal information.";
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.body_read && !prefix.match_a && !prefix.match_b,
         "a different current prefix does not match either expected fixture line");

  const int reads_before_denials = context.range.get_text_calls;
  context.range.allow_text_read = false;
  for (InputScope denied_scope : {static_cast<InputScope>(9999), IS_PRIVATE,
                                  IS_NUMERIC_PIN, IS_PASSWORD}) {
    scope.values = {denied_scope};
    prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                        FakeForegroundProcessId);
    Expect(!prefix.attempted && !prefix.body_read &&
           context.range.get_text_calls == reads_before_denials,
           "unknown, private, PIN, and password scopes cannot call GetText");
  }
  scope.values = {IS_TEXT};
  context.property.result = E_FAIL;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(!prefix.attempted && !prefix.body_read &&
         context.range.get_text_calls == reads_before_denials,
         "failed scope-property query cannot call GetText");
  context.property.result = S_FALSE;
  scope.values = {IS_TEXT};
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(!prefix.attempted && !prefix.body_read &&
         context.range.get_text_calls == reads_before_denials,
         "S_FALSE with a populated VT_UNKNOWN value is not strict permission to read");
  context.property.result = S_OK;
  scope.result = S_FALSE;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(!prefix.attempted && !prefix.body_read &&
         context.range.get_text_calls == reads_before_denials,
         "S_FALSE input-scope enumeration cannot authorize GetText");
  scope.result = S_OK;
  scope.malformed = true;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(!prefix.attempted && !prefix.body_read &&
         context.range.get_text_calls == reads_before_denials,
         "S_OK enumeration without an allocated scope array cannot authorize GetText");
  scope.malformed = false;
  context.disabled.type = VT_I4;
  context.disabled.value = 1;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  context.disabled.value = 0;
  Expect(!prefix.attempted && !prefix.body_read &&
         context.range.get_text_calls == reads_before_denials,
         "blocked context compartment cannot call GetText");
  fake_foreground_process_id = 11;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  fake_foreground_process_id = 10;
  Expect(!prefix.attempted && context.range.get_text_calls == reads_before_denials,
         "non-foreground process cannot call GetText");
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kEdge, 0, 10,
                                      FakeForegroundProcessId);
  Expect(!prefix.attempted && context.range.get_text_calls == reads_before_denials,
         "browser targets are explicitly excluded from prefix reads");

  context.range.allow_text_read = true;
  context.range.text = fixture_a;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kEdge, 0, 10,
                                      FakeForegroundProcessId, true);
  Expect(prefix.attempted && prefix.body_read && prefix.read_hr == S_OK &&
         prefix.count == fixture_a.size() && prefix.match_a && !prefix.match_b,
         "explicit Edge fixture opt-in reads and matches the exact A line");
  context.range.text = fixture_b;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kEdge, 1, 10,
                                      FakeForegroundProcessId, true);
  Expect(prefix.attempted && prefix.body_read && prefix.read_hr == S_OK &&
         prefix.count == fixture_b.size() && prefix.match_b && !prefix.match_a,
         "explicit Edge second fixture opt-in reads and matches the exact B line");
  context.range.text.clear();
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kEdge, 0, 10,
                                      FakeForegroundProcessId, true);
  Expect(prefix.attempted && prefix.body_read && prefix.read_hr == S_OK &&
         prefix.count == 0 && !prefix.match_a && !prefix.match_b,
         "explicit Edge fixture opt-in reports an empty prefix without a fixture match");

  const int edge_reads_before_denials = context.range.get_text_calls;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kChrome, 0, 10,
                                      FakeForegroundProcessId, true);
  Expect(!prefix.attempted && context.range.get_text_calls == edge_reads_before_denials,
         "Chrome remains denied even when the Edge fixture opt-in is set");
  for (int denied_field : {2, 3}) {
    prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kEdge, denied_field, 10,
                                        FakeForegroundProcessId, true);
    Expect(!prefix.attempted && context.range.get_text_calls == edge_reads_before_denials,
           "Edge secret fixture fields remain denied with opt-in");
  }
  for (InputScope denied_scope : {static_cast<InputScope>(9999), IS_PRIVATE,
                                  IS_NUMERIC_PIN, IS_PASSWORD}) {
    scope.values = {denied_scope};
    prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kEdge, 0, 10,
                                        FakeForegroundProcessId, true);
    Expect(!prefix.attempted && context.range.get_text_calls == edge_reads_before_denials,
           "unknown, private, PIN, and password scopes remain denied for opted-in Edge");
  }
  scope.values = {IS_TEXT};
  context.range.text = fixture_a;

  context.range.allow_text_read = true;
  context.range.text = fixture_a;
  context.range.text_result = S_FALSE;
  context.range.reported_text_count = 7;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.body_read && prefix.read_hr == S_FALSE && prefix.count == 7 &&
         !prefix.match_a && !prefix.match_b,
         "S_FALSE partial reads preserve HRESULT/count and are not empty or matches");
  context.range.text_result = E_FAIL;
  context.range.reported_text_count = 3;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.body_read && prefix.read_hr == E_FAIL && prefix.count == 3 &&
         !prefix.match_a && !prefix.match_b,
         "failed reads preserve HRESULT/count and never become matches");
  context.range.text_result = S_OK;
  context.range.reported_text_count = 129;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.body_read && prefix.count == 129 && !prefix.match_a && !prefix.match_b,
         "an impossible over-bound fetched count is rejected");
  context.range.reported_text_count = 0;

  context.selection_calls = 0;
  context.special_selection_call = 3;
  context.special_selection_result = S_FALSE;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.selection_hr == S_FALSE && !prefix.body_read,
         "non-S_OK selection result is preserved and never treated as an empty read");
  context.special_selection_call = 0;
  context.selection_fetched = 2;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(!prefix.body_read, "ambiguous selection count cannot call GetText");
  context.selection_fetched = 1;
  context.range.collapse_calls = 0;
  context.range.special_collapse_call = 3;
  context.range.special_collapse_result = E_FAIL;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.collapse_hr == E_FAIL && !prefix.body_read,
         "failed START collapse is preserved and cannot call GetText");
  context.range.special_collapse_call = 0;
  context.range.shift_result = E_FAIL;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.shift_hr == E_FAIL && !prefix.body_read,
         "failed bounded shift is preserved and cannot call GetText");
  context.range.shift_result = S_OK;
  context.range.reported_shifted = -129;
  prefix = ReadSyntheticContextPrefix(&context, 1, ContextProbeHost::kCode, 0, 10,
                                      FakeForegroundProcessId);
  Expect(prefix.shift_hr == S_OK && !prefix.body_read,
         "out-of-range reported shift cannot authorize GetText");
  context.range.reported_shifted = -999;
  context.range.allow_text_read = false;
  Expect(context.range.references == 1 && context.property.references == 1 &&
         scope.references == 1,
         "prefix probe balances range, property, and input-scope COM references");

  // Fresh Edge fixture reads have their own synchronous lock and a caller-owned
  // freshness gate. The gate is never retained after RequestEditSession returns.
  context.range.allow_text_read = true;
  context.range.text_result = S_OK;
  context.range.reported_text_count = 0;
  context.range.reported_shifted = -57;
  context.range.empty = true;
  context.range.text = fixture_a;
  AllowedGate gate;
  auto fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.request_hr == S_OK && fresh.session_hr == S_OK &&
         fresh.selection_hr == S_OK && fresh.read_hr == S_OK &&
         fresh.count == 57 && fresh.body_read && fresh.fresh_read_ok && gate.calls >= 5,
         "fresh Edge fixture prefix validates the exact complete A text under a read lock");
  Expect(context.range.shifted_amount == -128 && context.range.requested_text_count == 128 &&
         context.range.text_flags == 0,
         "fresh-prefix probe bounds its shift and text read without retaining text");

  context.range.text = L"synthetic mismatch";
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.body_read && fresh.read_hr == S_OK && !fresh.fresh_read_ok,
         "a complete but mismatching current prefix is not accepted");
  context.range.text = fixture_b;
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.body_read && fresh.count == 57 && !fresh.fresh_read_ok,
         "same-length different content cannot pass a fixed-prefix read");
  context.range.text.clear();
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.body_read && fresh.read_hr == S_OK && fresh.count == 0 &&
         !fresh.fresh_read_ok,
         "an empty fixture prefix is a completed non-match, not a fresh read");
  context.range.text = fixture_a;

  const int fresh_reads_before_denials = context.range.get_text_calls;
  context.active_end = TF_AE_NONE;
  context.range.empty = false;
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(!fresh.body_read && context.range.get_text_calls == fresh_reads_before_denials,
         "nonempty selection cannot reach GetText");
  for (TfActiveSelEnd end : {TF_AE_START, TF_AE_END}) {
    context.active_end = end;
    gate = {};
    fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
    Expect(!fresh.body_read && context.range.get_text_calls == fresh_reads_before_denials,
           "nonempty selection is denied for either active-end direction");
  }
  context.range.empty = true;
  context.active_end = TF_AE_END;
  context.interim_character = TRUE;
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  context.interim_character = FALSE;
  Expect(!fresh.body_read && context.range.get_text_calls == fresh_reads_before_denials,
         "interim-character selection cannot reach GetText");

  for (InputScope denied_scope : {static_cast<InputScope>(9999), IS_PRIVATE,
                                  IS_NUMERIC_PIN, IS_PASSWORD}) {
    scope.values = {denied_scope};
    gate = {};
    fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
    Expect(!fresh.body_read && context.range.get_text_calls == fresh_reads_before_denials,
           "unknown, private, PIN, and password scopes are denied before GetText");
  }
  scope.values = {IS_TEXT};

  gate.allowed = false;
  const int gate_calls_before = gate.calls;
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(!fresh.body_read && gate.calls == gate_calls_before + 1 &&
         context.range.get_text_calls == fresh_reads_before_denials,
         "a denied freshness gate blocks the edit-session request and text access");
  for (int boundary : {2, 3, 4, 5}) {
    gate = {};
    gate.deny_on_call = boundary;
    const int before_read = context.range.get_text_calls;
    fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
    Expect(!fresh.fresh_read_ok && fresh.body_read == (boundary >= 4) &&
           context.range.get_text_calls == before_read + (boundary >= 4 ? 1 : 0),
           "entry, pre-read, post-read and post-session denials each fail closed");
  }
  for (LONG shifted : {0L, -56L, -58L, -129L}) {
    gate = {};
    context.range.reported_shifted = shifted;
    const int before_read = context.range.get_text_calls;
    fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
    Expect(!fresh.body_read && !fresh.fresh_read_ok &&
           context.range.get_text_calls == before_read,
           "empty, truncated and unexpected prefix extents deny before GetText");
  }
  context.range.reported_shifted = -57;
  gate = {};
  context.range.after_get_text = DenyAfterRead;
  context.range.after_get_text_context = &gate;
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  context.range.after_get_text = nullptr;
  context.range.after_get_text_context = nullptr;
  Expect(fresh.body_read && !fresh.fresh_read_ok,
         "freshness is rechecked after GetText and invalidates a changed-generation read");

  context.range.text_result = S_FALSE;
  context.range.reported_text_count = 7;
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.body_read && fresh.read_hr == S_FALSE && fresh.count == 7 &&
         !fresh.fresh_read_ok,
         "partial S_FALSE read preserves its count but can never pass freshness");
  context.range.text_result = S_OK;
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.body_read && fresh.read_hr == S_OK && fresh.count == 7 &&
         !fresh.fresh_read_ok,
         "partial S_OK count cannot be mistaken for a complete fixture prefix");
  context.range.text_result = E_FAIL;
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.body_read && fresh.read_hr == E_FAIL && !fresh.fresh_read_ok,
         "failed GetText preserves failure and cannot pass freshness");
  context.range.text_result = S_OK;
  context.range.reported_text_count = 0;

  context.selection_calls = 0;
  context.special_selection_call = 3;
  context.special_selection_result = S_FALSE;
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  context.special_selection_call = 0;
  Expect(fresh.selection_hr == S_FALSE && !fresh.body_read,
         "non-S_OK selection is rejected before text access");
  context.selection_calls = 0;
  context.special_selection_call = 3;
  context.special_selection_result = E_FAIL;
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  context.special_selection_call = 0;
  Expect(fresh.request_hr == S_OK && fresh.session_hr == E_FAIL && !fresh.body_read,
         "failed DoEditSession is distinguished from a successful synchronous request");
  context.request_result = E_FAIL;
  gate = {};
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  context.request_result = S_OK;
  Expect(fresh.request_hr == E_FAIL && !fresh.fresh_read_ok,
         "failed edit-session request cannot report a fresh read");

  context.foreign_thread_edit_session = true;
  gate = {};
  const int reads_before_foreign_thread = context.range.get_text_calls;
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  context.foreign_thread_edit_session = false;
  Expect(fresh.request_hr == S_OK && fresh.session_hr == E_ACCESSDENIED &&
         !fresh.body_read && context.range.get_text_calls == reads_before_foreign_thread,
         "foreign-apartment session rejects before callback or result access");

  context.repeat_edit_session = true;
  gate = {};
  const int reads_before_repeat = context.range.get_text_calls;
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  context.repeat_edit_session = false;
  Expect(context.secondary_session_result == E_ABORT &&
         context.range.get_text_calls == reads_before_repeat + 1 && !fresh.fresh_read_ok,
         "one-shot session guard rejects a repeated DoEditSession without a second text read");

  context.defer_edit_session = true;
  gate = {};
  const ULONG context_refs_before = context.references;
  const long module_refs_before = ziliu::tsf::TestModuleReferenceCount();
  fresh = ReadFreshFixturePrefix(&context, 7, 0, CheckAllowed, &gate);
  Expect(fresh.request_hr == S_OK && fresh.session_hr == E_PENDING &&
         context.deferred_session != nullptr && context.references == context_refs_before + 1 &&
         ziliu::tsf::TestModuleReferenceCount() == module_refs_before + 1,
         "unexpected deferred session keeps only its own context reference and deactivates caller state");
  Expect(context.deferred_session->DoEditSession(1) == E_ABORT,
         "late deferred DoEditSession rejects without touching the returned stack state");
  context.deferred_session->Release();
  context.deferred_session = nullptr;
  Expect(context.references == context_refs_before,
         "late-session release balances its retained context reference");
  Expect(ziliu::tsf::TestModuleReferenceCount() == module_refs_before,
         "deferred-session destruction releases its module lifetime reference");
  context.defer_edit_session = false;
  Expect(context.range.get_text_calls >= fresh_reads_before_denials,
         "fresh probe fake fixture remains intact after denied and deferred paths");

  // This reader and the metadata below are synthetic fake-COM test inputs. The
  // fabricated identity and absolute offset exercise interoperability only; the
  // reader itself does not produce either value or establish production identity.
  using PrefixSnapshot = ContextPrefixSnapshot;
  using PrefixBinding = ziliu::core::ContextPrefixBinding;
  using RequestSnapshot = ziliu::core::ContextRequestSnapshot;
  using LifetimeToken = ziliu::core::ContextLifetimeToken;
  const auto read_current_prefix = [&](PrefixSnapshot& output, AllowedGate& read_gate,
                                       const PrefixSnapshot* previous = nullptr) {
    return ReadContextPrefixSnapshot(&context, 7, 0, CheckAllowed, &read_gate, output,
                                     previous);
  };
  const auto to_utf16 = [](std::wstring_view value) {
    std::u16string converted;
    converted.reserve(value.size());
    for (const wchar_t unit : value) converted.push_back(static_cast<char16_t>(unit));
    return converted;
  };
  const auto declared_test_snapshot = [](bool fresh_ok = true) {
    return RequestSnapshot{
        .field_token = LifetimeToken{0xA11CE, 0xB0B},
        .field_epoch = 1,
        .edit_revision = 2,
        .selection_start_utf16 = 256,  // Synthetic absolute offset, not reader output.
        .selection_end_utf16 = 256,
        .input_revision = 3,
        .candidate_revision = 4,
        .broker_instance = {5, 1},
        .engine_session_id = 1,
        .dictionary_epoch = 6,
        .privacy = ziliu::core::ContextPrivacy::ordinary,
        .identity_verified = true,  // Explicitly fabricated only for this core test.
        .fresh_read_ok = fresh_ok};
  };

  PrefixSnapshot prefix_snapshot;
  AllowedGate snapshot_gate;
  context.range.allow_text_read = true;
  context.range.empty = true;
  context.active_end = TF_AE_END;
  context.interim_character = FALSE;
  context.selection_result = S_OK;
  context.selection_fetched = 1;
  context.property_present = true;
  context.property.scope = &scope;
  context.property.result = S_OK;
  scope.result = S_OK;
  scope.malformed = false;
  scope.values = {IS_TEXT};
  context.range.text_result = S_OK;
  context.range.reported_text_count = 0;
  context.range.reported_shifted = -999;
  const std::wstring arbitrary_prefix = L"前文不固定：你好，当前输入框";
  context.range.text = arbitrary_prefix;
  context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  auto snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(snapshot_read.request_hr == S_OK && snapshot_read.session_hr == S_OK &&
         snapshot_read.read_hr == S_OK && snapshot_read.prefix_read_ok &&
         prefix_snapshot.text() == arbitrary_prefix &&
         snapshot_read.count == arbitrary_prefix.size() &&
         snapshot_read.shifted == -static_cast<LONG>(arbitrary_prefix.size()),
         "snapshot reader returns an arbitrary Chinese suffix without fixed-text matching or absolute offset");

  std::array<WCHAR, PrefixSnapshot::kCapacity> maximum_prefix{};
  maximum_prefix.fill(L'中');
  maximum_prefix[63] = static_cast<WCHAR>(0xD83D);
  maximum_prefix[64] = static_cast<WCHAR>(0xDE00);
  context.range.text.assign(maximum_prefix.data(), maximum_prefix.size());
  context.range.reported_shifted = -static_cast<LONG>(maximum_prefix.size());
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(snapshot_read.prefix_read_ok && snapshot_read.count == PrefixSnapshot::kCapacity &&
         prefix_snapshot.text().size() == PrefixSnapshot::kCapacity &&
         prefix_snapshot.text()[63] == static_cast<WCHAR>(0xD83D) &&
         prefix_snapshot.text()[64] == static_cast<WCHAR>(0xDE00),
         "snapshot reader accepts the full 128-unit suffix with an intact surrogate pair");

  context.range.text = arbitrary_prefix;
  context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(snapshot_read.prefix_read_ok && prefix_snapshot.text() == arbitrary_prefix,
         "valid read repopulates the caller-owned snapshot before rejection cases");
  const int snapshot_reads_before_denials = context.range.get_text_calls;
  context.range.empty = false;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(!snapshot_read.prefix_read_ok && !snapshot_read.body_read &&
         prefix_snapshot.text().empty() &&
         context.range.get_text_calls == snapshot_reads_before_denials,
         "nonempty selection is rejected and clears prior snapshot text");
  context.range.empty = true;
  for (const auto denied_end : {TF_AE_NONE, TF_AE_START, TF_AE_END}) {
    context.active_end = denied_end;
    context.range.empty = false;
    snapshot_gate = {};
    snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
    Expect(!snapshot_read.prefix_read_ok && !snapshot_read.body_read &&
           prefix_snapshot.text().empty() &&
           context.range.get_text_calls == snapshot_reads_before_denials,
           "non-collapsed active-end selection is denied and leaves no prior snapshot");
  }
  context.active_end = TF_AE_END;
  context.range.empty = true;
  context.interim_character = TRUE;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  context.interim_character = FALSE;
  Expect(!snapshot_read.prefix_read_ok && !snapshot_read.body_read &&
         prefix_snapshot.text().empty() &&
         context.range.get_text_calls == snapshot_reads_before_denials,
         "interim selection is denied without text access or retained output");
  for (InputScope denied_scope : {static_cast<InputScope>(9999), IS_PRIVATE,
                                  IS_NUMERIC_PIN, IS_PASSWORD}) {
    scope.values = {denied_scope};
    snapshot_gate = {};
    snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
    Expect(!snapshot_read.prefix_read_ok && !snapshot_read.body_read &&
           prefix_snapshot.text().empty() &&
           context.range.get_text_calls == snapshot_reads_before_denials,
           "unknown, private, PIN and password scopes are denied without text access");
  }
  scope.values = {IS_TEXT};

  for (int boundary : {2, 3, 4, 5}) {
    context.range.text = arbitrary_prefix;
    context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
    snapshot_gate = {};
    snapshot_gate.deny_on_call = boundary;
    snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
    Expect(!snapshot_read.prefix_read_ok && prefix_snapshot.text().empty(),
           "focus/revision gate changes before, during, or after the synchronous read clear output");
  }
  snapshot_gate = {};
  context.range.after_get_text = +[](void* value) {
    static_cast<Scope*>(value)->values = {IS_PRIVATE};
  };
  context.range.after_get_text_context = &scope;
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  context.range.after_get_text = nullptr;
  context.range.after_get_text_context = nullptr;
  Expect(snapshot_read.body_read && !snapshot_read.prefix_read_ok &&
         prefix_snapshot.text().empty(),
         "privacy change after GetText vetoes the read and clears copied output");
  scope.values = {IS_TEXT};

  for (const std::wstring invalid_text : {
           std::wstring{L'a', L'\0', L'b'},
           std::wstring{static_cast<WCHAR>(0xD800)},
           std::wstring{static_cast<WCHAR>(0xDC00)},
           std::wstring{static_cast<WCHAR>(0xD800), L'x'}}) {
    context.range.text = invalid_text;
    context.range.reported_shifted = -static_cast<LONG>(invalid_text.size());
    snapshot_gate = {};
    snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
    Expect(snapshot_read.body_read && !snapshot_read.prefix_read_ok &&
           prefix_snapshot.text().empty(),
           "NUL and malformed UTF-16 fail closed with no retained prefix");
  }

  context.range.text = arbitrary_prefix;
  context.range.reported_shifted = 0;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(!snapshot_read.prefix_read_ok && !snapshot_read.body_read &&
         prefix_snapshot.text().empty(), "zero shifted extent cannot authorize text access");
  context.range.reported_shifted = -129;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(!snapshot_read.prefix_read_ok && !snapshot_read.body_read &&
         prefix_snapshot.text().empty(), "out-of-capacity shifted extent is rejected");
  context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  for (const auto [hr, reported_count] : {
           std::pair{S_OK, static_cast<ULONG>(arbitrary_prefix.size() - 1)},
           std::pair{S_FALSE, static_cast<ULONG>(arbitrary_prefix.size())},
           std::pair{E_FAIL, static_cast<ULONG>(arbitrary_prefix.size())},
           std::pair{S_OK, static_cast<ULONG>(PrefixSnapshot::kCapacity + 1)}}) {
    context.range.text_result = hr;
    context.range.reported_text_count = reported_count;
    snapshot_gate = {};
    snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
    Expect(snapshot_read.body_read && snapshot_read.read_hr == hr &&
           snapshot_read.count == reported_count && !snapshot_read.prefix_read_ok &&
           prefix_snapshot.text().empty(),
           "partial S_OK/S_FALSE, error, or out-of-bounds count cannot retain text");
  }
  context.range.text_result = S_OK;
  context.range.reported_text_count = 0;

  context.range.text = arbitrary_prefix;
  context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  context.repeat_edit_session = true;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  context.repeat_edit_session = false;
  Expect(!snapshot_read.prefix_read_ok && prefix_snapshot.text().empty(),
         "repeated DoEditSession callback cannot preserve first read output");
  context.defer_edit_session = true;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  context.defer_edit_session = false;
  Expect(!snapshot_read.prefix_read_ok && prefix_snapshot.text().empty() &&
         context.deferred_session != nullptr &&
         context.deferred_session->DoEditSession(1) == E_ABORT,
         "deferred callback is deactivated and cannot repopulate returned output");
  context.deferred_session->Release();
  context.deferred_session = nullptr;
  context.foreign_thread_edit_session = true;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  context.foreign_thread_edit_session = false;
  Expect(!snapshot_read.prefix_read_ok && snapshot_read.session_hr == E_ACCESSDENIED &&
         prefix_snapshot.text().empty(), "foreign-thread callback cannot retain snapshot text");

  context.range.text = arbitrary_prefix;
  context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(snapshot_read.prefix_read_ok, "snapshot is available for clear-memory contract");
  const std::wstring_view retained_view = prefix_snapshot.text();
  prefix_snapshot.Clear();
  Expect(prefix_snapshot.text().empty() &&
         std::all_of(retained_view.begin(), retained_view.end(),
                     [](WCHAR unit) { return unit == L'\0'; }),
         "Clear zeroes the caller buffer observed by an already-retained text view");

  // Synthetic reader-to-binding interoperability. Caller-provided metadata is
  // deliberately fabricated here and is not a production identity/offset source.
  context.range.text = arbitrary_prefix;
  context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  const std::u16string issued_prefix = to_utf16(prefix_snapshot.text());
  PrefixBinding accepted_binding(LifetimeToken{0xCAFE, 0xBEEF});
  const auto test_snapshot = declared_test_snapshot(snapshot_read.prefix_read_ok);
  const auto issued_at = PrefixBinding::TimePoint{} + std::chrono::seconds(1);
  const auto deadline = PrefixBinding::TimePoint{} + std::chrono::seconds(5);
  const auto accepted_ticket = accepted_binding.Begin(test_snapshot, issued_prefix, issued_at,
                                                       deadline);
  Expect(accepted_ticket.has_value(), "synthetic current snapshot starts one bound request");
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(snapshot_read.prefix_read_ok &&
         accepted_binding.TryAccept(*accepted_ticket,
                                    declared_test_snapshot(snapshot_read.prefix_read_ok),
                                    to_utf16(prefix_snapshot.text()), issued_at +
                                        std::chrono::seconds(1)),
         "unchanged synthetic current-session suffix passes the existing binding veto");

  context.range.text = arbitrary_prefix;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  PrefixBinding changed_binding(LifetimeToken{0xCAFE, 0xBEE0});
  const auto changed_ticket = changed_binding.Begin(test_snapshot, issued_prefix, issued_at,
                                                     deadline);
  Expect(changed_ticket.has_value(), "synthetic changed-prefix request starts");
  std::wstring same_length_different = arbitrary_prefix;
  same_length_different.back() = L'景';
  context.range.text = same_length_different;
  context.range.reported_shifted = -static_cast<LONG>(same_length_different.size());
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  Expect(snapshot_read.prefix_read_ok &&
         !changed_binding.TryAccept(*changed_ticket,
                                    declared_test_snapshot(snapshot_read.prefix_read_ok),
                                    to_utf16(prefix_snapshot.text()), issued_at +
                                        std::chrono::seconds(1)),
         "same-length changed synthetic suffix is vetoed by exact prefix binding");

  context.range.text = arbitrary_prefix;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  PrefixBinding denied_binding(LifetimeToken{0xCAFE, 0xBEE1});
  const auto denied_ticket = denied_binding.Begin(test_snapshot, issued_prefix, issued_at,
                                                   deadline);
  Expect(denied_ticket.has_value(), "synthetic denied-fresh-read request starts");
  scope.values = {IS_PASSWORD};
  snapshot_gate = {};
  snapshot_read = read_current_prefix(prefix_snapshot, snapshot_gate);
  scope.values = {IS_TEXT};
  Expect(!snapshot_read.prefix_read_ok && prefix_snapshot.text().empty() &&
         !denied_binding.TryAccept(*denied_ticket,
                                   declared_test_snapshot(snapshot_read.prefix_read_ok),
                                   to_utf16(prefix_snapshot.text()), issued_at +
                                       std::chrono::seconds(1)),
         "denied fresh snapshot cannot pass the synthetic binding gate");

  prefix_snapshot.Clear();
  Expect(Range::active_clones == 0, "clearing reader output releases its caret clone");
  const ULONG context_refs_before_anchor_tests = context.references;
  const LONG clones_before_anchor_tests = Range::active_clones;
  PrefixSnapshot previous_snapshot;
  PrefixSnapshot current_snapshot;
  context.range.start_position = 40;
  context.range.end_position = 40;
  context.range.text = arbitrary_prefix;
  context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  snapshot_gate = {};
  snapshot_read = read_current_prefix(previous_snapshot, snapshot_gate);
  Expect(snapshot_read.prefix_read_ok && Range::active_clones == clones_before_anchor_tests + 1 &&
         context.references == context_refs_before_anchor_tests + 1,
         "successful previous snapshot retains exactly one caret clone and context reference");

  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate, &previous_snapshot);
  Expect(snapshot_read.prefix_read_ok && snapshot_read.caret_compared &&
         snapshot_read.caret_matches && Range::last_equal_start_anchor == TF_ANCHOR_START &&
         Range::last_equal_end_anchor == TF_ANCHOR_END,
         "unchanged same-context caret matches both anchors before reading text");

  const int get_text_before_anchor_mismatches = context.range.get_text_calls;
  context.range.start_position = 41;
  context.range.end_position = 40;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate, &previous_snapshot);
  Expect(snapshot_read.caret_compared && !snapshot_read.caret_matches &&
         !snapshot_read.body_read && !snapshot_read.prefix_read_ok &&
         current_snapshot.text().empty() &&
         context.range.get_text_calls == get_text_before_anchor_mismatches,
         "moved caret start anchor vetoes before GetText");

  context.range.start_position = 40;
  context.range.end_position = 41;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate, &previous_snapshot);
  Expect(snapshot_read.caret_compared && !snapshot_read.caret_matches &&
         !snapshot_read.body_read && current_snapshot.text().empty() &&
         context.range.get_text_calls == get_text_before_anchor_mismatches,
         "only the moved end anchor is sufficient to veto before GetText");

  context.range.start_position = 40;
  context.range.end_position = 40;
  Range::equal_start_result = S_FALSE;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate, &previous_snapshot);
  Expect(snapshot_read.caret_compared && !snapshot_read.caret_matches &&
         !snapshot_read.body_read && current_snapshot.text().empty() &&
         context.range.get_text_calls == get_text_before_anchor_mismatches,
         "S_FALSE from IsEqualStart is not accepted as a matching anchor");
  Range::equal_start_result = S_OK;
  Range::equal_end_result = E_FAIL;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate, &previous_snapshot);
  Expect(snapshot_read.caret_compared && !snapshot_read.caret_matches &&
         !snapshot_read.body_read && current_snapshot.text().empty() &&
         context.range.get_text_calls == get_text_before_anchor_mismatches,
         "failed IsEqualEnd is not accepted as a matching anchor");
  Range::equal_end_result = S_OK;

  Context other_context;
  Scope other_scope;
  other_context.property_present = true;
  other_context.property_result = S_OK;
  other_context.property.scope = &other_scope;
  other_context.property.result = S_OK;
  other_scope.values = {IS_TEXT};
  other_context.range.allow_text_read = true;
  other_context.range.empty = true;
  other_context.range.start_position = 40;
  other_context.range.end_position = 40;
  other_context.range.text = arbitrary_prefix;
  other_context.range.reported_shifted = -static_cast<LONG>(arbitrary_prefix.size());
  snapshot_gate = {};
  const auto foreign_context_read = ReadContextPrefixSnapshot(
      &other_context, 7, 0, CheckAllowed, &snapshot_gate, current_snapshot,
      &previous_snapshot);
  Expect(foreign_context_read.request_hr == S_OK,
         "different-context synthetic comparison receives a synchronous request");
  Expect(foreign_context_read.session_hr == E_ABORT,
         "different context fails the prior-context comparison");
  Expect(!foreign_context_read.caret_compared && !foreign_context_read.body_read &&
         current_snapshot.text().empty() && other_context.range.get_text_calls == 0,
         "same-position caret from a different context is rejected before text read");
  current_snapshot.Clear();
  Expect(other_context.references == 1 && other_context.range.references == 1 &&
         Range::active_clones == clones_before_anchor_tests + 1,
         "foreign-context veto releases temporary COM identity/range references");

  context.range.clone_result = E_FAIL;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate);
  Expect(!snapshot_read.prefix_read_ok && !snapshot_read.body_read &&
         current_snapshot.text().empty() && Range::active_clones == clones_before_anchor_tests + 1,
         "clone failure vetoes before text and releases no unrelated prior clone");
  context.range.clone_result = S_OK;

  context.range.after_get_text = DenyAfterRead;
  context.range.after_get_text_context = &snapshot_gate;
  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate, &previous_snapshot);
  context.range.after_get_text = nullptr;
  context.range.after_get_text_context = nullptr;
  Expect(snapshot_read.caret_compared && !snapshot_read.caret_matches &&
         snapshot_read.body_read && !snapshot_read.prefix_read_ok &&
         current_snapshot.text().empty(),
         "changed focus/revision gate vetoes after anchor comparison and GetText");

  previous_snapshot.Clear();
  snapshot_gate = {};
  snapshot_read = read_current_prefix(current_snapshot, snapshot_gate, &previous_snapshot);
  Expect(!snapshot_read.caret_compared && !snapshot_read.body_read &&
         current_snapshot.text().empty(),
         "a cleared previous snapshot cannot serve as a comparison token");
  snapshot_gate = {};
  const int reads_before_alias = context.range.get_text_calls;
  snapshot_read = read_current_prefix(previous_snapshot, snapshot_gate, &previous_snapshot);
  Expect(snapshot_read.request_hr == E_PENDING && !snapshot_read.body_read &&
         previous_snapshot.text().empty() && context.range.get_text_calls == reads_before_alias,
         "aliased output/previous snapshot clears and rejects without a host read");
  current_snapshot.Clear();
  previous_snapshot.Clear();
  Expect(Range::active_clones == clones_before_anchor_tests &&
         context.references == context_refs_before_anchor_tests &&
         context.range.references == 1 && context.property.references == 1 &&
         scope.references == 1,
         "all previous/current caret clones and canonical context references balance on Clear");

  std::cout << "TSF privacy contracts PASS (not a real-application password-field gate)\n";
}
