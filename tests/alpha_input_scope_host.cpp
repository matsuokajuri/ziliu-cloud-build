#include <windows.h>

#include <initguid.h>
#include <inputscope.h>
#include <msctf.h>
#include <richedit.h>
#include <shellapi.h>
#include <UIAutomation.h>

#include "../src/tsf/include/ziliu/tsf/guids.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

namespace {

constexpr wchar_t kWindowClass[] = L"ZiliuAlphaInputScopeHost";
constexpr wchar_t kWindowTitle[] = L"Ziliu TEST ONLY - InputScope host fixture";
constexpr int kExportButtonId = 100;
constexpr UINT_PTR kProbeTimerId = 2;
constexpr UINT kProbeDelayMilliseconds = 120;
constexpr ULONG kProbeTextLimit = 128;

struct ContextDiagnostic {
  HRESULT thread_manager_get_focus = E_PENDING;
  HRESULT document_manager_get_top = E_PENDING;
  HRESULT request_edit_session = E_PENDING;
  HRESULT edit_session = E_PENDING;
  HRESULT get_selection = E_PENDING;
  HRESULT clone_range = E_PENDING;
  HRESULT collapse_range = E_PENDING;
  HRESULT get_app_property = E_PENDING;
  HRESULT get_value = E_PENDING;
  HRESULT query_input_scope = E_PENDING;
  HRESULT get_input_scopes = E_PENDING;
  bool focus_matches_edit = false;
  std::vector<int> scope_values;
};

struct Field {
  std::wstring_view label;
  std::wstring_view scope_name;
  std::optional<InputScope> scope;
  DWORD edit_style;
  HWND edit = nullptr;
  HRESULT set_scope_result = S_FALSE;
  bool scope_applied = false;
  ContextDiagnostic context;
};

struct ProbeIdentity {
  IUnknown* canonical = nullptr;
  unsigned int ordinal = 0;
};

struct ProbeResult {
  std::wstring name;
  std::wstring field;
  std::wstring expected_prefix;
  std::wstring actual_prefix;
  HRESULT focus = E_PENDING;
  HRESULT action = E_PENDING;
  HRESULT set_input_scope = E_PENDING;
  HRESULT selection_scope_value = E_PENDING;
  VARTYPE selection_scope_type = VT_EMPTY;
  VARTYPE caret_scope_type = VT_EMPTY;
  HRESULT thread_manager_get_focus = E_PENDING;
  HRESULT document_manager_get_top = E_PENDING;
  HRESULT document_identity = E_PENDING;
  HRESULT context_identity = E_PENDING;
  HRESULT request_edit_session = E_PENDING;
  HRESULT edit_session = E_PENDING;
  HRESULT get_selection = E_PENDING;
  HRESULT query_range_acp = E_PENDING;
  HRESULT get_selection_extent = E_PENDING;
  HRESULT clone_range = E_PENDING;
  HRESULT collapse_range = E_PENDING;
  HRESULT get_app_property = E_PENDING;
  HRESULT get_value = E_PENDING;
  HRESULT query_input_scope = E_PENDING;
  HRESULT get_input_scopes = E_PENDING;
  HRESULT shift_start = E_PENDING;
  HRESULT get_text = E_PENDING;
  LONG expected_start = 0;
  LONG expected_length = 0;
  LONG actual_start = -1;
  LONG actual_length = -1;
  unsigned int document_id = 0;
  unsigned int context_id = 0;
  bool ordinary_scope = false;
  bool passed = false;
};

struct AppState {
  std::filesystem::path output_path;
  HWND status = nullptr;
  HMODULE rich_edit_module = nullptr;
  ITfThreadMgr* thread_manager = nullptr;
  TfClientId client_id = TF_CLIENTID_NULL;
  HRESULT thread_manager_create = E_PENDING;
  HRESULT thread_manager_activate = E_PENDING;
  bool context_probe = false;
  unsigned int probe_step = 0;
  std::vector<ProbeIdentity> probe_identities;
  std::vector<ProbeResult> probe_results;
  HRESULT probe_last_action = S_OK;
  bool probe_passed = false;
  std::array<Field, 6> fields = {{{L"Ordinary (IS_DEFAULT)", L"IS_DEFAULT", IS_DEFAULT, 0},
                                  {L"Private (IS_PRIVATE)", L"IS_PRIVATE", IS_PRIVATE, 0},
                                  {L"Password (IS_PASSWORD)", L"IS_PASSWORD", IS_PASSWORD,
                                   ES_PASSWORD},
                                  {L"PIN (IS_NUMERIC_PIN)", L"IS_NUMERIC_PIN", IS_NUMERIC_PIN,
                                   ES_PASSWORD},
                                  {L"Unspecified scope", L"UNSPECIFIED", std::nullopt, 0},
                                  {L"Second ordinary (IS_DEFAULT)", L"IS_DEFAULT", IS_DEFAULT,
                                   0}}};
};

class ScopeReadSession final : public ITfEditSession {
 public:
  ScopeReadSession(Field* field, ITfContext* context) : field_(field), context_(context) {
    context_->AddRef();
  }

  STDMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
    if (object == nullptr) return E_INVALIDARG;
    *object = nullptr;
    if (interface_id == IID_IUnknown || interface_id == IID_ITfEditSession) {
      *object = static_cast<ITfEditSession*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    ContextDiagnostic& diagnostic = field_->context;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    diagnostic.get_selection = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
    if (FAILED(diagnostic.get_selection) || fetched != 1 || selection.range == nullptr) return S_OK;
    ITfRange* range = selection.range;
    ITfRange* collapsed = nullptr;
    diagnostic.clone_range = range->Clone(&collapsed);
    range->Release();
    if (FAILED(diagnostic.clone_range) || collapsed == nullptr) return S_OK;
    diagnostic.collapse_range = collapsed->Collapse(
        cookie, selection.style.ase == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END);
    if (FAILED(diagnostic.collapse_range)) {
      collapsed->Release();
      return S_OK;
    }
    ITfReadOnlyProperty* property = nullptr;
    diagnostic.get_app_property = context_->GetAppProperty(GUID_PROP_INPUTSCOPE, &property);
    if (SUCCEEDED(diagnostic.get_app_property) && property != nullptr) {
      VARIANT value;
      VariantInit(&value);
      diagnostic.get_value = property->GetValue(cookie, collapsed, &value);
      if (SUCCEEDED(diagnostic.get_value) && value.vt == VT_UNKNOWN && value.punkVal != nullptr) {
        ITfInputScope* scope = nullptr;
        diagnostic.query_input_scope = value.punkVal->QueryInterface(IID_PPV_ARGS(&scope));
        if (SUCCEEDED(diagnostic.query_input_scope) && scope != nullptr) {
          InputScope* values = nullptr;
          UINT count = 0;
          diagnostic.get_input_scopes = scope->GetInputScopes(&values, &count);
          if (SUCCEEDED(diagnostic.get_input_scopes) && values != nullptr) {
            for (UINT index = 0; index < count; ++index) {
              diagnostic.scope_values.push_back(static_cast<int>(values[index]));
            }
          }
          CoTaskMemFree(values);
          scope->Release();
        }
      }
      VariantClear(&value);
      property->Release();
    }
    collapsed->Release();
    return S_OK;
  }

 private:
  ~ScopeReadSession() { context_->Release(); }
  std::atomic<ULONG> references_{1};
  Field* field_;
  ITfContext* context_ = nullptr;
};

bool IsKnownOrdinaryScope(InputScope scope) {
  const int value = static_cast<int>(scope);
  return value >= static_cast<int>(IS_DEFAULT) &&
         value <= static_cast<int>(IS_CHAT_WITHOUT_EMOJI) && scope != IS_PRIVATE &&
         scope != IS_PASSWORD && scope != IS_NUMERIC_PASSWORD &&
         scope != IS_NUMERIC_PIN && scope != IS_ALPHANUMERIC_PIN &&
         scope != IS_ALPHANUMERIC_PIN_SET;
}

class ProbeReadSession final : public ITfEditSession {
 public:
  ProbeReadSession(ITfContext* context, ProbeResult* result)
      : context_(context), result_(result) { context_->AddRef(); }
  STDMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
    if (object == nullptr) return E_INVALIDARG;
    *object = nullptr;
    if (interface_id == IID_IUnknown || interface_id == IID_ITfEditSession) {
      *object = static_cast<ITfEditSession*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }
  STDMETHODIMP DoEditSession(TfEditCookie cookie) override {
    TF_SELECTION selection{};
    ULONG fetched = 0;
    result_->get_selection = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1,
                                                     &selection, &fetched);
    if (FAILED(result_->get_selection) || fetched != 1 || selection.range == nullptr) return S_OK;
    ITfRangeACP* range_acp = nullptr;
    result_->query_range_acp = selection.range->QueryInterface(IID_PPV_ARGS(&range_acp));
    if (SUCCEEDED(result_->query_range_acp) && range_acp != nullptr) {
      result_->get_selection_extent = range_acp->GetExtent(
          &result_->actual_start, &result_->actual_length);
      range_acp->Release();
    }
    ITfRange* clone = nullptr;
    result_->clone_range = selection.range->Clone(&clone);
    selection.range->Release();
    if (FAILED(result_->clone_range) || clone == nullptr) return S_OK;
    if (FAILED(result_->get_selection_extent)) { clone->Release(); return S_OK; }
    // Compare the uncollapsed selection with the insertion point. This is only
    // property diagnostics: neither result permits bypassing the caret gate.
    ITfReadOnlyProperty* property = nullptr;
    result_->get_app_property = context_->GetAppProperty(GUID_PROP_INPUTSCOPE, &property);
    if (FAILED(result_->get_app_property) || property == nullptr) { clone->Release(); return S_OK; }
    VARIANT value;
    VariantInit(&value);
    result_->selection_scope_value = property->GetValue(cookie, clone, &value);
    result_->selection_scope_type = value.vt;
    VariantClear(&value);
    result_->collapse_range = clone->Collapse(cookie, TF_ANCHOR_START);
    if (FAILED(result_->collapse_range)) {
      property->Release();
      clone->Release();
      return S_OK;
    }
    VariantInit(&value);
    result_->get_value = property->GetValue(cookie, clone, &value);
    result_->caret_scope_type = value.vt;
    if (SUCCEEDED(result_->get_value) && value.vt == VT_UNKNOWN && value.punkVal != nullptr) {
      ITfInputScope* input_scope = nullptr;
      result_->query_input_scope = value.punkVal->QueryInterface(IID_PPV_ARGS(&input_scope));
      if (SUCCEEDED(result_->query_input_scope) && input_scope != nullptr) {
        InputScope* scopes = nullptr;
        UINT count = 0;
        result_->get_input_scopes = input_scope->GetInputScopes(&scopes, &count);
        if (SUCCEEDED(result_->get_input_scopes) && scopes != nullptr && count != 0) {
          result_->ordinary_scope = true;
          for (UINT index = 0; index < count; ++index) {
            if (!IsKnownOrdinaryScope(scopes[index])) result_->ordinary_scope = false;
          }
        }
        CoTaskMemFree(scopes);
        input_scope->Release();
      }
    }
    VariantClear(&value);
    property->Release();
    if (result_->ordinary_scope) {
      LONG shifted = 0;
      result_->shift_start = clone->ShiftStart(cookie, -static_cast<LONG>(kProbeTextLimit),
                                                &shifted, nullptr);
      if (SUCCEEDED(result_->shift_start)) {
        std::array<WCHAR, kProbeTextLimit> buffer{};
        ULONG text_length = 0;
        result_->get_text = clone->GetText(cookie, 0, buffer.data(), kProbeTextLimit,
                                           &text_length);
        if (SUCCEEDED(result_->get_text) && text_length <= kProbeTextLimit) {
          result_->actual_prefix.assign(buffer.data(), text_length);
        }
      }
    }
    clone->Release();
    return S_OK;
  }
 private:
  ~ProbeReadSession() { context_->Release(); }
  std::atomic<ULONG> references_{1};
  ITfContext* context_;
  ProbeResult* result_;
};

enum class Mode { kFields, kAuditRegistration, kContextProbe, kUiaFocusProbe, kUiaSelfTest };

struct Arguments {
  Mode mode;
  std::filesystem::path output_path;
};

using SetInputScopeFunction = HRESULT(WINAPI*)(HWND, InputScope);

std::wstring HresultText(HRESULT result) {
  wchar_t buffer[16]{};
  swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(result));
  return buffer;
}

std::wstring GuidText(REFGUID guid) {
  wchar_t buffer[40]{};
  return StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) > 0
      ? std::wstring(buffer)
      : L"";
}

std::wstring JsonEscape(std::wstring_view input) {
  std::wstring escaped;
  escaped.reserve(input.size());
  for (const wchar_t character : input) {
    switch (character) {
      case L'\\': escaped += L"\\\\"; break;
      case L'\"': escaped += L"\\\""; break;
      case L'\n': escaped += L"\\n"; break;
      case L'\r': escaped += L"\\r"; break;
      case L'\t': escaped += L"\\t"; break;
      default:
        if (character < 0x20 || character > 0x7e) {
          wchar_t buffer[7]{};
          swprintf_s(buffer, L"\\u%04X", static_cast<unsigned int>(character));
          escaped += buffer;
        } else {
          escaped += character;
        }
    }
  }
  return escaped;
}

std::vector<unsigned int> ToCodepoints(std::wstring_view text) {
  std::vector<unsigned int> codepoints;
  for (std::size_t index = 0; index < text.size(); ++index) {
    unsigned int value = text[index];
    if (value >= 0xD800 && value <= 0xDBFF && index + 1 < text.size()) {
      const unsigned int low = text[index + 1];
      if (low >= 0xDC00 && low <= 0xDFFF) {
        value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
        ++index;
      }
    }
    codepoints.push_back(value);
  }
  return codepoints;
}

std::wstring ReadOwnedEditText(HWND edit) {
  const LRESULT length = SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0);
  if (length < 0 || length > 4096) return L"";
  std::wstring text(static_cast<std::size_t>(length), L'\0');
  if (length != 0) {
    SendMessageW(edit, WM_GETTEXT, static_cast<WPARAM>(text.size() + 1),
                 reinterpret_cast<LPARAM>(text.data()));
  }
  return text;
}

std::wstring FocusName(const AppState& state, HWND focus) {
  for (const Field& field : state.fields) {
    if (field.edit == focus) return std::wstring(field.label);
  }
  return focus == nullptr ? L"none" : L"test-window-control";
}

Field* FindField(AppState* state, HWND edit) {
  for (Field& field : state->fields) {
    if (field.edit == edit) return &field;
  }
  return nullptr;
}

void CaptureFocusedContext(AppState* state, HWND edit) {
  Field* field = FindField(state, edit);
  if (field == nullptr) return;
  field->context = ContextDiagnostic{};
  field->context.focus_matches_edit = GetFocus() == edit;
  if (state->thread_manager == nullptr || state->client_id == TF_CLIENTID_NULL) return;
  ITfDocumentMgr* document_manager = nullptr;
  field->context.thread_manager_get_focus = state->thread_manager->GetFocus(&document_manager);
  if (FAILED(field->context.thread_manager_get_focus) || document_manager == nullptr) return;
  ITfContext* context = nullptr;
  field->context.document_manager_get_top = document_manager->GetTop(&context);
  document_manager->Release();
  if (FAILED(field->context.document_manager_get_top) || context == nullptr) return;
  auto* session = new (std::nothrow) ScopeReadSession(field, context);
  if (session == nullptr) {
    context->Release();
    field->context.request_edit_session = E_OUTOFMEMORY;
    return;
  }
  HRESULT session_result = E_FAIL;
  field->context.request_edit_session = context->RequestEditSession(
      state->client_id, session, TF_ES_SYNC | TF_ES_READ, &session_result);
  field->context.edit_session = session_result;
  session->Release();
  context->Release();
}

unsigned int HoldIdentity(AppState* state, IUnknown* object, HRESULT* result) {
  IUnknown* canonical = nullptr;
  *result = object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&canonical));
  if (FAILED(*result) || canonical == nullptr) return 0;
  for (const ProbeIdentity& identity : state->probe_identities) {
    if (identity.canonical == canonical) {
      canonical->Release();
      return identity.ordinal;
    }
  }
  const unsigned int ordinal = static_cast<unsigned int>(state->probe_identities.size() + 1);
  state->probe_identities.push_back({canonical, ordinal});
  return ordinal;
}

void CaptureProbeSnapshot(AppState* state, Field& field, std::wstring_view name,
                          std::wstring_view expected_prefix, LONG expected_start,
                          LONG expected_length) {
  ProbeResult result;
  result.name = name;
  result.field = field.label == L"Second ordinary (IS_DEFAULT)" ? L"B" : L"A";
  result.expected_prefix = expected_prefix;
  result.expected_start = expected_start;
  result.expected_length = expected_length;
  result.action = state->probe_last_action;
  result.focus = GetFocus() == field.edit ? S_OK : E_FAIL;
  if (result.focus == S_OK) {
    // SetInputScope is initially called before Rich Edit publishes a TSF document.
    // Reapply only this fixture's explicit ordinary scope after focus is settled;
    // the subsequent property read must still succeed before GetText is allowed.
    const HMODULE module = LoadLibraryExW(L"msctf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto setter = module == nullptr ? nullptr
        : reinterpret_cast<SetInputScopeFunction>(GetProcAddress(module, "SetInputScope"));
    result.set_input_scope = setter == nullptr ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
                                               : setter(field.edit, IS_DEFAULT);
    if (module != nullptr) FreeLibrary(module);
  }
  ITfDocumentMgr* document = nullptr;
  if (result.focus == S_OK) {
    result.thread_manager_get_focus = state->thread_manager == nullptr
        ? E_UNEXPECTED : state->thread_manager->GetFocus(&document);
  }
  if (SUCCEEDED(result.thread_manager_get_focus) && document != nullptr) {
    result.document_id = HoldIdentity(state, document, &result.document_identity);
    ITfContext* context = nullptr;
    result.document_manager_get_top = document->GetTop(&context);
    if (SUCCEEDED(result.document_manager_get_top) && context != nullptr) {
      result.context_id = HoldIdentity(state, context, &result.context_identity);
      auto* session = new (std::nothrow) ProbeReadSession(context, &result);
      if (session == nullptr) {
        result.request_edit_session = E_OUTOFMEMORY;
      } else {
        HRESULT session_result = E_FAIL;
        result.request_edit_session = context->RequestEditSession(
            state->client_id, session, TF_ES_SYNC | TF_ES_READ, &session_result);
        result.edit_session = session_result;
        session->Release();
      }
      context->Release();
    }
    document->Release();
  }
  result.passed = result.focus == S_OK && result.thread_manager_get_focus == S_OK &&
      result.document_manager_get_top == S_OK && result.document_id != 0 &&
      result.context_id != 0 && result.request_edit_session == S_OK &&
      result.edit_session == S_OK && result.get_selection == S_OK &&
      result.query_range_acp == S_OK &&
      result.get_selection_extent == S_OK && result.clone_range == S_OK &&
      result.collapse_range == S_OK && result.get_app_property == S_OK &&
      result.get_value == S_OK && result.query_input_scope == S_OK &&
      result.get_input_scopes == S_OK && result.ordinary_scope &&
      result.shift_start == S_OK && result.get_text == S_OK &&
      result.actual_start == expected_start && result.actual_length == expected_length &&
      result.actual_prefix == expected_prefix && state->probe_last_action == S_OK;
  result.passed = result.passed && result.set_input_scope == S_OK;
  state->probe_results.push_back(std::move(result));
}

bool ProbeIdentityStable(const AppState& state, std::wstring_view field,
                         std::wstring_view kind) {
  unsigned int first = 0;
  for (const ProbeResult& result : state.probe_results) {
    if (result.field != field) continue;
    const unsigned int identity = kind == L"document" ? result.document_id : result.context_id;
    if (identity == 0) return false;
    if (first == 0) first = identity;
    else if (first != identity) return false;
  }
  return first != 0;
}

bool ProbeIdentityDiffers(const AppState& state, std::wstring_view kind) {
  unsigned int a = 0;
  unsigned int b = 0;
  for (const ProbeResult& result : state.probe_results) {
    unsigned int& identity = result.field == L"A" ? a : b;
    if (identity == 0) identity = kind == L"document" ? result.document_id : result.context_id;
  }
  return a != 0 && b != 0 && a != b;
}

void SetStatus(HWND status, std::wstring_view text);

bool ExportProbe(const AppState& state, std::wstring* error) {
  std::wofstream output(state.output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) { *error = L"Could not write the caller-specified probe path."; return false; }
  output << L"{\n  \"testOnly\": true,\n  \"mode\": \"context-probe\",\n"
         << L"  \"snapshotOnly\": true,\n  \"documentIdentityDiffersAB\": "
         << (ProbeIdentityDiffers(state, L"document") ? L"true" : L"false")
         << L",\n  \"contextIdentityDiffersAB\": "
         << (ProbeIdentityDiffers(state, L"context") ? L"true" : L"false")
         << L",\n  \"documentIdentityStableA\": "
         << (ProbeIdentityStable(state, L"A", L"document") ? L"true" : L"false")
         << L",\n  \"contextIdentityStableA\": "
         << (ProbeIdentityStable(state, L"A", L"context") ? L"true" : L"false")
         << L",\n  \"allStepsPassed\": " << (state.probe_passed ? L"true" : L"false")
         << L",\n  \"steps\": [\n";
  for (std::size_t index = 0; index < state.probe_results.size(); ++index) {
    const ProbeResult& step = state.probe_results[index];
    output << L"    {\"name\": \"" << JsonEscape(step.name) << L"\", \"field\": \""
           << step.field << L"\", \"documentId\": " << step.document_id
           << L", \"contextId\": " << step.context_id << L", \"expectedPrefix\": \""
           << JsonEscape(step.expected_prefix) << L"\", \"actualPrefix\": \""
           << JsonEscape(step.actual_prefix) << L"\", \"expectedSelection\": ["
           << step.expected_start << L", " << step.expected_length << L"], \"actualSelection\": ["
           << step.actual_start << L", " << step.actual_length << L"], \"ordinaryScope\": "
           << (step.ordinary_scope ? L"true" : L"false") << L", \"passed\": "
           << (step.passed ? L"true" : L"false")
           << L", \"selectionScopeVariantType\": " << step.selection_scope_type
           << L", \"caretScopeVariantType\": " << step.caret_scope_type << L", \"hresult\": {"
           << L"\"focus\": \"" << HresultText(step.focus)
           << L"\", \"syntheticEditAction\": \"" << HresultText(step.action)
           << L"\", \"setInputScopeAfterFocus\": \"" << HresultText(step.set_input_scope)
           << L"\", \"selectionScopeGetValue\": \"" << HresultText(step.selection_scope_value)
           << L"\", \"threadManagerGetFocus\": \"" << HresultText(step.thread_manager_get_focus)
           << L"\", \"documentManagerGetTop\": \"" << HresultText(step.document_manager_get_top)
           << L"\", \"documentIdentity\": \"" << HresultText(step.document_identity)
           << L"\", \"contextIdentity\": \"" << HresultText(step.context_identity)
           << L"\", \"requestEditSession\": \"" << HresultText(step.request_edit_session)
           << L"\", \"editSession\": \"" << HresultText(step.edit_session)
           << L"\", \"getSelection\": \"" << HresultText(step.get_selection)
           << L"\", \"queryRangeAcp\": \"" << HresultText(step.query_range_acp)
           << L"\", \"selectionExtent\": \"" << HresultText(step.get_selection_extent)
           << L"\", \"cloneRange\": \"" << HresultText(step.clone_range)
           << L"\", \"collapseRange\": \"" << HresultText(step.collapse_range)
           << L"\", \"getAppProperty\": \"" << HresultText(step.get_app_property)
           << L"\", \"getValue\": \"" << HresultText(step.get_value)
           << L"\", \"queryInputScope\": \"" << HresultText(step.query_input_scope)
           << L"\", \"getInputScopes\": \"" << HresultText(step.get_input_scopes)
           << L"\", \"shiftStart\": \"" << HresultText(step.shift_start)
           << L"\", \"getText\": \"" << HresultText(step.get_text) << L"\"}}"
           << (index + 1 == state.probe_results.size() ? L"\n" : L",\n");
  }
  output << L"  ]\n}\n";
  if (!output.good()) { *error = L"Writing the caller-specified probe path failed."; return false; }
  return state.probe_passed;
}

bool ScheduleProbe(HWND window, AppState* state) {
  if (SetTimer(window, kProbeTimerId, kProbeDelayMilliseconds, nullptr) != 0) return true;
  state->probe_passed = false;
  std::wstring error;
  static_cast<void>(ExportProbe(*state, &error));
  PostMessageW(window, WM_CLOSE, 0, 0);
  return false;
}

void RunProbeStep(HWND window, AppState* state) {
  Field& a = state->fields[0];
  Field& b = state->fields[5];
  const auto set_text = [](HWND edit, const wchar_t* text) {
    return SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text)) != 0 ? S_OK : E_FAIL;
  };
  switch (state->probe_step++) {
    case 0:
      SetFocus(a.edit);
      state->probe_last_action = set_text(a.edit, L"甲乙丙丁");
      SendMessageW(a.edit, EM_SETSEL, 2, 4);
      break;
    case 1: CaptureProbeSnapshot(state, a, L"A_selected_suffix", L"甲乙", 2, 2); break;
    case 2:
      SetFocus(b.edit);
      state->probe_last_action = set_text(b.edit, L"戊己庚辛");
      SendMessageW(b.edit, EM_SETSEL, 4, 4);
      break;
    case 3: CaptureProbeSnapshot(state, b, L"B_caret_end", L"戊己庚辛", 4, 0); break;
    case 4: SetFocus(a.edit); state->probe_last_action = S_OK; break;
    case 5: CaptureProbeSnapshot(state, a, L"A_return", L"甲乙", 2, 2); break;
    case 6: SendMessageW(a.edit, EM_SETSEL, 1, 1); state->probe_last_action = S_OK; break;
    case 7: CaptureProbeSnapshot(state, a, L"A_caret_move", L"甲", 1, 0); break;
    case 8:
      SendMessageW(a.edit, EM_SETSEL, 2, 4);
      SendMessageW(a.edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"寅卯"));
      state->probe_last_action = S_OK;
      break;
    case 9: CaptureProbeSnapshot(state, a, L"A_replace", L"甲乙寅卯", 4, 0); break;
    case 10:
      SendMessageW(a.edit, EM_SETSEL, 2, 4);
      SendMessageW(a.edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
      state->probe_last_action = S_OK;
      break;
    case 11: CaptureProbeSnapshot(state, a, L"A_delete", L"甲乙", 2, 0); break;
    case 12:
      state->probe_last_action = set_text(a.edit, L"");
      SendMessageW(a.edit, EM_SETSEL, 0, 0);
      break;
    case 13: {
      CaptureProbeSnapshot(state, a, L"A_clear", L"", 0, 0);
      bool steps_passed = state->probe_results.size() == 7;
      for (const ProbeResult& result : state->probe_results) steps_passed = steps_passed && result.passed;
      // A document manager may serve multiple contexts. The pair must distinguish
      // these two test fields; identical pairs cannot prove field isolation.
      const bool identities = (ProbeIdentityDiffers(*state, L"document") ||
          ProbeIdentityDiffers(*state, L"context")) &&
          ProbeIdentityStable(*state, L"A", L"document") &&
          ProbeIdentityStable(*state, L"A", L"context");
      state->probe_passed = steps_passed && identities;
      std::wstring error;
      const bool exported = ExportProbe(*state, &error);
      SetStatus(state->status, exported ? L"Context probe complete; JSON exported."
                                        : L"Context probe failed; inspect the requested JSON path.");
      PostMessageW(window, WM_CLOSE, 0, 0);
      if (!exported) state->probe_passed = false;
      return;
    }
    default: break;
  }
  static_cast<void>(ScheduleProbe(window, state));
}

void WriteContextDiagnostic(std::wostream& output, const ContextDiagnostic& diagnostic) {
  output << L"\"focusMatchesEdit\": " << (diagnostic.focus_matches_edit ? L"true" : L"false")
         << L", \"threadMgrGetFocus\": \"" << HresultText(diagnostic.thread_manager_get_focus)
         << L"\", \"documentMgrGetTop\": \"" << HresultText(diagnostic.document_manager_get_top)
         << L"\", \"requestEditSession\": \"" << HresultText(diagnostic.request_edit_session)
         << L"\", \"editSession\": \"" << HresultText(diagnostic.edit_session)
         << L"\", \"getSelection\": \"" << HresultText(diagnostic.get_selection)
         << L"\", \"cloneRange\": \"" << HresultText(diagnostic.clone_range)
         << L"\", \"collapseRange\": \"" << HresultText(diagnostic.collapse_range)
         << L"\", \"getAppProperty\": \"" << HresultText(diagnostic.get_app_property)
         << L"\", \"getValue\": \"" << HresultText(diagnostic.get_value)
         << L"\", \"queryInputScope\": \"" << HresultText(diagnostic.query_input_scope)
         << L"\", \"getInputScopes\": \"" << HresultText(diagnostic.get_input_scopes)
         << L"\", \"scopeValues\": [";
  for (std::size_t index = 0; index < diagnostic.scope_values.size(); ++index) {
    output << diagnostic.scope_values[index];
    if (index + 1 != diagnostic.scope_values.size()) output << L", ";
  }
  output << L']';
}

bool Export(const AppState& state, std::wstring* error) {
  wchar_t focus_class[256]{};
  const HWND focus = GetFocus();
  if (focus != nullptr) GetClassNameW(focus, focus_class, static_cast<int>(std::size(focus_class)));

  std::wofstream output(state.output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    *error = L"Could not write the caller-specified export path.";
    return false;
  }
  output << L"{\n  \"testOnly\": true,\n  \"windowTitle\": \""
         << JsonEscape(kWindowTitle) << L"\",\n  \"windowClass\": \""
         << JsonEscape(kWindowClass) << L"\",\n  \"focus\": {\"window\": \""
         << JsonEscape(FocusName(state, focus)) << L"\", \"class\": \""
         << JsonEscape(focus_class) << L"\"},\n  \"threadManagerCreateHresult\": \""
         << HresultText(state.thread_manager_create) << L"\",\n  \"threadManagerActivateHresult\": \""
         << HresultText(state.thread_manager_activate) << L"\",\n  \"fields\": [\n";
  for (std::size_t index = 0; index < state.fields.size(); ++index) {
    const Field& field = state.fields[index];
    const std::wstring text = ReadOwnedEditText(field.edit);
    output << L"    {\"label\": \"" << JsonEscape(field.label) << L"\", \"scope\": \""
           << JsonEscape(field.scope_name) << L"\", \"setInputScopeHresult\": \""
           << (field.scope_applied ? HresultText(field.set_scope_result) : L"NOT_APPLIED")
           << L"\", \"value\": \"" << JsonEscape(text) << L"\", \"codepoints\": [";
    const std::vector<unsigned int> codepoints = ToCodepoints(text);
    for (std::size_t codepoint_index = 0; codepoint_index < codepoints.size(); ++codepoint_index) {
      wchar_t codepoint[12]{};
      swprintf_s(codepoint, L"U+%04X", codepoints[codepoint_index]);
      output << L'\"' << codepoint << L'\"';
      if (codepoint_index + 1 != codepoints.size()) output << L", ";
    }
    output << L"], \"focusedContext\": {";
    WriteContextDiagnostic(output, field.context);
    output << L"}}" << (index + 1 == state.fields.size() ? L"\n" : L",\n");
  }
  output << L"  ]\n}\n";
  if (!output.good()) {
    *error = L"Writing the caller-specified export path failed.";
    return false;
  }
  return true;
}

bool AuditRegistration(const std::filesystem::path& output_path, std::wstring* error) {
  const HRESULT initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize = SUCCEEDED(initialize_result);
  ITfCategoryMgr* category_manager = nullptr;
  const HRESULT category_create = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                                   CLSCTX_INPROC_SERVER,
                                                   IID_PPV_ARGS(&category_manager));
  IEnumGUID* category_enumerator = nullptr;
  const HRESULT category_enumerate = SUCCEEDED(category_create)
      ? category_manager->EnumCategoriesInItem(ziliu::tsf::kTextServiceClsid, &category_enumerator)
      : category_create;
  std::vector<GUID> categories;
  HRESULT category_terminal = category_enumerate;
  if (SUCCEEDED(category_enumerate) && category_enumerator != nullptr) {
    GUID category{};
    ULONG fetched = 0;
    for (;;) {
      const HRESULT next = category_enumerator->Next(1, &category, &fetched);
      if (next != S_OK || fetched != 1) {
        category_terminal = next;
        break;
      }
      categories.push_back(category);
    }
  } else if (SUCCEEDED(category_enumerate)) {
    category_terminal = E_UNEXPECTED;
  }
  if (category_enumerator != nullptr) category_enumerator->Release();
  if (category_manager != nullptr) category_manager->Release();

  ITfInputProcessorProfiles* profiles = nullptr;
  const HRESULT profiles_create = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                                   CLSCTX_INPROC_SERVER,
                                                   IID_PPV_ARGS(&profiles));
  IEnumGUID* processor_enumerator = nullptr;
  const HRESULT processor_enumerate = SUCCEEDED(profiles_create)
      ? profiles->EnumInputProcessorInfo(&processor_enumerator)
      : profiles_create;
  bool service_listed = false;
  HRESULT processor_terminal = processor_enumerate;
  if (SUCCEEDED(processor_enumerate) && processor_enumerator != nullptr) {
    GUID service{};
    ULONG fetched = 0;
    for (;;) {
      const HRESULT next = processor_enumerator->Next(1, &service, &fetched);
      if (next != S_OK || fetched != 1) {
        processor_terminal = next;
        break;
      }
      if (IsEqualGUID(service, ziliu::tsf::kTextServiceClsid)) service_listed = true;
    }
  } else if (SUCCEEDED(processor_enumerate)) {
    processor_terminal = E_UNEXPECTED;
  }
  if (processor_enumerator != nullptr) processor_enumerator->Release();

  IEnumTfLanguageProfiles* profile_enumerator = nullptr;
  const HRESULT profile_enumerate = SUCCEEDED(profiles_create)
      ? profiles->EnumLanguageProfiles(ziliu::tsf::kSimplifiedChineseLanguageId, &profile_enumerator)
      : profiles_create;
  bool exact_profile_listed = false;
  HRESULT profile_terminal = profile_enumerate;
  if (SUCCEEDED(profile_enumerate) && profile_enumerator != nullptr) {
    TF_LANGUAGEPROFILE profile{};
    ULONG fetched = 0;
    for (;;) {
      const HRESULT next = profile_enumerator->Next(1, &profile, &fetched);
      if (next != S_OK || fetched != 1) {
        profile_terminal = next;
        break;
      }
      if (IsEqualGUID(profile.clsid, ziliu::tsf::kTextServiceClsid) &&
          IsEqualGUID(profile.guidProfile, ziliu::tsf::kSimplifiedChineseProfileGuid)) {
        exact_profile_listed = true;
      }
    }
  } else if (SUCCEEDED(profile_enumerate)) {
    profile_terminal = E_UNEXPECTED;
  }
  if (profile_enumerator != nullptr) profile_enumerator->Release();
  if (profiles != nullptr) profiles->Release();

  std::wofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    if (uninitialize) CoUninitialize();
    *error = L"Could not write the caller-specified audit path.";
    return false;
  }
  output << L"{\n  \"testOnly\": true,\n  \"mode\": \"audit-registration\",\n"
         << L"  \"serviceClsid\": \"" << GuidText(ziliu::tsf::kTextServiceClsid) << L"\",\n"
         << L"  \"profileGuid\": \"" << GuidText(ziliu::tsf::kSimplifiedChineseProfileGuid) << L"\",\n"
         << L"  \"languageId\": \"0x0804\",\n"
         << L"  \"coInitializeHresult\": \"" << HresultText(initialize_result) << L"\",\n"
         << L"  \"categoryManagerCreateHresult\": \"" << HresultText(category_create) << L"\",\n"
         << L"  \"enumCategoriesInItemHresult\": \"" << HresultText(category_enumerate) << L"\",\n"
         << L"  \"enumCategoriesTerminalHresult\": \"" << HresultText(category_terminal)
         << L"\",\n  \"enumCategoriesComplete\": "
         << (category_terminal == S_FALSE ? L"true" : L"false") << L",\n"
         << L"  \"categories\": [";
  for (std::size_t index = 0; index < categories.size(); ++index) {
    output << L'\"' << GuidText(categories[index]) << L'\"';
    if (index + 1 != categories.size()) output << L", ";
  }
  output << L"],\n  \"inputProcessorProfilesCreateHresult\": \""
         << HresultText(profiles_create) << L"\",\n  \"enumInputProcessorInfoHresult\": \""
         << HresultText(processor_enumerate) << L"\",\n  \"serviceClsidListed\": "
         << (service_listed ? L"true" : L"false")
         << L",\n  \"enumInputProcessorInfoTerminalHresult\": \""
         << HresultText(processor_terminal) << L"\",\n  \"enumInputProcessorInfoComplete\": "
         << (processor_terminal == S_FALSE ? L"true" : L"false")
         << L",\n  \"enumLanguageProfilesHresult\": \"" << HresultText(profile_enumerate)
         << L"\",\n  \"exactLanguageProfileListed\": "
         << (exact_profile_listed ? L"true" : L"false")
         << L",\n  \"enumLanguageProfilesTerminalHresult\": \""
         << HresultText(profile_terminal) << L"\",\n  \"enumLanguageProfilesComplete\": "
         << (profile_terminal == S_FALSE ? L"true" : L"false") << L"\n}\n";
  const bool succeeded = output.good() && category_terminal == S_FALSE &&
                         processor_terminal == S_FALSE && profile_terminal == S_FALSE;
  if (uninitialize) CoUninitialize();
  if (!succeeded) {
    *error = L"Writing the caller-specified audit path failed.";
  }
  return succeeded;
}

void SetStatus(HWND status, std::wstring_view text) {
  SetWindowTextW(status, std::wstring(text).c_str());
}

HRESULT ApplyInputScope(SetInputScopeFunction set_input_scope, HWND edit, InputScope scope) {
  return set_input_scope == nullptr ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
                                    : set_input_scope(edit, scope);
}

void CreateControls(HWND window, AppState* state) {
  constexpr int kLabelX = 12;
  constexpr int kEditX = 210;
  constexpr int kStatusX = 450;
  constexpr int kFirstY = 16;
  constexpr int kRowHeight = 38;
  const HMODULE msctf_module = LoadLibraryExW(L"msctf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  const auto set_input_scope = msctf_module == nullptr ? nullptr
      : reinterpret_cast<SetInputScopeFunction>(GetProcAddress(msctf_module, "SetInputScope"));
  for (std::size_t index = 0; index < state->fields.size(); ++index) {
    Field& field = state->fields[index];
    const int y = kFirstY + static_cast<int>(index) * kRowHeight;
    CreateWindowExW(0, L"STATIC", std::wstring(field.label).c_str(), WS_CHILD | WS_VISIBLE,
                    kLabelX, y + 4, 190, 22, window, nullptr, nullptr, nullptr);
    field.edit = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                     field.edit_style,
                                 kEditX, y, 220, 24, window, nullptr, nullptr, nullptr);
    if (field.edit != nullptr) {
      // A classic EDIT may have no focused TSF document manager even when
      // SetInputScope succeeds. Rich Edit explicitly enables its TSF text store.
      SendMessageW(field.edit, EM_SETEDITSTYLE, SES_USECTF, SES_USECTF);
    }
    if (field.scope.has_value()) {
      field.set_scope_result = ApplyInputScope(set_input_scope, field.edit, *field.scope);
      field.scope_applied = true;
    }
    const std::wstring status = field.scope_applied
        ? L"SetInputScope: " + HresultText(field.set_scope_result)
        : L"SetInputScope: NOT_APPLIED";
    CreateWindowExW(0, L"STATIC", status.c_str(), WS_CHILD | WS_VISIBLE, kStatusX, y + 4, 210,
                    22, window, nullptr, nullptr, nullptr);
  }
  CreateWindowExW(0, L"BUTTON", L"Export bounded JSON", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                  kEditX, kFirstY + static_cast<int>(state->fields.size()) * kRowHeight + 4, 170,
                  28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExportButtonId)), nullptr,
                  nullptr);
  state->status = CreateWindowExW(0, L"STATIC", L"No data is exported until the button is clicked.",
                                  WS_CHILD | WS_VISIBLE, kEditX + 182,
                                  kFirstY + static_cast<int>(state->fields.size()) * kRowHeight + 8,
                                  300, 22, window, nullptr, nullptr, nullptr);
  if (msctf_module != nullptr) FreeLibrary(msctf_module);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  constexpr UINT_PTR kFocusCaptureTimer = 1;
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_NCCREATE:
      SetWindowLongPtrW(window, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
      return TRUE;
    case WM_CREATE:
      CreateControls(window, state);
      return 0;
    case WM_COMMAND:
      if (HIWORD(wparam) == EN_SETFOCUS) {
        // EN_SETFOCUS arrives before Rich Edit has published its TSF focus.
        // Read the context on the message loop after the focus transition.
        static_cast<void>(SetTimer(window, kFocusCaptureTimer, 100, nullptr));
        return 0;
      }
      if (LOWORD(wparam) == kExportButtonId && HIWORD(wparam) == BN_CLICKED) {
        std::wstring error;
        if (Export(*state, &error)) {
          SetStatus(state->status, L"Exported only this test window's fields to the caller path.");
        } else {
          SetStatus(state->status, error);
        }
      }
      return 0;
    case WM_TIMER:
      if (wparam == kFocusCaptureTimer) {
        KillTimer(window, kFocusCaptureTimer);
        CaptureFocusedContext(state, GetFocus());
        SetStatus(state->status, L"Focused Rich Edit TSF context captured; export to inspect it.");
        return 0;
      }
      if (wparam == kProbeTimerId && state->context_probe) {
        KillTimer(window, kProbeTimerId);
        RunProbeStep(window, state);
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
    case WM_DESTROY:
      PostQuitMessage(state != nullptr && state->context_probe && !state->probe_passed ? 1 : 0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

constexpr wchar_t kSyntheticFixtureTitle[] = L"Ziliu TEST ONLY - local input fields";
constexpr wchar_t kEdgeCaptionSuffix[] = L" - Microsoft Edge";
// Observed in the host Edge window caption; do not normalize arbitrary title text.
constexpr wchar_t kEdgeCaptionZeroWidthSuffix[] = L" - Microsoft\u200B Edge";
constexpr DWORD kUiaTimeoutMs = 250;
constexpr ULONGLONG kUiaMaximumDurationMs = 60'000;
constexpr ULONGLONG kUiaSamplePeriodMs = 500;
constexpr ULONGLONG kUiaMaximumLatenessMs = 250;
constexpr unsigned int kUiaMaximumSamples = 120;
constexpr std::size_t kUiaMaximumRuntimeIdItems = 32;
constexpr std::size_t kUiaMaximumOutputBytes = 256 * 1024;

struct UiaOutput final {
  explicit UiaOutput(HANDLE value) : handle(value) {}
  ~UiaOutput() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::size_t bytes_written = 0;
  bool first_sample = true;
  bool failed = false;

  bool Write(std::string_view text) {
    if (failed || bytes_written + text.size() > kUiaMaximumOutputBytes) { failed = true; return false; }
    std::size_t offset = 0;
    while (offset < text.size()) {
      DWORD written = 0;
      const DWORD remaining = static_cast<DWORD>(text.size() - offset);
      if (!WriteFile(handle, text.data() + offset, remaining, &written, nullptr) || written == 0) {
        failed = true;
        return false;
      }
      offset += written;
      bytes_written += written;
    }
    if (!FlushFileBuffers(handle)) failed = true;
    return !failed;
  }
};

struct UiaRuntimeId final {
  std::array<LONG, kUiaMaximumRuntimeIdItems> values{};
  ULONG count = 0;
};

std::string UiaHresult(HRESULT value) {
  std::ostringstream text;
  text << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
       << static_cast<unsigned long>(value);
  return text.str();
}

bool MatchesFixtureCaption(std::wstring_view caption) {
  const std::wstring_view fixture(kSyntheticFixtureTitle);
  if (caption == fixture) return true;
  if (!caption.starts_with(fixture) || caption.size() <= fixture.size()) return false;
  const auto suffix = caption.substr(fixture.size());
  for (const std::wstring_view edge_suffix :
       {std::wstring_view(kEdgeCaptionSuffix), std::wstring_view(kEdgeCaptionZeroWidthSuffix)}) {
    if (suffix == edge_suffix) return true;
    if (suffix.size() <= 3 + edge_suffix.size() || !suffix.starts_with(L" - ") ||
        !suffix.ends_with(edge_suffix)) continue;
    const auto profile = suffix.substr(3, suffix.size() - 3 - edge_suffix.size());
    if (profile.empty() || profile.size() > 64) continue;
    if (std::all_of(profile.begin(), profile.end(), [](wchar_t ch) {
          return ch >= 32 && ch != 127;
        })) return true;
  }
  return false;
}

bool IsExactFixtureCaption(HWND window) {
  if (window == nullptr) return false;
  wchar_t title[256]{};
  const int length = GetWindowTextW(window, title, static_cast<int>(std::size(title)));
  if (length <= 0 || length >= static_cast<int>(std::size(title)) - 1) return false;
  // This is a sampling opt-in label only, never URL proof or field identity.
  return MatchesFixtureCaption({title, static_cast<std::size_t>(length)});
}

bool IsExpectedEdgeImage(DWORD process_id) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) return false;
  std::array<wchar_t, 32768> image{};
  DWORD count = static_cast<DWORD>(image.size());
  const bool queried = QueryFullProcessImageNameW(process, 0, image.data(), &count) != FALSE;
  CloseHandle(process);
  if (!queried || count == 0 || count >= image.size()) return false;
  const std::filesystem::path actual(std::wstring_view(image.data(), count));
  if (_wcsicmp(actual.filename().c_str(), L"msedge.exe") != 0) return false;
  std::array<wchar_t, 32768> program_files{};
  for (const wchar_t* variable : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
    const DWORD length = GetEnvironmentVariableW(variable, program_files.data(),
                                                  static_cast<DWORD>(program_files.size()));
    if (length == 0 || length >= program_files.size()) continue;
    const auto expected = std::filesystem::path(program_files.data()) /
                          L"Microsoft" / L"Edge" / L"Application" / L"msedge.exe";
    if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL)
      return true;
  }
  return false;
}

bool ReadRuntimeId(IUIAutomationElement* element, UiaRuntimeId* output, HRESULT* result) {
  SAFEARRAY* raw = nullptr;
  *result = element->GetRuntimeId(&raw);
  struct SafeArrayOwner final {
    SAFEARRAY* value;
    ~SafeArrayOwner() { if (value != nullptr) SafeArrayDestroy(value); }
  } owner{raw};
  if (*result != S_OK || raw == nullptr || SafeArrayGetDim(raw) != 1) { *result = E_INVALIDARG; return false; }
  VARTYPE type = VT_EMPTY;
  LONG lower = 0;
  LONG upper = -1;
  if (FAILED(SafeArrayGetVartype(raw, &type)) || type != VT_I4 ||
      FAILED(SafeArrayGetLBound(raw, 1, &lower)) || FAILED(SafeArrayGetUBound(raw, 1, &upper)) ||
      upper < lower || static_cast<unsigned long long>(static_cast<long long>(upper) - lower) + 1 > kUiaMaximumRuntimeIdItems) {
    *result = E_INVALIDARG;
    return false;
  }
  LONG* values = nullptr;
  *result = SafeArrayAccessData(raw, reinterpret_cast<void**>(&values));
  if (FAILED(*result) || values == nullptr) return false;
  output->count = static_cast<ULONG>(static_cast<long long>(upper) - lower + 1);
  std::copy_n(values, output->count, output->values.begin());
  const HRESULT unaccess = SafeArrayUnaccessData(raw);
  if (FAILED(unaccess)) { *result = unaccess; output->count = 0; return false; }
  return true;
}

std::string UiaSampleJson(ULONGLONG elapsed_ms, const char* status, DWORD foreground_pid,
                          DWORD element_pid, HRESULT focus_hr, HRESULT pid_hr,
                          HRESULT password_hr, HRESULT control_hr, HRESULT focusable_hr,
                          HRESULT has_focus_hr, HRESULT runtime_id_hr, CONTROLTYPEID control_type,
                          BOOL password, BOOL focusable, BOOL has_focus,
                          const UiaRuntimeId& runtime_id, bool foreground_stable) {
  std::ostringstream json;
  FILETIME utc{};
  GetSystemTimeAsFileTime(&utc);
  const ULONGLONG utc_ticks = (static_cast<ULONGLONG>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
  json << "{\"elapsedMs\":" << elapsed_ms
       << ",\"recordUtcMs\":" << ((utc_ticks - 116444736000000000ull) / 10000)
       << ",\"status\":\"" << status
       << "\",\"foregroundPid\":" << foreground_pid << ",\"elementPid\":" << element_pid
       << ",\"foregroundStable\":" << (foreground_stable ? "true" : "false")
       << ",\"getFocusedElementHresult\":\"" << UiaHresult(focus_hr)
       << "\",\"getProcessIdHresult\":\"" << UiaHresult(pid_hr)
       << "\",\"isPasswordHresult\":\"" << UiaHresult(password_hr)
       << "\",\"controlTypeHresult\":\"" << UiaHresult(control_hr)
       << "\",\"isKeyboardFocusableHresult\":\"" << UiaHresult(focusable_hr)
       << "\",\"hasKeyboardFocusHresult\":\"" << UiaHresult(has_focus_hr)
       << "\",\"runtimeIdHresult\":\"" << UiaHresult(runtime_id_hr)
       << "\",\"controlType\":" << control_type << ",\"isPassword\":"
       << (password ? "true" : "false") << ",\"isKeyboardFocusable\":"
       << (focusable ? "true" : "false") << ",\"hasKeyboardFocus\":"
       << (has_focus ? "true" : "false") << ",\"runtimeId\":[";
  for (ULONG i = 0; i < runtime_id.count; ++i) {
    if (i != 0) json << ',';
    json << runtime_id.values[i];
  }
  json << "]}";
  return json.str();
}

int RunUiaFocusProbe(const std::filesystem::path& output_path) {
  if (!output_path.is_absolute()) return 2;
  HANDLE raw_output = CreateFileW(output_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw_output == INVALID_HANDLE_VALUE) return 2;
  UiaOutput output(raw_output);
  if (!output.Write("{\"testOnly\":true,\"bodyRead\":false,\"productionFieldIdentityVerified\":false,\"mode\":\"uia-focus-probe\",\"scope\":\"synthetic-fixture-metadata-only\",\"samples\":["))
    return 1;

  const HRESULT initialize = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialize)) {
    static_cast<void>(output.Write("],\"terminalStatus\":\"com_initialization_failed\"}"));
    return 1;
  }
  Microsoft::WRL::ComPtr<IUIAutomation2> automation;
  const HRESULT create = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(automation.GetAddressOf()));
  HRESULT connection_timeout = E_ABORT;
  HRESULT transaction_timeout = E_ABORT;
  if (create == S_OK && automation) {
    connection_timeout = automation->put_ConnectionTimeout(kUiaTimeoutMs);
    transaction_timeout = automation->put_TransactionTimeout(kUiaTimeoutMs);
  }
  if (create != S_OK || !automation || connection_timeout != S_OK || transaction_timeout != S_OK) {
    static_cast<void>(output.Write("],\"terminalStatus\":\"uia_timeout_setup_failed\",\"createHresult\":\"" +
        UiaHresult(create) + "\",\"connectionTimeoutHresult\":\"" + UiaHresult(connection_timeout) +
        "\",\"transactionTimeoutHresult\":\"" + UiaHresult(transaction_timeout) + "\"}"));
    automation.Reset();
    CoUninitialize();
    return 1;
  }

  const ULONGLONG start = GetTickCount64();
  const ULONGLONG deadline = start + kUiaMaximumDurationMs;
  unsigned int sample_count = 0;
  unsigned int captured_count = 0;
  const char* terminal = "sample_limit";
  while (sample_count < kUiaMaximumSamples) {
    const ULONGLONG scheduled = start + sample_count * kUiaSamplePeriodMs;
    ULONGLONG now = GetTickCount64();
    if (now >= deadline) { terminal = "deadline"; break; }
    if (now < scheduled) { Sleep(static_cast<DWORD>(scheduled - now)); now = GetTickCount64(); }
    if (now > scheduled + kUiaMaximumLatenessMs) { terminal = "sample_late"; break; }

    const ULONGLONG elapsed = now - start;
    HWND foreground = GetForegroundWindow();
    DWORD foreground_pid = 0;
    if (foreground != nullptr) GetWindowThreadProcessId(foreground, &foreground_pid);
    const bool fixture_caption = IsExactFixtureCaption(foreground);
    const bool edge_image = fixture_caption && IsExpectedEdgeImage(foreground_pid);
    HRESULT focus_hr = E_ABORT, pid_hr = E_ABORT, password_hr = E_ABORT;
    HRESULT control_hr = E_ABORT, focusable_hr = E_ABORT, has_focus_hr = E_ABORT;
    HRESULT runtime_id_hr = E_ABORT;
    DWORD element_pid = 0;
    CONTROLTYPEID control_type = 0;
    BOOL password = FALSE, focusable = FALSE, has_focus = FALSE;
    UiaRuntimeId runtime_id;
    const char* status = !fixture_caption ? "foreground_not_fixture" :
        !edge_image ? "foreground_not_verified_edge" : "uia_not_read";
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
    if (edge_image) {
      focus_hr = automation->GetFocusedElement(element.GetAddressOf());
      if (focus_hr == S_OK && element) {
        int current_pid = 0;
        pid_hr = element->get_CurrentProcessId(&current_pid);
        if (pid_hr == S_OK && current_pid > 0) element_pid = static_cast<DWORD>(current_pid);
        if (pid_hr != S_OK) status = "element_process_read_failed";
        else if (element_pid != foreground_pid) { status = "pid_mismatch"; element_pid = 0; }
        else {
          password_hr = element->get_CurrentIsPassword(&password);
          if (password_hr != S_OK) status = "password_state_unavailable";
          else if (password) status = "password_control_blocked";
          else {
            control_hr = element->get_CurrentControlType(&control_type);
            focusable_hr = element->get_CurrentIsKeyboardFocusable(&focusable);
            has_focus_hr = element->get_CurrentHasKeyboardFocus(&has_focus);
            if (control_hr != S_OK || focusable_hr != S_OK || has_focus_hr != S_OK)
              status = "element_metadata_read_failed";
            else if (control_type != UIA_EditControlTypeId) status = "not_edit_control";
            else if (!focusable) status = "not_keyboard_focusable";
            else if (!has_focus) status = "element_not_focused";
            else if (ReadRuntimeId(element.Get(), &runtime_id, &runtime_id_hr)) status = "captured";
            else status = "runtime_id_read_failed";
          }
        }
      } else status = "focused_element_read_failed";
    }
    if (runtime_id.count != 0) {
      Microsoft::WRL::ComPtr<IUIAutomationElement> fresh;
      BOOL same = FALSE;
      if (automation->GetFocusedElement(&fresh) != S_OK || !fresh ||
          automation->CompareElements(element.Get(), fresh.Get(), &same) != S_OK || same != TRUE) {
        status = "focused_element_changed";
        runtime_id.count = 0;
      }
    }
    DWORD recheck_pid = 0;
    if (foreground != nullptr) GetWindowThreadProcessId(foreground, &recheck_pid);
    const bool foreground_stable = foreground != nullptr && GetForegroundWindow() == foreground &&
        recheck_pid == foreground_pid && IsExactFixtureCaption(foreground) &&
        IsExpectedEdgeImage(recheck_pid) && GetForegroundWindow() == foreground;
    if (!foreground_stable && edge_image) {
      status = "foreground_changed";
      runtime_id.count = 0;
    }
    if (!edge_image || !foreground_stable) {
      foreground_pid = 0;
      element_pid = 0;
      control_type = 0;
      password = focusable = has_focus = FALSE;
    }
    std::string record = UiaSampleJson(elapsed, status, foreground_pid, element_pid,
        focus_hr, pid_hr, password_hr, control_hr, focusable_hr, has_focus_hr,
        runtime_id_hr, control_type, password, focusable, has_focus, runtime_id, foreground_stable);
    if (!output.first_sample) record.insert(0, ",");
    record.push_back('\n');
    output.first_sample = false;
    if (!output.Write(record)) { terminal = "output_failed"; break; }
    if (std::string_view(status) == "captured") ++captured_count;
    ++sample_count;
    now = GetTickCount64();
    if (now >= scheduled + kUiaSamplePeriodMs + kUiaMaximumLatenessMs) {
      terminal = now >= deadline ? "deadline" : "sample_late";
      break;
    }
  }
  const std::string final_record = "],\"terminalStatus\":\"" + std::string(terminal) +
      "\",\"sampleCount\":" + std::to_string(sample_count) +
      ",\"capturedCount\":" + std::to_string(captured_count) + "}";
  const bool finalized = output.Write(final_record);
  automation.Reset();
  CoUninitialize();
  return finalized && captured_count != 0 &&
      (std::string_view(terminal) == "sample_limit" || std::string_view(terminal) == "deadline") ? 0 : 1;
}

int UiaProbeSelfTest() {
  // Pure local checks: no COM initialization, foreground query, or UIA call.
  if (!MatchesFixtureCaption(kSyntheticFixtureTitle) ||
      !MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - Microsoft Edge") ||
      !MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - 個人 - Microsoft Edge") ||
      !MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - Microsoft\u200B Edge") ||
      !MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - 個人 - Microsoft\u200B Edge") ||
      MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - Microsoft\u200B Edge extra") ||
      MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - Microsoft\u200C Edge") ||
      MatchesFixtureCaption(L"Ziliu TEST ONLY - local input\u200B fields - Microsoft Edge") ||
      MatchesFixtureCaption(L"unrelated - Microsoft\u200B Edge") ||
      MatchesFixtureCaption(L"unrelated - Microsoft Edge") ||
      MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields extra - Microsoft Edge") ||
      MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - Microsoft Edge extra") ||
      MatchesFixtureCaption(L"Ziliu TEST ONLY - local input fields - \n - Microsoft Edge")) return 1;
  const auto json = UiaSampleJson(0, "not_captured", 0, 0, E_ABORT, E_ABORT, E_ABORT,
      E_ABORT, E_ABORT, E_ABORT, E_ABORT, 0, FALSE, FALSE, FALSE, {}, false);
  return json.find("\"runtimeId\":[]") != std::string::npos &&
      json.find("\"foregroundPid\":0") != std::string::npos &&
      json.find("\"getFocusedElementHresult\":\"0x80004004\"") != std::string::npos ? 0 : 1;
}

std::optional<Arguments> ParseArguments() {
  int count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (arguments == nullptr) return std::nullopt;
  std::optional<Arguments> parsed;
  if (count == 2 && std::wstring_view(arguments[1]) == L"--uia-probe-self-test") {
    parsed = Arguments{.mode = Mode::kUiaSelfTest, .output_path = {}};
  } else if (count == 4 && std::wstring_view(arguments[1]) == L"--uia-focus-probe" &&
             std::wstring_view(arguments[2]) == L"--output") {
    const std::filesystem::path candidate(arguments[3]);
    if (candidate.is_absolute()) parsed = Arguments{Mode::kUiaFocusProbe, candidate};
  }
  if (count == 3 && (std::wstring_view(arguments[1]) == L"--output" ||
                     std::wstring_view(arguments[1]) == L"--audit-registration" ||
                     std::wstring_view(arguments[1]) == L"--context-probe")) {
    std::filesystem::path candidate(arguments[2]);
    if (candidate.is_absolute()) {
      const std::wstring_view mode(arguments[1]);
      parsed = Arguments{.mode = mode == L"--output" ? Mode::kFields
              : mode == L"--audit-registration" ? Mode::kAuditRegistration : Mode::kContextProbe,
          .output_path = candidate};
    }
  }
  LocalFree(arguments);
  return parsed;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  const std::optional<Arguments> arguments = ParseArguments();
  if (!arguments.has_value()) {
    return 2;
  }
  if (arguments->mode == Mode::kUiaSelfTest) return UiaProbeSelfTest();
  if (arguments->mode == Mode::kUiaFocusProbe) return RunUiaFocusProbe(arguments->output_path);
  if (arguments->mode == Mode::kAuditRegistration) {
    std::wstring error;
    return AuditRegistration(arguments->output_path, &error) ? 0 : 1;
  }
  const HRESULT initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialize_result)) {
    MessageBoxW(nullptr, L"The TEST ONLY InputScope host requires COM initialization on its UI thread.",
                kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
  }
  AppState state{.output_path = arguments->output_path};
  state.context_probe = arguments->mode == Mode::kContextProbe;
  state.rich_edit_module = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (state.rich_edit_module == nullptr) {
    CoUninitialize();
    return 1;
  }
  state.thread_manager_create = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(&state.thread_manager));
  if (SUCCEEDED(state.thread_manager_create)) {
    state.thread_manager_activate = state.thread_manager->Activate(&state.client_id);
  }
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassW(&window_class) == 0) {
    if (SUCCEEDED(state.thread_manager_activate)) state.thread_manager->Deactivate();
    if (state.thread_manager != nullptr) state.thread_manager->Release();
    FreeLibrary(state.rich_edit_module);
    CoUninitialize();
    return 1;
  }
  HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 690, 350, nullptr, nullptr, instance, &state);
  if (window == nullptr) {
    if (SUCCEEDED(state.thread_manager_activate)) state.thread_manager->Deactivate();
    if (state.thread_manager != nullptr) state.thread_manager->Release();
    FreeLibrary(state.rich_edit_module);
    CoUninitialize();
    return 1;
  }
  ShowWindow(window, show_command);
  if (state.context_probe) static_cast<void>(ScheduleProbe(window, &state));
  MSG message{};
  BOOL message_result = 0;
  while ((message_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  for (ProbeIdentity& identity : state.probe_identities) {
    if (identity.canonical != nullptr) identity.canonical->Release();
    identity.canonical = nullptr;
  }
  if (SUCCEEDED(state.thread_manager_activate)) state.thread_manager->Deactivate();
  if (state.thread_manager != nullptr) state.thread_manager->Release();
  FreeLibrary(state.rich_edit_module);
  CoUninitialize();
  return message_result == 0 ? static_cast<int>(message.wParam) : 1;
}
