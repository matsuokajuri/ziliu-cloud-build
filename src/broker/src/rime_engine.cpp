#include "ziliu/broker/rime_engine.h"

#include "ziliu/core/ipc_protocol.h"
#include "ziliu/core/dictionary_epoch.h"

#if defined(ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST) && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
#include "ziliu/broker/rime_source_profile_pin.h"
#include "ziliu/broker/rime_user_profile.h"
#endif

#include <windows.h>
#include <shlobj.h>

#include <rime_api.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ziliu::broker {
namespace {

constexpr int kRimeBackspace = 0xFF08;

bool EnsureInitialOverlay(const std::filesystem::path& source,
                          const std::filesystem::path& destination) {
  // These are user-owned after initial creation. Never overwrite customization
  // to obtain contextual admission (nor silently migrate an older overlay).
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(destination, status_error);
  if (status.type() != std::filesystem::file_type::not_found) {
    return !status_error && std::filesystem::is_regular_file(status);
  }
  if (status_error && status_error != std::errc::no_such_file_or_directory) return false;
  std::error_code copy_error;
  return std::filesystem::copy_file(source, destination,
                                    std::filesystem::copy_options::none,
                                    copy_error) &&
         !copy_error;
}

std::string ToUtf8(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr,
                                           nullptr);
  if (required <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), required, nullptr,
                          nullptr) != required) {
    return {};
  }
  return result;
}

std::wstring FromUtf8(const char* value) {
  if (value == nullptr || *value == '\0') {
    return {};
  }
  const int length = static_cast<int>(std::strlen(value));
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, length, nullptr, 0);
  if (required <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, length, result.data(), required) !=
      required) {
    return {};
  }
  return result;
}

std::wstring FormatPreedit(std::wstring_view preedit) {
  std::wstring result;
  result.reserve(preedit.size());
  bool pending_separator = false;
  for (const wchar_t character : preedit) {
    const bool whitespace =
        character == L' ' || character == L'\t' || character == L'\r' || character == L'\n';
    if (whitespace) {
      pending_separator = !result.empty();
      continue;
    }
    if (pending_separator && result.back() != L'\'' && character != L'\'') {
      result.push_back(L'\'');
    }
    pending_separator = false;
    result.push_back(character);
  }
  return result;
}

std::filesystem::path ExecutableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || static_cast<std::size_t>(length) >= path.size()) {
    return {};
  }
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

std::filesystem::path UserDataDirectory() {
  std::wstring override_path(32768, L'\0');
  const DWORD override_length =
      GetEnvironmentVariableW(L"ZILIU_RIME_USER_DATA_DIR", override_path.data(),
                              static_cast<DWORD>(override_path.size()));
  if (override_length > 0 && static_cast<std::size_t>(override_length) < override_path.size()) {
    override_path.resize(override_length);
    return override_path;
  }

  PWSTR local_app_data = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                  &local_app_data))) {
    return {};
  }
  const std::filesystem::path result =
      std::filesystem::path(local_app_data) / L"Ziliu" / L"Rime";
  CoTaskMemFree(local_app_data);
  return result;
}

bool HasLuaEnvironmentOverride() noexcept {
  // Presence alone is unsupported, even when the value preserves Lua defaults.
  // Do not log values (they may contain private paths), unset them, or prevent
  // ordinary Rime from honoring them. This only vetoes contextual metadata.
  for (const auto* name : {
           L"LUA_PATH", L"LUA_CPATH", L"LUA_INIT", L"LUA_PATH_5_1", L"LUA_CPATH_5_1",
           L"LUA_INIT_5_1", L"LUA_PATH_5_2", L"LUA_CPATH_5_2", L"LUA_INIT_5_2",
           L"LUA_PATH_5_3", L"LUA_CPATH_5_3", L"LUA_INIT_5_3", L"LUA_PATH_5_4",
           L"LUA_CPATH_5_4", L"LUA_INIT_5_4"}) {
    SetLastError(ERROR_SUCCESS);
    if (GetEnvironmentVariableW(name, nullptr, 0) != 0 ||
        GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
      return true;
    }
  }
  return false;
}

#if defined(ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST) && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
// The export below is intentionally specific to the independently pinned x64
// MSVC DLL. Call only after its file provenance passed. Do not infer the default
// module list from the local source checkout's build options.
bool HasPinnedModuleProfile(HMODULE library, RimeApi* api) {
  if (!RIME_API_AVAILABLE(api, find_module) || !RIME_API_AVAILABLE(api, get_version)) return false;
  const char* version = api->get_version();
  if (!version || std::strcmp(version, "1.17.0") != 0 || api->find_module("plugins")) return false;
  const auto exported = GetProcAddress(library, "?kDefaultModules@rime@@3PAPEBDA");
  if (!exported) return false;
  const auto modules = std::bit_cast<const char* const*>(exported);
  constexpr const char* expected[] = {"default", "lua", "octagram", "predict"};
  for (std::size_t index = 0; index < std::size(expected); ++index) {
    if (!modules[index] || std::strcmp(modules[index], expected[index]) != 0 ||
        !api->find_module(expected[index])) return false;
  }
  if (modules[std::size(expected)] != nullptr) return false;
  for (const auto* name : {"core", "dict", "gears", "levers", "deployer"}) {
    if (!api->find_module(name)) return false;
  }
  // dumpbin /dependents for the pinned DLL lists only these Windows imports.
  // A same-named app-local dependency must not qualify as the OS library.
  wchar_t system_directory[MAX_PATH]{};
  const UINT count = GetSystemDirectoryW(system_directory, MAX_PATH);
  if (count == 0 || count >= MAX_PATH) return false;
  for (const auto* name : {L"dbghelp.dll", L"kernel32.dll", L"user32.dll"}) {
    wchar_t loaded_path[32768]{};
    const HMODULE dependency = GetModuleHandleW(name);
    const DWORD length = dependency ? GetModuleFileNameW(dependency, loaded_path, 32768) : 0;
    const auto expected_path = (std::filesystem::path(system_directory) / name).native();
    if (length == 0 || length >= 32768 ||
        CompareStringOrdinal(loaded_path, static_cast<int>(length), expected_path.c_str(),
                             static_cast<int>(expected_path.size()), TRUE) != CSTR_EQUAL) return false;
  }
  return true;
}
#endif

class RimeRuntime final {
 public:
  // Serializes first-party entry points, not the Deployer callback. The callback
  // must remain lock-free with respect to this mutex (Rime can notify inline).
  class Operation final {
   public:
    explicit Operation(RimeRuntime& runtime)
        : runtime_(runtime), lock_(runtime.operation_mutex_),
          exceptions_(std::uncaught_exceptions()), token_(runtime.epoch_.BeginOperation()) {}
    ~Operation() {
      if (std::uncaught_exceptions() != exceptions_) runtime_.epoch_.Invalidate();
      if (!finished_) static_cast<void>(Finish());
    }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    [[nodiscard]] std::uint64_t Finish() noexcept {
      if (finished_) return 0;
      finished_ = true;
      return runtime_.epoch_.EndOperation(token_);
    }
   private:
    RimeRuntime& runtime_;
    std::unique_lock<std::mutex> lock_;
    int exceptions_;
    std::uint64_t token_;
    bool finished_ = false;
  };

  ~RimeRuntime() {
    epoch_.Invalidate();
    if (api_ != nullptr) {
      // Keep callback storage alive until Rime has joined work and finalized
      // sessions/modules. Never clear/replace the handler while it can notify.
      api_->finalize();
      if (notification_registered_) api_->set_notification_handler(nullptr, nullptr);
    }
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  RimeRuntime(const RimeRuntime&) = delete;
  RimeRuntime& operator=(const RimeRuntime&) = delete;

  static RimeRuntime& Instance() {
    static RimeRuntime runtime;
    return runtime;
  }

  [[nodiscard]] RimeApi* api() const noexcept { return api_; }
  [[nodiscard]] std::uint64_t DictionaryEpoch() const noexcept { return epoch_.Read(); }

 private:
  RimeRuntime() { Initialize(); }

  static void OnNotification(void* context, RimeSessionId, const char* type,
                             const char* value) noexcept {
    auto& runtime = *static_cast<RimeRuntime*>(context);
    if (type == nullptr || std::strcmp(type, "deploy") != 0) return;
    if (runtime.initializing_.load() && value != nullptr &&
        (std::strcmp(value, "start") == 0 || std::strcmp(value, "success") == 0)) return;
    // Post-startup background work permanently retires this runtime's metadata.
    // In particular, success must not restore it: more tasks may still follow.
    runtime.epoch_.Invalidate();
  }

  void Initialize() {
    const auto executable_directory = ExecutableDirectory();
    const auto shared_data_path = executable_directory / L"data" / L"rime";
    const auto library_path = executable_directory / L"rime.dll";
    std::error_code file_error;
    const bool has_library = std::filesystem::is_regular_file(library_path, file_error);
    file_error.clear();
    const bool has_data =
        std::filesystem::is_regular_file(shared_data_path / L"default.yaml", file_error);
    if (executable_directory.empty() || !has_library || !has_data || file_error) {
      return;
    }

#if defined(ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST) && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
    // Source provenance is a necessary precondition for diagnostic positive
    // epochs, not a complete user-config admission or permission to enable the
    // product. A failure retires metadata only; ordinary Rime still initializes.
    const bool source_profile_verified =
        CheckRimeSourceProfile(library_path, shared_data_path, kPinnedRimeSourceProfile).status ==
        RimeSourceProfileStatus::kVerifiedSourceFiles;
    if (!source_profile_verified) {
      epoch_.Invalidate();
    }
#endif

    const auto user_data_path = UserDataDirectory();
    if (user_data_path.empty()) {
      return;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(user_data_path, directory_error);
    if (directory_error) {
      return;
    }
#if defined(ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST) && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
    // Latch before initial overlay provisioning or user initialization can
    // obscure an unsupported preexisting configuration. Never delete extensions.
    if (!source_profile_verified ||
        CheckRimeUserProfile(user_data_path, shared_data_path) != RimeUserProfileStatus::kValid) {
      epoch_.Invalidate();
    }
    std::error_code cache_error;
    const bool has_cache = std::filesystem::exists(user_data_path / L"build", cache_error);
    // Existing compiled files can be consumed during maintenance. A later
    // rebuild must not erase evidence that startup began with an unknown cache.
    if (cache_error || (has_cache &&
        CheckRimeCompiledProfile(user_data_path / L"build", kPinnedRimeCompiledProfile).status !=
            RimeCompiledProfileStatus::kVerifiedCompiledFiles)) epoch_.Invalidate();
#endif
    for (const auto* overlay : {L"default.custom.yaml", L"rime_ice.custom.yaml"}) {
      if (!EnsureInitialOverlay(shared_data_path / overlay, user_data_path / overlay)) {
        return;
      }
    }

    module_ = LoadLibraryExW(library_path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_ == nullptr) {
      return;
    }
    using GetApi = RimeApi*(__cdecl*)();
    const FARPROC procedure = GetProcAddress(module_, "rime_get_api");
    if (procedure == nullptr) {
      FreeLibrary(module_);
      module_ = nullptr;
      return;
    }
    const auto get_api = std::bit_cast<GetApi>(procedure);
    api_ = get_api();
    if (!HasRequiredApi()) {
      api_ = nullptr;
      FreeLibrary(module_);
      module_ = nullptr;
      return;
    }
#if defined(ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST) && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
    if (!source_profile_verified || !HasPinnedModuleProfile(module_, api_)) epoch_.Invalidate();
#endif

    shared_data_directory_ = ToUtf8(shared_data_path.native());
    user_data_directory_ = ToUtf8(user_data_path.native());
    if (shared_data_directory_.empty() || user_data_directory_.empty()) {
      api_ = nullptr;
      FreeLibrary(module_);
      module_ = nullptr;
      return;
    }

    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = shared_data_directory_.c_str();
    traits.user_data_dir = user_data_directory_.c_str();
    traits.distribution_name = "Ziliu";
    traits.distribution_code_name = "ziliu";
    traits.distribution_version = ZILIU_DISTRIBUTION_VERSION;
    traits.app_name = "rime.ziliu";
    traits.min_log_level = 2;
    if (RIME_API_AVAILABLE(api_, set_notification_handler)) {
      api_->set_notification_handler(&OnNotification, this);
      notification_registered_ = true;
    }
    // Latch before initialize executes user Lua: a script could remove itself
    // while retaining callbacks, so a post-initialization check is insufficient.
    // Bundled plugin/script paths were audited; user extensions are not covered.
    if (HasLuaEnvironmentOverride()) epoch_.Invalidate();
    for (const auto& override_path : {user_data_path / L"rime.lua", user_data_path / L"lua"}) {
      std::error_code override_error;
      const bool has_override = std::filesystem::exists(override_path, override_error);
      if (has_override || override_error) epoch_.Invalidate();
    }
    api_->setup(&traits);
    api_->initialize(&traits);
    // Full maintenance rebuilds the Rime workspace on every Broker cold start.
    // The non-full path checks source timestamps and only schedules deployment
    // after an install, bundled-data update, or user configuration change.
    static_cast<void>(api_->start_maintenance(False));
    // The API joins the general Deployer future, including non-maintenance work.
    // This boundary exists only at startup: never wait for recovery per keystroke.
    api_->join_maintenance_thread();
    initializing_.store(false);
    if (HasLuaEnvironmentOverride()) epoch_.Invalidate();
    // Also reject overrides appearing during startup; neither check can revive
    // a generation invalidated by the other check or a deployment callback.
    for (const auto& override_path : {user_data_path / L"rime.lua", user_data_path / L"lua"}) {
      std::error_code override_error;
      const bool has_override = std::filesystem::exists(override_path, override_error);
      if (has_override || override_error) epoch_.Invalidate();
    }
    // Observed entry points do not certify the loaded DLL, shared scripts or
    // arbitrary user schemas. Until runtime admission is implemented, only the
    // isolated test executable may exercise positive generations. No runtime
    // environment/configuration switch can opt the product into this experiment.
#if defined(ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST) && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
    if (!source_profile_verified || !HasPinnedModuleProfile(module_, api_) ||
        CheckRimeUserProfile(user_data_path, shared_data_path) != RimeUserProfileStatus::kValid ||
        CheckRimeSourceProfile(library_path, shared_data_path, kPinnedRimeSourceProfile).status !=
            RimeSourceProfileStatus::kVerifiedSourceFiles ||
        CheckRimeCompiledProfile(user_data_path / L"build", kPinnedRimeCompiledProfile).status !=
            RimeCompiledProfileStatus::kVerifiedCompiledFiles) {
      epoch_.Invalidate();
    }
    if (notification_registered_) static_cast<void>(epoch_.EnableAfterInitialization());
#else
    epoch_.Invalidate();
#endif
  }

  [[nodiscard]] bool HasRequiredApi() const noexcept {
    return api_ != nullptr && api_->setup != nullptr && api_->initialize != nullptr &&
           api_->finalize != nullptr && api_->start_maintenance != nullptr &&
           api_->join_maintenance_thread != nullptr && api_->create_session != nullptr &&
           api_->destroy_session != nullptr && api_->process_key != nullptr &&
           api_->clear_composition != nullptr && api_->commit_composition != nullptr &&
           api_->get_commit != nullptr && api_->free_commit != nullptr &&
           api_->get_context != nullptr && api_->free_context != nullptr &&
           api_->select_schema != nullptr && api_->set_option != nullptr &&
           api_->select_candidate != nullptr && api_->candidate_list_from_index != nullptr &&
           api_->candidate_list_next != nullptr && api_->candidate_list_end != nullptr;
  }

  HMODULE module_ = nullptr;
  RimeApi* api_ = nullptr;
  std::string shared_data_directory_;
  std::string user_data_directory_;
  std::mutex operation_mutex_;
  core::DictionaryEpoch epoch_;
  std::atomic_bool initializing_{true};
  bool notification_registered_ = false;
};

class RimeEngine final : public core::Engine {
 public:
  RimeEngine(RimeRuntime& runtime, RimeSessionId session_id)
      : runtime_(runtime), api_(runtime.api()), session_id_(session_id) {}
  [[nodiscard]] std::uint64_t DictionaryEpoch() const noexcept override {
    return runtime_.DictionaryEpoch();
  }

  ~RimeEngine() override {
    RimeRuntime::Operation operation(runtime_);
    api_->clear_composition(session_id_);
    api_->destroy_session(session_id_);
  }

  void Reset() override {
    RimeRuntime::Operation operation(runtime_);
    ResetPaging();
    ClearTrackedInput();
    api_->clear_composition(session_id_);
  }

  bool ProcessLetter(wchar_t letter) override {
    RimeRuntime::Operation operation(runtime_);
    if (letter < L'A' || (letter > L'Z' && letter < L'a') || letter > L'z') {
      return false;
    }
    const int keycode = static_cast<int>(letter >= L'A' && letter <= L'Z' ? letter - L'A' + L'a'
                                                                          : letter);
    if (pinyin_letter_count_ >= core::kMaximumPinyinLetters) {
      ResetPaging();
      return true;
    }
    const bool consumed = ProcessTrackedKey(keycode, static_cast<wchar_t>(keycode), true);
    if (consumed) {
      ResetPaging();
    }
    return consumed;
  }

  bool ProcessSeparator() override {
    RimeRuntime::Operation operation(runtime_);
    if (!raw_keys_.empty() && raw_keys_.back() == L'\'') {
      return false;
    }
    const bool consumed = ProcessTrackedKey('\'', L'\'', false);
    if (consumed) {
      ResetPaging();
    }
    return consumed;
  }

  bool Backspace() override {
    RimeRuntime::Operation operation(runtime_);
    if (raw_keys_.empty()) {
      return false;
    }
    if (!automatic_commit_text_prefix_.empty()) {
      raw_keys_.pop_back();
      RebuildComposition();
      ResetPaging();
      return true;
    }
    const bool consumed = api_->process_key(session_id_, kRimeBackspace, 0) != False;
    if (consumed) {
      if (raw_keys_.back() != L'\'') {
        --pinyin_letter_count_;
      }
      raw_keys_.pop_back();
      ResetPaging();
    }
    return consumed;
  }

  bool PageUp() override {
    RimeRuntime::Operation operation(runtime_);
    if (pinyin_letter_count_ >= core::kMaximumPinyinLetters) {
      return false;
    }
    if (previous_page_offsets_.empty()) {
      return false;
    }
    candidate_offset_ = previous_page_offsets_.back();
    previous_page_offsets_.pop_back();
    return true;
  }

  bool PageDown() override {
    RimeRuntime::Operation operation(runtime_);
    if (pinyin_letter_count_ >= core::kMaximumPinyinLetters) {
      return false;
    }
    const int next_offset = NextVisiblePageOffset();
    if (next_offset < 0) {
      return false;
    }
    previous_page_offsets_.push_back(candidate_offset_);
    candidate_offset_ = next_offset;
    return true;
  }

  void SetCandidatePageSize(std::size_t page_size) override {
    RimeRuntime::Operation operation(runtime_);
    ResetPaging();
    candidate_page_size_ =
        std::clamp<std::size_t>(page_size, 1, core::ipc::kMaximumCandidatesPerPage);
  }

  void SetCandidateWindowPageCount(std::size_t page_count) override {
    RimeRuntime::Operation operation(runtime_);
    ResetPaging();
    candidate_window_page_count_ =
        std::clamp<std::size_t>(page_count, 1, core::ipc::kMaximumCandidateWindowPages);
  }

  void SetTraditional(bool enabled) override {
    RimeRuntime::Operation operation(runtime_);
    ResetPaging();
    api_->set_option(session_id_, "traditionalization", enabled ? True : False);
  }

  void SetChineseCandidatesOnly(bool enabled) override {
    RimeRuntime::Operation operation(runtime_);
    if (chinese_candidates_only_ == enabled) {
      return;
    }
    chinese_candidates_only_ = enabled;
    ResetPaging();
  }

  core::SelectionResult Select(std::size_t candidate_index) override {
    RimeRuntime::Operation operation(runtime_);
    if (pinyin_letter_count_ >= core::kMaximumPinyinLetters) {
      return {};
    }
    const int source_candidate_index = CandidateIndexForVisible(candidate_index);
    if (candidate_index >= candidate_page_size_ || source_candidate_index < 0 ||
        !api_->select_candidate(session_id_,
                                static_cast<std::size_t>(source_candidate_index))) {
      return {};
    }
    ResetPaging();

    RIME_STRUCT(RimeCommit, commit);
    if (api_->get_commit(session_id_, &commit)) {
      std::wstring result = automatic_commit_text_prefix_ + FromUtf8(commit.text);
      api_->free_commit(&commit);
      ClearTrackedInput();
      return core::SelectionResult{true, std::move(result)};
    }

    RIME_STRUCT(RimeContext, context);
    const bool has_context = api_->get_context(session_id_, &context);
    const bool selection_is_complete =
        has_context && context.composition.length > 0 && context.menu.num_candidates == 0;
    if (has_context) {
      api_->free_context(&context);
    }
    if (!selection_is_complete || !api_->commit_composition(session_id_)) {
      return core::SelectionResult{true, {}};
    }
    if (!api_->get_commit(session_id_, &commit)) {
      return core::SelectionResult{true, {}};
    }
    std::wstring result = automatic_commit_text_prefix_ + FromUtf8(commit.text);
    api_->free_commit(&commit);
    ClearTrackedInput();
    return core::SelectionResult{true, std::move(result)};
  }

  [[nodiscard]] core::CompositionSnapshot Snapshot() const override {
    RimeRuntime::Operation operation(runtime_);
    return BuildSnapshot();
  }

  [[nodiscard]] core::VersionedCompositionSnapshot SnapshotWithDictionaryEpoch() const override {
    RimeRuntime::Operation operation(runtime_);
    auto snapshot = BuildSnapshot();
    // Finish keeps the mutex held until this return value is captured. A late
    // deploy callback can only turn the generation unavailable, never revive it.
    const auto epoch = operation.Finish();
    return {std::move(snapshot), epoch};
  }

 private:
  [[nodiscard]] core::CompositionSnapshot BuildSnapshot() const {
    core::CompositionSnapshot snapshot = ReadSnapshot();
    if (!automatic_commit_preedit_prefix_.empty()) {
      if (!snapshot.preedit.empty() && automatic_commit_preedit_prefix_.back() != L'\'' &&
          snapshot.preedit.front() != L'\'') {
        snapshot.preedit.insert(snapshot.preedit.begin(), L'\'');
      }
      snapshot.preedit.insert(0, automatic_commit_preedit_prefix_);
    }
    if (!automatic_commit_text_prefix_.empty()) {
      for (auto& candidate : snapshot.candidates) {
        candidate.text.insert(0, automatic_commit_text_prefix_);
      }
    }
    if (snapshot.preedit.empty() && !raw_keys_.empty()) {
      snapshot.preedit = raw_keys_;
    }
    if (pinyin_letter_count_ >= core::kMaximumPinyinLetters) {
      snapshot.candidates.clear();
      snapshot.has_previous_page = false;
      snapshot.has_next_page = false;
    }
    return snapshot;
  }

  bool ProcessTrackedKey(int keycode, wchar_t raw_key, bool is_letter) {
    const core::CompositionSnapshot before = ReadSnapshot();
    bool accepted = api_->process_key(session_id_, keycode, 0) != False;
    bool has_commit = false;
    RIME_STRUCT(RimeCommit, commit);
    if (api_->get_commit(session_id_, &commit)) {
      AppendAutomaticSegment(FromUtf8(commit.text), before.preedit);
      api_->free_commit(&commit);
      has_commit = true;
    }

    const core::CompositionSnapshot after = ReadSnapshot();
    if (!has_commit && (!accepted || after.preedit.empty()) && !before.preedit.empty() &&
        !before.candidates.empty()) {
      // Rime Ice can auto-clear a long but otherwise valid spelling sequence
      // (notably repeated initials such as "s") without producing a commit.
      // Preserve its first choice as a completed internal segment, then retry
      // the current key in a fresh Rime composition.
      AppendAutomaticSegment(before.candidates.front().text, before.preedit);
      api_->clear_composition(session_id_);
      accepted = api_->process_key(session_id_, keycode, 0) != False;
      if (api_->get_commit(session_id_, &commit)) {
        AppendAutomaticSegment(FromUtf8(commit.text), std::wstring_view{});
        api_->free_commit(&commit);
      }
    }

    if (!accepted && !is_letter) {
      return false;
    }
    raw_keys_.push_back(raw_key);
    if (is_letter) {
      ++pinyin_letter_count_;
    }
    return true;
  }

  void AppendAutomaticSegment(std::wstring_view text, std::wstring_view preedit) {
    automatic_commit_text_prefix_.append(text);
    if (preedit.empty()) {
      return;
    }
    if (!automatic_commit_preedit_prefix_.empty() &&
        automatic_commit_preedit_prefix_.back() != L'\'' && preedit.front() != L'\'') {
      automatic_commit_preedit_prefix_.push_back(L'\'');
    }
    automatic_commit_preedit_prefix_.append(preedit);
  }

  void RebuildComposition() {
    const std::wstring keys = raw_keys_;
    api_->clear_composition(session_id_);
    ClearTrackedInput();
    for (const wchar_t key : keys) {
      const bool is_letter = key != L'\'';
      if (!ProcessTrackedKey(static_cast<int>(key), key, is_letter)) {
        break;
      }
    }
  }

  void ClearTrackedInput() {
    raw_keys_.clear();
    automatic_commit_text_prefix_.clear();
    automatic_commit_preedit_prefix_.clear();
    pinyin_letter_count_ = 0;
  }

  [[nodiscard]] core::CompositionSnapshot ReadSnapshot() const {
    core::CompositionSnapshot snapshot;
    RIME_STRUCT(RimeContext, context);
    if (!api_->get_context(session_id_, &context)) {
      return snapshot;
    }
    snapshot.preedit = FormatPreedit(FromUtf8(context.composition.preedit));
    const bool has_menu = context.menu.num_candidates > 0;
    api_->free_context(&context);

    if (has_menu) {
      const std::size_t current_page = previous_page_offsets_.size();
      const std::size_t first_page =
          (current_page / candidate_window_page_count_) * candidate_window_page_count_;
      int first_candidate_offset = candidate_offset_;
      if (first_page < current_page && first_page < previous_page_offsets_.size()) {
        first_candidate_offset = previous_page_offsets_[first_page];
      }
      const std::size_t active_page = current_page - first_page;
      const std::size_t maximum_candidates =
          candidate_page_size_ * candidate_window_page_count_;
      RimeCandidateListIterator iterator{};
      if (api_->candidate_list_from_index(session_id_, &iterator, first_candidate_offset)) {
        snapshot.candidates.reserve(maximum_candidates);
        while (snapshot.candidates.size() < maximum_candidates &&
               api_->candidate_list_next(&iterator)) {
          const auto& candidate = iterator.candidate;
          const std::wstring candidate_text = FromUtf8(candidate.text);
          if (!ShouldShowCandidate(candidate_text)) {
            continue;
          }
          const std::size_t visible_index = snapshot.candidates.size();
          snapshot.candidates.push_back(core::Candidate{
              candidate_text, FromUtf8(candidate.comment),
              1.0 - static_cast<double>(visible_index) /
                        static_cast<double>(maximum_candidates + 1)});
        }
        api_->candidate_list_end(&iterator);
      }
      if (!snapshot.candidates.empty()) {
        snapshot.highlighted_index =
            std::min(active_page * candidate_page_size_, snapshot.candidates.size() - 1);
      }
      snapshot.has_previous_page = !previous_page_offsets_.empty();
      snapshot.has_next_page = NextVisiblePageOffset() >= 0;
    }
    return snapshot;
  }

  void ResetPaging() {
    candidate_offset_ = 0;
    previous_page_offsets_.clear();
  }

  [[nodiscard]] bool ShouldShowCandidate(std::wstring_view text) const noexcept {
    return !chinese_candidates_only_ || core::IsChineseCandidate(text);
  }

  [[nodiscard]] int CandidateIndexForVisible(std::size_t visible_index) const {
    if (visible_index >= candidate_page_size_) {
      return -1;
    }
    RimeCandidateListIterator iterator{};
    if (!api_->candidate_list_from_index(session_id_, &iterator, candidate_offset_)) {
      return -1;
    }
    int source_index = candidate_offset_;
    std::size_t current_visible_index = 0;
    int result = -1;
    while (api_->candidate_list_next(&iterator)) {
      const int current_source_index = source_index++;
      if (!ShouldShowCandidate(FromUtf8(iterator.candidate.text))) {
        continue;
      }
      if (current_visible_index == visible_index) {
        result = current_source_index;
        break;
      }
      ++current_visible_index;
    }
    api_->candidate_list_end(&iterator);
    return result;
  }

  [[nodiscard]] int NextVisiblePageOffset() const {
    RimeCandidateListIterator iterator{};
    if (!api_->candidate_list_from_index(session_id_, &iterator, candidate_offset_)) {
      return -1;
    }
    int source_index = candidate_offset_;
    std::size_t visible_count = 0;
    int next_offset = -1;
    while (api_->candidate_list_next(&iterator)) {
      const int current_source_index = source_index++;
      if (!ShouldShowCandidate(FromUtf8(iterator.candidate.text))) {
        continue;
      }
      if (visible_count == candidate_page_size_) {
        next_offset = current_source_index;
        break;
      }
      ++visible_count;
    }
    api_->candidate_list_end(&iterator);
    return next_offset;
  }

  RimeRuntime& runtime_;
  RimeApi* api_;
  RimeSessionId session_id_;
  int candidate_offset_ = 0;
  std::size_t candidate_page_size_ = core::ipc::kMaximumCandidatesPerPage;
  std::size_t candidate_window_page_count_ = 1;
  bool chinese_candidates_only_ = true;
  std::wstring raw_keys_;
  std::wstring automatic_commit_text_prefix_;
  std::wstring automatic_commit_preedit_prefix_;
  std::size_t pinyin_letter_count_ = 0;
  std::vector<int> previous_page_offsets_;
};

std::unique_ptr<core::Engine> TryCreateRimeEngine(bool restricted) {
  auto& runtime = RimeRuntime::Instance();
  RimeApi* api = runtime.api();
  if (api == nullptr) {
    return nullptr;
  }
  RimeRuntime::Operation operation(runtime);
  const RimeSessionId session_id = api->create_session();
  if (session_id == 0) {
    return nullptr;
  }
  const char* requested_schema = restricted ? "ziliu_private" : "rime_ice";
  char current_schema[64]{};
  // Rime already loads a schema at session creation. Reapplying it tears down
  // translators and reopens their user databases. Its bounded copy may omit the
  // terminator on truncation, so only a complete exact match can skip selection.
  const bool schema_matches = RIME_API_AVAILABLE(api, get_current_schema) &&
      api->get_current_schema(session_id, current_schema, sizeof(current_schema)) != False &&
      std::memchr(current_schema, '\0', sizeof(current_schema)) != nullptr &&
      std::strcmp(current_schema, requested_schema) == 0;
  if (!schema_matches && !api->select_schema(session_id, requested_schema)) {
    api->destroy_session(session_id);
    return nullptr;
  }
  return std::make_unique<RimeEngine>(runtime, session_id);
}

}  // namespace

void WarmUpEngineRuntime() {
  static_cast<void>(RimeRuntime::Instance());
}

std::unique_ptr<core::Engine> CreateEngine(bool restricted) {
  auto engine = TryCreateRimeEngine(restricted);
  // Missing restricted data fails closed rather than silently changing the
  // private-field engine contract. Ordinary mode retains the development stub.
  if (engine == nullptr && !restricted) {
    engine = core::CreateStubEngine();
  }
  return engine;
}

}  // namespace ziliu::broker
