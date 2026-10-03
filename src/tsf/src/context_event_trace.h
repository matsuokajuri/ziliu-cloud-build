#pragma once

// TEST ONLY: fixed Edge fixture event metadata, never document text or raw titles.
#include "context_event_probe.h"
#include "context_shadow_trial.h"
#include "context_fresh_probe.h"
#include "../include/ziliu/tsf/module_state.h"

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <string_view>

namespace ziliu::tsf::detail {

inline std::optional<int> ParseContextEventStep(std::wstring_view title) noexcept {
  constexpr std::wstring_view prefix = L"Ziliu CONTEXT EVENTS [";
  if (!title.starts_with(prefix) || title.size() < prefix.size() + 2) return {};
  const auto digit = title[prefix.size()];
  if (digit < L'0' || digit > L'9' || title[prefix.size() + 1] != L']') return {};
  const auto end = prefix.size() + 2;
  if (title.size() != end && title[end] != L' ') return {};
  return static_cast<int>(digit - L'0');
}

inline bool ContextEventProbeDirectory(wchar_t (&directory)[MAX_PATH]) noexcept {
  const auto length = GetModuleFileNameW(ModuleInstance(), directory, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  auto* slash = std::wcsrchr(directory, L'\\');
  if (!slash) return false;
  *slash = L'\0';
  return true;
}

// Subscription opt-in differs from per-record eligibility: activation can occur
// before the user focuses the fixture. Only the per-record gate permits logging.
inline bool ContextEventProbeSessionEnabled() noexcept {
  wchar_t executable[MAX_PATH]{};
  const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  const auto* base = std::wcsrchr(executable, L'\\');
  if (!base || _wcsicmp(base + 1, L"msedge.exe") != 0) return false;
  wchar_t directory[MAX_PATH]{};
  wchar_t marker[MAX_PATH]{};
  if (!ContextEventProbeDirectory(directory) ||
      swprintf_s(marker, L"%s\\context-events-probe.enabled", directory) < 0) return false;
  const auto attributes = GetFileAttributesW(marker);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::atomic<unsigned> context_event_records{0};

inline bool ContextShadowSessionEnabled() noexcept {
  if (!ContextEventProbeSessionEnabled()) return false;
  wchar_t directory[MAX_PATH]{}, marker[MAX_PATH]{};
  if (!ContextEventProbeDirectory(directory) ||
      swprintf_s(marker, L"%s\\context-shadow-probe.enabled", directory) < 0) return false;
  const auto attributes = GetFileAttributesW(marker);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool ContextFreshSessionEnabled() noexcept {
  if (!ContextShadowSessionEnabled()) return false;
  wchar_t directory[MAX_PATH]{}, marker[MAX_PATH]{};
  if (!ContextEventProbeDirectory(directory) ||
      swprintf_s(marker, L"%s\\context-fresh-probe.enabled", directory) < 0) return false;
  const auto attributes = GetFileAttributesW(marker);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool ReserveContextFreshRead() noexcept {
  static std::atomic<unsigned> reads{0};
  return reads.fetch_add(1) < 32;
}

inline std::optional<int> ParseContextShadowCase(std::wstring_view title) noexcept {
  constexpr std::wstring_view prefix = L"Ziliu CONTEXT SHADOW [";
  if (!title.starts_with(prefix) || title.size() < prefix.size() + 2) return {};
  const auto digit = title[prefix.size()];
  if (digit < L'0' || digit > L'6' || title[prefix.size() + 1] != L']') return {};
  const auto end = prefix.size() + 2;
  if (title.size() != end && title[end] != L' ') return {};
  return static_cast<int>(digit - L'0');
}

inline std::optional<int> ParseContextFreshCase(std::wstring_view title) noexcept {
  constexpr std::wstring_view prefix = L"Ziliu CONTEXT FRESH [";
  if (!title.starts_with(prefix) || title.size() < prefix.size() + 2) return {};
  const auto digit = title[prefix.size()];
  if (digit < L'0' || digit > L'7' || title[prefix.size() + 1] != L']') return {};
  const auto end = prefix.size() + 2;
  if (title.size() != end && title[end] != L' ') return {};
  return static_cast<int>(digit - L'0');
}

inline std::optional<int> CurrentContextShadowCase() noexcept {
  if (!ContextShadowSessionEnabled()) return {};
  const HWND foreground = GetForegroundWindow();
  DWORD process = 0;
  GetWindowThreadProcessId(foreground, &process);
  if (process != GetCurrentProcessId()) return {};
  wchar_t title[160]{};
  if (GetWindowTextW(foreground, title, 160) == 0) return {};
  if (ContextFreshSessionEnabled()) return ParseContextFreshCase(title);
  return ParseContextShadowCase(title);
}

inline std::optional<int> CurrentContextEventStep() noexcept {
  if (context_event_records.load() >= 128 || !ContextEventProbeSessionEnabled()) return {};
  const HWND foreground = GetForegroundWindow();
  DWORD process = 0;
  GetWindowThreadProcessId(foreground, &process);
  if (process != GetCurrentProcessId()) return {};
  wchar_t title[160]{};
  if (GetWindowTextW(foreground, title, 160) == 0) return {};
  if (ContextFreshSessionEnabled()) return ParseContextFreshCase(title);
  if (ContextShadowSessionEnabled()) return ParseContextShadowCase(title);
  return ParseContextEventStep(title);
}

inline bool ContextEventProbeAllowed() noexcept {
  return CurrentContextEventStep().has_value();
}

inline bool ShouldTraceContextEvent(const ContextEvent& event) noexcept {
  // A successful poll is not a boundary. Keep failures and every real event;
  // polling still runs and retains all of its invalidation/rebinding behavior.
  return event.kind != ContextEventKind::kCheckpoint || event.hr != S_OK;
}

inline unsigned ContextKeyCategory(WPARAM key) noexcept {
  if (key >= L'A' && key <= L'Z') return 1;
  if (key == VK_ESCAPE) return 2;
  if (key == VK_BACK) return 3;
  return 0;
}

inline bool ContextKeyStateProbeAllowed() noexcept {
  if (!CurrentContextEventStep() || ContextShadowSessionEnabled()) return false;
  wchar_t directory[MAX_PATH]{}, marker[MAX_PATH]{};
  if (!ContextEventProbeDirectory(directory) ||
      swprintf_s(marker, L"%s\\context-key-state-probe.enabled", directory) < 0) return false;
  const auto attributes = GetFileAttributesW(marker);
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Separate bounded stream: no key values, document text, preedit or candidates.
// Call only after a consumed response and successful composition edit, in an
// ordinary focused context. This is diagnostic evidence, not field identity.
inline void WriteContextKeyState(WPARAM key, std::size_t preedit_length,
                                 std::size_t candidate_count) noexcept {
  const unsigned category = ContextKeyCategory(key);
  if (!category || !ContextKeyStateProbeAllowed()) return;
  static std::atomic<unsigned> records{0};
  const unsigned record = records.fetch_add(1);
  if (record >= 32) return;
  FILETIME time{};
  GetSystemTimePreciseAsFileTime(&time);
  ULARGE_INTEGER ticks{};
  ticks.LowPart = time.dwLowDateTime;
  ticks.HighPart = time.dwHighDateTime;
  char line[512]{};
  const int count = sprintf_s(line,
      "{\"record\":%u,\"pid\":%lu,\"tid\":%lu,\"utc_ms\":%llu,"
      "\"key_category\":%u,\"consumed\":true,\"edit_ok\":true,"
      "\"preedit_length\":%zu,\"candidate_count\":%zu,\"body_read\":false,"
      "\"production_field_identity_verified\":false}\n",
      record, GetCurrentProcessId(), GetCurrentThreadId(),
      static_cast<unsigned long long>(ticks.QuadPart / 10000ULL - 11644473600000ULL),
      category, preedit_length, candidate_count);
  wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
  if (count <= 0 || !ContextEventProbeDirectory(directory) ||
      swprintf_s(path, L"%s\\context-key-state-%lu-%lu.jsonl", directory,
                 GetCurrentProcessId(), GetCurrentThreadId()) < 0) return;
  const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  static_cast<void>(WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr));
  CloseHandle(file);
}

inline void WriteContextEvent(const ContextEvent& event) noexcept {
  if (!ShouldTraceContextEvent(event)) return;
  const auto step = CurrentContextEventStep();
  if (!step) return;
  const unsigned record = context_event_records.fetch_add(1);
  if (record >= 128) return;
  FILETIME time{};
  GetSystemTimePreciseAsFileTime(&time);
  ULARGE_INTEGER ticks{};
  ticks.LowPart = time.dwLowDateTime;
  ticks.HighPart = time.dwHighDateTime;
  const auto utc_ms = ticks.QuadPart / 10000ULL - 11644473600000ULL;
  char line[1024]{};
  const int count = sprintf_s(line,
      "{\"record\":%u,\"pid\":%lu,\"tid\":%lu,\"utc_ms\":%llu,\"tick_ms\":%llu,"
      "\"step\":%d,\"kind\":%u,\"identity_epoch\":%u,\"context\":%u,\"previous_context\":%u,"
      "\"document\":%u,\"previous_document\":%u,\"hr\":%ld,\"selection_hr\":%ld,"
      "\"selection_changed\":%s,\"updates_hr\":%ld,\"next_hr\":%ld,"
      "\"changed_range_present\":%s,\"body_read\":false}\n",
      record, GetCurrentProcessId(), GetCurrentThreadId(),
      static_cast<unsigned long long>(utc_ms), static_cast<unsigned long long>(GetTickCount64()),
      *step, static_cast<unsigned>(event.kind), event.identity_epoch, event.context_id,
      event.previous_context_id, event.document_id, event.previous_document_id, event.hr,
      event.selection_hr, event.selection_changed ? "true" : "false", event.updates_hr,
      event.next_hr, event.changed_range_present ? "true" : "false");
  wchar_t directory[MAX_PATH]{};
  wchar_t path[MAX_PATH]{};
  if (count <= 0 || !ContextEventProbeDirectory(directory) ||
      swprintf_s(path, L"%s\\context-events-%lu-%lu.jsonl", directory,
                 GetCurrentProcessId(), GetCurrentThreadId()) < 0) return;
  const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  static_cast<void>(WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr));
  CloseHandle(file);
}

inline void WriteContextShadowOutcome(const ShadowOutcome& outcome, bool ordinary_scope,
                                     const ContextFreshProbeResult* fresh = nullptr) noexcept {
  if (outcome.phase == 0 || !CurrentContextShadowCase()) return;
  static std::atomic<unsigned> records{0};
  const unsigned record = records.fetch_add(1);
  if (record >= 64) return;
  FILETIME time{};
  GetSystemTimePreciseAsFileTime(&time);
  ULARGE_INTEGER ticks{};
  ticks.LowPart = time.dwLowDateTime;
  ticks.HighPart = time.dwHighDateTime;
  const ContextFreshProbeResult unread{};
  const auto& read = fresh ? *fresh : unread;
  char line[1024]{};
  const int count = sprintf_s(line,
      "{\"record\":%u,\"pid\":%lu,\"tid\":%lu,\"utc_ms\":%llu,\"scenario\":%d,"
      "\"phase\":%d,\"sequence\":%u,\"accepted\":%s,\"age_ms\":%lld,"
      "\"ordinary_scope\":%s,\"synthetic_only\":true,\"body_read\":%s,"
      "\"fresh_read_required\":%s,\"fresh_read_ok\":%s,\"prefix_count\":%lu,"
      "\"request_hr\":%ld,\"session_hr\":%ld,\"selection_hr\":%ld,\"read_hr\":%ld,"
      "\"production_field_identity_verified\":false}\n",
      record, GetCurrentProcessId(), GetCurrentThreadId(),
      static_cast<unsigned long long>(ticks.QuadPart / 10000ULL - 11644473600000ULL),
      outcome.scenario, outcome.phase, outcome.sequence, outcome.accepted ? "true" : "false",
      outcome.age_ms, ordinary_scope ? "true" : "false", read.body_read ? "true" : "false",
      fresh ? "true" : "false", read.fresh_read_ok ? "true" : "false", read.count,
      read.request_hr, read.session_hr, read.selection_hr, read.read_hr);
  wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
  if (count <= 0 || !ContextEventProbeDirectory(directory) ||
      swprintf_s(path, L"%s\\context-%s-%lu-%lu.jsonl", directory, fresh ? L"fresh" : L"shadow",
                 GetCurrentProcessId(), GetCurrentThreadId()) < 0) return;
  const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  static_cast<void>(WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr));
  CloseHandle(file);
}

}  // namespace ziliu::tsf::detail
