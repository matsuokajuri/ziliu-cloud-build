#pragma once

// TEST ONLY. Included by the TIP only under ZILIU_CONTEXT_METADATA_PROBE.
// No document text, key values, raw COM pointers, or arbitrary titles are logged.
#include "input_privacy.h"
#include "../include/ziliu/tsf/module_state.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <string_view>

namespace ziliu::tsf::detail {

struct ScopeMetadata {
  HRESULT selection = E_PENDING;
  HRESULT collapse = E_PENDING;
  HRESULT property = E_PENDING;
  HRESULT value = E_PENDING;
  HRESULT query_scope = E_PENDING;
  HRESULT enumeration = E_PENDING;
  VARTYPE variant_type = VT_EMPTY;
  UINT scope_count = 0;
  bool scope_values_present = false;
  std::array<int, 8> scopes{};
};

enum class ContextProbeHost : int {
  kEdge = 0,
  kChrome = 1,
  kNotepad = 2,
  kCode = 3,
};

inline ScopeMetadata ReadScopeMetadata(ITfContext* context, TfEditCookie cookie);
inline bool HasStrictOrdinaryProbeScope(ITfContext* context, TfEditCookie cookie);
inline DWORD CurrentForegroundProcessId() {
  DWORD process_id = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process_id);
  return process_id;
}

// Synthetic-fixture-only probe result. It deliberately contains no field text,
// digest, or inferred field identity. S_FALSE/partial reads remain distinguishable
// from an empty result through read_hr and count.
struct ContextPrefixProbeResult {
  bool attempted = false;
  HRESULT selection_hr = E_PENDING;
  HRESULT collapse_hr = E_PENDING;
  HRESULT shift_hr = E_PENDING;
  HRESULT read_hr = E_PENDING;
  ULONG count = 0;
  bool match_a = false;
  bool match_b = false;
  bool body_read = false;
};

inline ContextPrefixProbeResult ReadSyntheticContextPrefix(
    ITfContext* context, TfEditCookie cookie, ContextProbeHost host, int field,
    DWORD own_pid, DWORD (*foreground_pid_provider)() = CurrentForegroundProcessId,
    bool edge_fixture_opt_in = false) {
  ContextPrefixProbeResult result;
  const bool allowed_host = host == ContextProbeHost::kNotepad ||
      host == ContextProbeHost::kCode ||
      (host == ContextProbeHost::kEdge && edge_fixture_opt_in);
  if (!allowed_host ||
      (field != 0 && field != 1) || own_pid == 0 || foreground_pid_provider == nullptr ||
      foreground_pid_provider() != own_pid ||
      ContextBlocksInput(context, 0) || ClassifyScope(context, cookie) != InputPrivacy::kOrdinary ||
      !HasStrictOrdinaryProbeScope(context, cookie)) {
    return result;
  }

  result.attempted = true;
  TF_SELECTION selection{};
  ULONG fetched = 0;
  result.selection_hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1,
                                               &selection, &fetched);
  Microsoft::WRL::ComPtr<ITfRange> range;
  range.Attach(selection.range);
  if (result.selection_hr != S_OK || fetched != 1 || !range ||
      selection.style.fInterimChar != FALSE) return result;
  if (selection.style.ase == TF_AE_NONE) {
    BOOL empty = FALSE;
    if (range->IsEmpty(cookie, &empty) != S_OK || empty == FALSE) return result;
  } else if (selection.style.ase != TF_AE_START && selection.style.ase != TF_AE_END) {
    return result;
  }

  // Always collapse to the selection START: selected text is excluded regardless
  // of active-end direction. This synthetic oracle is not general field identity.
  result.collapse_hr = range->Collapse(cookie, TF_ANCHOR_START);
  if (result.collapse_hr != S_OK) return result;
  LONG shifted = 0;
  result.shift_hr = range->ShiftStart(cookie, -128, &shifted, nullptr);
  if (result.shift_hr != S_OK || shifted < -128 || shifted > 0) return result;

  constexpr std::wstring_view expected_a =
      L"Ziliu synthetic context probe A. No personal information.";
  constexpr std::wstring_view expected_b =
      L"Ziliu synthetic context probe B. No personal information.";
  // Recheck immediately before the only text-retrieval call; focus may have
  // changed while the TSF range and property were being queried.
  if (foreground_pid_provider() != own_pid) return result;
  std::array<WCHAR, 128> buffer{};
  ULONG count = 0;
  result.body_read = true;
  result.read_hr = range->GetText(cookie, 0, buffer.data(),
                                  static_cast<ULONG>(buffer.size()), &count);
  result.count = count;
  // Only a complete, exact S_OK read is comparable. S_FALSE and partial reads
  // are not interpreted as empty or as a match.
  if (result.read_hr == S_OK && count <= buffer.size()) {
    result.match_a = count == expected_a.size() &&
        std::equal(expected_a.begin(), expected_a.end(), buffer.begin());
    result.match_b = count == expected_b.size() &&
        std::equal(expected_b.begin(), expected_b.end(), buffer.begin());
  }
  SecureZeroMemory(buffer.data(), sizeof(buffer));
  return result;
}

struct ContextProbeTarget {
  ContextProbeHost host;
  int field;
};

// Test label only, NOT a production field identity or trusted browser origin.
// Edge text access also requires the separate short-lived local opt-in.
inline bool IsEdgeSyntheticPrefixFixture(
    ContextProbeHost host, int field, std::wstring_view title) {
  if (host != ContextProbeHost::kEdge || field < 0 || field > 1) return false;
  constexpr std::array titles{
      std::wstring_view{L"Ziliu SYNTHETIC PREFIX [ordinary]"},
      std::wstring_view{L"Ziliu SYNTHETIC PREFIX [ordinarySecond]"}};
  const auto expected = titles[static_cast<std::size_t>(field)];
  return title.starts_with(expected) &&
      (title.size() == expected.size() || title[expected.size()] == L' ');
}

// Exact, synthetic fixture allowlist. Browser fixture titles retain the known
// prefix convention; editor titles must begin with one of the two fixed names.
inline std::optional<ContextProbeTarget> MatchContextProbeTarget(
    std::wstring_view executable, std::wstring_view title) {
  const auto equals_ascii_case_insensitive = [](std::wstring_view left,
                                                std::wstring_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
      wchar_t a = left[i];
      wchar_t b = right[i];
      if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
      if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
      if (a != b) return false;
    }
    return true;
  };
  ContextProbeHost host;
  if (equals_ascii_case_insensitive(executable, L"msedge.exe")) {
    host = ContextProbeHost::kEdge;
  } else if (equals_ascii_case_insensitive(executable, L"chrome.exe")) {
    host = ContextProbeHost::kChrome;
  } else if (equals_ascii_case_insensitive(executable, L"notepad.exe")) {
    host = ContextProbeHost::kNotepad;
  } else if (equals_ascii_case_insensitive(executable, L"code.exe")) {
    host = ContextProbeHost::kCode;
  } else {
    return std::nullopt;
  }

  constexpr std::array browser_titles{
      std::wstring_view{L"Ziliu CONTEXT METADATA [ordinary]"},
      std::wstring_view{L"Ziliu CONTEXT METADATA [ordinarySecond]"},
      std::wstring_view{L"Ziliu CONTEXT METADATA [password]"},
      std::wstring_view{L"Ziliu CONTEXT METADATA [pinLike]"}};
  if (host == ContextProbeHost::kEdge || host == ContextProbeHost::kChrome) {
    for (int field = 0; field < 2; ++field) {
      if (IsEdgeSyntheticPrefixFixture(host, field, title)) {
        return ContextProbeTarget{host, field};
      }
    }
    for (std::size_t i = 0; i < browser_titles.size(); ++i) {
      if (title.starts_with(browser_titles[i])) {
        return ContextProbeTarget{host, static_cast<int>(i)};
      }
    }
    return std::nullopt;
  }

  // Windows editor titles may mark unsaved documents with '*'; Code may also
  // prepend its explicit modified-document dot/bullet. Never search substrings.
  if (!title.empty() && title.front() == L'*') title.remove_prefix(1);
  if (host == ContextProbeHost::kCode && !title.empty() &&
      (title.front() == L'•' || title.front() == L'●')) title.remove_prefix(1);
  while (!title.empty() && (title.front() == L' ' || title.front() == L'\t')) {
    title.remove_prefix(1);
  }
  constexpr std::array editor_titles{L"ZiliuContextProbe-A.txt", L"ZiliuContextProbe-B.txt"};
  for (std::size_t i = 0; i < editor_titles.size(); ++i) {
    const std::wstring_view filename{editor_titles[i]};
    if (title.starts_with(filename) &&
        (title.size() == filename.size() || title[filename.size()] == L' ')) {
      return ContextProbeTarget{host, static_cast<int>(i)};
    }
  }
  return std::nullopt;
}

inline ScopeMetadata ReadScopeMetadata(ITfContext* context, TfEditCookie cookie) {
  ScopeMetadata result;
  if (context == nullptr) return result;
  TF_SELECTION selection{};
  ULONG fetched = 0;
  result.selection = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
  Microsoft::WRL::ComPtr<ITfRange> range;
  range.Attach(selection.range);
  if (result.selection != S_OK || fetched != 1 || !range || selection.style.fInterimChar) return result;
  if (selection.style.ase == TF_AE_NONE) {
    BOOL empty = FALSE;
    if (range->IsEmpty(cookie, &empty) != S_OK || !empty) return result;
  } else if (selection.style.ase != TF_AE_START && selection.style.ase != TF_AE_END) {
    return result;
  }
  result.collapse = range->Collapse(cookie, selection.style.ase == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END);
  if (result.collapse != S_OK) return result;
  Microsoft::WRL::ComPtr<ITfReadOnlyProperty> property;
  result.property = context->GetAppProperty(GUID_PROP_INPUTSCOPE, property.GetAddressOf());
  if (result.property != S_OK || !property) return result;
  VARIANT value;
  VariantInit(&value);
  result.value = property->GetValue(cookie, range.Get(), &value);
  result.variant_type = value.vt;
  if (result.value == S_OK && value.vt == VT_UNKNOWN && value.punkVal) {
    Microsoft::WRL::ComPtr<ITfInputScope> scope;
    result.query_scope = value.punkVal->QueryInterface(IID_PPV_ARGS(&scope));
    if (result.query_scope == S_OK && scope) {
      InputScope* values = nullptr;
      result.enumeration = scope->GetInputScopes(&values, &result.scope_count);
      if (result.enumeration == S_OK && values) {
        result.scope_values_present = true;
        for (UINT i = 0; i < result.scope_count && i < result.scopes.size(); ++i) {
          result.scopes[i] = static_cast<int>(values[i]);
        }
      }
      CoTaskMemFree(values);
    }
  }
  VariantClear(&value);
  return result;
}

inline bool HasStrictOrdinaryProbeScope(ITfContext* context, TfEditCookie cookie) {
  const ScopeMetadata metadata = ReadScopeMetadata(context, cookie);
  if (metadata.selection != S_OK || metadata.collapse != S_OK || metadata.property != S_OK ||
      metadata.value != S_OK || metadata.variant_type != VT_UNKNOWN ||
      metadata.query_scope != S_OK || metadata.enumeration != S_OK ||
      !metadata.scope_values_present || metadata.scope_count == 0 ||
      metadata.scope_count > metadata.scopes.size()) return false;
  for (UINT i = 0; i < metadata.scope_count; ++i) {
    if (!IsKnownOrdinaryScope(static_cast<InputScope>(metadata.scopes[i]))) return false;
  }
  return true;
}

class ContextMetadataProbe {
 public:
  void Reset() {
    identities_ = {};
    identity_epoch_ = epochs_.fetch_add(1) + 1;
    last_field_ = -1;
    last_context_ = 0;
  }

  void Observe(ITfContext* context, TfEditCookie cookie) {
    // Three independent opt-ins: special build, Edge fixture foreground, and
    // a marker beside this diagnostic DLL. No directory is created by the TIP.
    if (records_.load() >= 32) return;
    wchar_t executable[MAX_PATH]{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (executable_length == 0 || executable_length >= MAX_PATH) return;
    const auto* base = std::wcsrchr(executable, L'\\');
    if (!base) return;
    wchar_t title[160]{};
    const HWND foreground = GetForegroundWindow();
    if (GetWindowTextW(foreground, title, 160) == 0) return;
    const auto target = MatchContextProbeTarget(base + 1, title);
    if (!target) return;
    const int field = target->field;
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(ModuleInstance(), directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    auto* slash = std::wcsrchr(directory, L'\\');
    if (!slash) return;
    *slash = L'\0';
    wchar_t path[MAX_PATH]{};
    if (swprintf_s(path, L"%s\\context-probe.enabled", directory) < 0 ||
        GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> document;
    const HRESULT document_result = context->GetDocumentMgr(document.GetAddressOf());
    const auto context_id = Identity(context);
    const auto document_id = Identity(document.Get());
    const bool editor_prefix_enabled =
        (target->host == ContextProbeHost::kNotepad || target->host == ContextProbeHost::kCode) &&
        field >= 0 && field <= 1 &&
        swprintf_s(path, L"%s\\context-prefix-probe.enabled", directory) >= 0 &&
        GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    const bool edge_prefix_enabled =
        IsEdgeSyntheticPrefixFixture(target->host, field, title) &&
        swprintf_s(path, L"%s\\context-edge-prefix-probe.enabled", directory) >= 0 &&
        GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    const bool prefix_enabled = editor_prefix_enabled || edge_prefix_enabled;
    const bool same_metadata_observation =
        field == last_field_ && context_id == last_context_ && context_id != 0;
    if (same_metadata_observation && !prefix_enabled) return;
    const ScopeMetadata data = ReadScopeMetadata(context, cookie);
    // Reserve one shared observation slot before any opted-in text access. Prefix
    // mode deliberately skips metadata dedup so repeated same-context edits remain
    // observable, but all output still fits the original 32-record ceiling.
    const unsigned record = records_.fetch_add(1);
    if (record >= 32) return;
    ContextPrefixProbeResult prefix;
    if (prefix_enabled) {
      prefix = ReadSyntheticContextPrefix(
          context, cookie, target->host, field, GetCurrentProcessId(),
          CurrentForegroundProcessId, edge_prefix_enabled);
    }
    // Fixed field ordinals only; this is not proof that a TSF context uniquely
    // identifies a DOM input. The experiment explicitly tests that assumption.
    char line[1024]{};
    const auto format_metadata = [&](bool include_prefix) {
      const char* metadata_format =
          "{\"record\":%u,\"pid\":%lu,\"tid\":%lu,\"identity_epoch\":%u,\"host\":%d,\"field\":%d,\"context\":%u,\"document\":%u,"
          "\"document_hr\":%ld,\"selection_hr\":%ld,\"collapse_hr\":%ld,\"property_hr\":%ld,"
          "\"value_hr\":%ld,\"vt\":%u,\"scope_qi_hr\":%ld,\"scope_enum_hr\":%ld,\"scope_count\":%u,"
          "\"scopes\":[%d,%d,%d,%d,%d,%d,%d,%d],\"body_read\":false}\n";
      const char* prefix_format =
          "{\"record\":%u,\"pid\":%lu,\"tid\":%lu,\"identity_epoch\":%u,\"host\":%d,\"field\":%d,\"context\":%u,\"document\":%u,"
          "\"document_hr\":%ld,\"selection_hr\":%ld,\"collapse_hr\":%ld,\"property_hr\":%ld,"
          "\"value_hr\":%ld,\"vt\":%u,\"scope_qi_hr\":%ld,\"scope_enum_hr\":%ld,\"scope_count\":%u,"
          "\"scopes\":[%d,%d,%d,%d,%d,%d,%d,%d],\"attempted\":%s,"
          "\"prefix_selection_hr\":%ld,\"prefix_collapse_hr\":%ld,"
          "\"prefix_shift_hr\":%ld,\"read_hr\":%ld,"
          "\"count\":%lu,\"match_a\":%s,\"match_b\":%s,\"body_read\":%s}\n";
      const char* format = include_prefix ? prefix_format : metadata_format;
      if (include_prefix) {
        return sprintf_s(line, format,
            record, GetCurrentProcessId(), GetCurrentThreadId(), identity_epoch_,
            static_cast<int>(target->host), field, context_id, document_id,
            document_result, data.selection, data.collapse, data.property, data.value,
            static_cast<unsigned>(data.variant_type), data.query_scope, data.enumeration,
            data.scope_count, data.scopes[0], data.scopes[1], data.scopes[2], data.scopes[3],
            data.scopes[4], data.scopes[5], data.scopes[6], data.scopes[7],
            prefix.attempted ? "true" : "false", prefix.selection_hr, prefix.collapse_hr,
            prefix.shift_hr, prefix.read_hr, prefix.count,
            prefix.match_a ? "true" : "false", prefix.match_b ? "true" : "false",
            prefix.body_read ? "true" : "false");
      }
      return sprintf_s(line, format,
          record, GetCurrentProcessId(), GetCurrentThreadId(), identity_epoch_,
          static_cast<int>(target->host), field, context_id, document_id,
          document_result, data.selection, data.collapse, data.property, data.value,
          static_cast<unsigned>(data.variant_type), data.query_scope, data.enumeration,
          data.scope_count, data.scopes[0], data.scopes[1], data.scopes[2], data.scopes[3],
          data.scopes[4], data.scopes[5], data.scopes[6], data.scopes[7]);
    };
    const int size = format_metadata(prefix_enabled);
    if (size <= 0 || swprintf_s(path, L"%s\\context-scope-%lu-%lu.jsonl", directory,
                              GetCurrentProcessId(), GetCurrentThreadId()) < 0) return;
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL saved = WriteFile(file, line, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(file);
    if (saved && written == static_cast<DWORD>(size)) {
      last_field_ = field;
      last_context_ = context_id;
    }

  }

 private:
  unsigned Identity(IUnknown* object) {
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
    for (std::size_t i = 0; i < identities_.size(); ++i) {
      if (identities_[i].Get() == identity.Get()) return static_cast<unsigned>(i + 1);
      if (!identities_[i]) {
        identities_[i] = identity;
        return static_cast<unsigned>(i + 1);
      }
    }
    return 0;
  }
  inline static std::atomic<unsigned> records_{0};
  inline static std::atomic<unsigned> epochs_{0};
  unsigned identity_epoch_ = epochs_.fetch_add(1) + 1;
  std::array<Microsoft::WRL::ComPtr<IUnknown>, 16> identities_;
  int last_field_ = -1;
  unsigned last_context_ = 0;
};

}  // namespace ziliu::tsf::detail
