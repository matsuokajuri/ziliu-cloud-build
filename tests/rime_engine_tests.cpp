#include "ziliu/broker/rime_engine.h"
#include "ziliu/core/context_response_binding.h"
#include "ziliu/core/session_host.h"

#if defined(ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST) && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
#include "ziliu/broker/rime_source_profile_pin.h"
#endif

#include <windows.h>
#include <rime_api.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#ifndef ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST
#error "RIME epoch tests require an explicit diagnostic build definition."
#elif ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST != 0 && ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST != 1
#error "ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST must be 0 or 1."
#endif

namespace {

void Expect(bool condition, std::string_view message);

std::filesystem::path UserDataPath() {
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, "ZILIU_RIME_USER_DATA_DIR") != 0 || value == nullptr) {
    std::free(value);
    return {};
  }
  const std::filesystem::path result(value);
  std::free(value);
  return result;
}

using FileSnapshot = std::map<std::filesystem::path, std::vector<char>>;

FileSnapshot SnapshotFiles(const std::filesystem::path& root) {
  FileSnapshot snapshot;
  std::error_code error;
  std::filesystem::path failed_path;
  for (std::filesystem::recursive_directory_iterator it(root, error), end;
       !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error) || error) continue;
    // LevelDB's process-held LOCK is synchronization state, not persisted input,
    // and is intentionally not readable while held. Only its known empty form is excluded.
    if (it->path().filename() == L"LOCK") {
      if (it->file_size(error) != 0 || error) {
        failed_path = it->path();
        if (!error) error = std::error_code(ERROR_INVALID_DATA, std::system_category());
        break;
      }
      continue;
    }
    const auto relative = std::filesystem::relative(it->path(), root, error);
    if (error) break;
    const HANDLE file = CreateFileW(it->path().c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      failed_path = it->path();
      error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
      break;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > SIZE_MAX) {
      failed_path = it->path();
      const DWORD last_error = GetLastError();
      error = std::error_code(static_cast<int>(last_error == ERROR_SUCCESS
                                                   ? ERROR_INVALID_DATA : last_error),
                              std::system_category());
      CloseHandle(file);
      break;
    }
    std::vector<char> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto chunk = static_cast<DWORD>((std::min)(
          bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
      DWORD read = 0;
      if (!ReadFile(file, bytes.data() + offset, chunk, &read, nullptr)) {
        failed_path = it->path();
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        break;
      }
      if (read != chunk) {
        failed_path = it->path();
        error = std::error_code(ERROR_HANDLE_EOF, std::system_category());
        break;
      }
      offset += read;
    }
    CloseHandle(file);
    if (error) break;
    snapshot.emplace(relative, std::move(bytes));
  }
  if (error) {
    std::cerr << "Snapshot failure at " << failed_path.string() << ": " << error.message() << '\n';
  }
  Expect(!error, "Rime user-data snapshot should be readable");
  return snapshot;
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>>
PrepareUnchangedOverlays(const std::filesystem::path& executable_path) {
  std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> timestamps;
  const std::filesystem::path user_data_path = UserDataPath();
  if (user_data_path.empty()) return timestamps;
  const auto shared_data_path = executable_path.parent_path() / "data" / "rime";
  std::error_code file_error;
  std::filesystem::create_directories(user_data_path, file_error);
  Expect(!file_error, "Rime test user data directory should be available");
  const auto sentinel = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
  for (const auto* overlay : {"default.custom.yaml", "rime_ice.custom.yaml"}) {
    const auto source = shared_data_path / overlay;
    if (!std::filesystem::is_regular_file(source, file_error) || file_error) {
      timestamps.clear();
      return timestamps;
    }
    const auto destination = user_data_path / overlay;
    file_error.clear();
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, file_error);
    Expect(!file_error, "Rime test overlay should be prepared");
    std::filesystem::last_write_time(destination, sentinel, file_error);
    Expect(!file_error, "Rime test overlay timestamp should be adjustable");
    timestamps.emplace_back(destination, sentinel);
  }
  return timestamps;
}

#if ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST == 1
void TestRimeResponseBinding(std::unique_ptr<ziliu::core::Engine> initial_engine) {
  using namespace ziliu::core;
  using ipc::Command;
  SessionHost host([&](bool restricted) {
    if (initial_engine) return std::move(initial_engine);
    return ziliu::broker::CreateEngine(restricted);
  });
  std::uint64_t request_id = 0;
  ipc::Request last_request;
  const auto send = [&](std::uint64_t session, Command command, std::uint32_t value = 0) {
    last_request = {++request_id, session, command, value};
    std::vector<std::byte> wire;
    ipc::Request decoded_request;
    Expect(ipc::EncodeRequest(last_request, &wire) &&
               ipc::DecodeRequest(wire, &decoded_request), "Rime request codec roundtrip");
    auto response = host.Handle(decoded_request);
    // This is a codec + SessionHost test, not an actual pipe or TSF host.
    // Synthetic broker/field identities below must never be production evidence.
    response.broker_instance = {0x54455354, 1};
    ipc::Response decoded_response;
    Expect(ipc::EncodeResponse(response, &wire) &&
               ipc::DecodeResponse(wire, &decoded_response), "Rime response codec roundtrip");
    return decoded_response;
  };
  const auto created = send(0, Command::kCreateSession);
  const auto peer = send(0, Command::kCreateSession);
  Expect(created.status == ipc::Status::kOk && peer.status == ipc::Status::kOk &&
             created.session_id != peer.session_id, "two real Rime sessions created");
  const auto type = [&](std::uint64_t session) {
    ipc::Response response;
    for (const wchar_t letter : std::wstring_view(L"nihao")) {
      response = send(session, Command::kInputLetter, static_cast<std::uint32_t>(letter));
      Expect(response.status == ipc::Status::kOk && response.consumed, "Rime consumes input");
    }
    Expect(response.snapshot.preedit == L"ni'hao" && !response.snapshot.candidates.empty() &&
               response.snapshot.candidates.front().text == L"你好" && response.dictionary_epoch != 0,
           "candidate binding uses actual Rime snapshot and diagnostic epoch, never Stub");
    return response;
  };
  const ContextRequestSnapshot field{
      .field_token = {0x4649454c44, 1}, .field_epoch = 1, .edit_revision = 1,
      .selection_start_utf16 = 2, .selection_end_utf16 = 2,
      .privacy = ContextPrivacy::ordinary, .identity_verified = true, .fresh_read_ok = true};
  const auto now = ContextResponseBinding::TimePoint{} + std::chrono::seconds(1);
  ContextResponseBinding binding({0x42494e44, 1});
  const auto begin = [&](const ipc::Response& candidates) {
    const auto ticket = binding.Begin(field, u"汽车", last_request, candidates,
                                      now, now + std::chrono::seconds(30));
    Expect(ticket.has_value(), "actual Rime metadata starts a synthetic-field binding");
    return *ticket;
  };
  const auto accept = [&](const ContextRequestTicket& ticket, const ipc::Response& state) {
    return binding.TryAccept(ticket, field, u"汽车", last_request, state,
                             now + std::chrono::seconds(1));
  };

  const auto candidates = type(created.session_id);
  auto ticket = begin(candidates);
  auto state = send(created.session_id, Command::kGetSessionState);
  Expect(state.dictionary_epoch == candidates.dictionary_epoch &&
             state.input_revision == candidates.input_revision &&
             state.candidate_revision == candidates.candidate_revision &&
             state.snapshot.empty() && state.snapshot.candidates.empty() && accept(ticket, state),
         "fresh state query accepts unchanged real Rime metadata without lazy re-evaluation");
  state = send(created.session_id, Command::kGetSessionState);
  Expect(state.dictionary_epoch == candidates.dictionary_epoch &&
             state.candidate_revision == candidates.candidate_revision,
         "repeated state queries do not advance engine or candidate revisions");

  auto updated = send(created.session_id, Command::kSetCandidatePageSize, 7);
  ticket = begin(updated);
  static_cast<void>(type(peer.session_id));
  const auto learned = send(peer.session_id, Command::kSelectCandidate, 0);
  Expect(learned.status == ipc::Status::kOk && learned.consumed && learned.commit == L"你好",
         "peer performs a real Rime selection through SessionHost");
  state = send(created.session_id, Command::kGetSessionState);
  Expect(state.input_revision == updated.input_revision &&
             state.candidate_revision == updated.candidate_revision &&
             state.dictionary_epoch > updated.dictionary_epoch && !accept(ticket, state),
         "peer learning rejects A's result despite unchanged A input/candidate revisions and prefix");

  updated = send(created.session_id, Command::kSetCandidatePageSize, 7);
  ticket = begin(updated);
  static_cast<void>(send(created.session_id, Command::kBackspace));
  state = send(created.session_id, Command::kGetSessionState);
  Expect(state.input_revision > updated.input_revision && !accept(ticket, state),
         "real edit rejects delayed response even when field fixture stays unchanged");

  updated = send(created.session_id, Command::kInputLetter, L'o');
  ticket = begin(updated);
  static_cast<void>(send(peer.session_id, Command::kCloseSession));
  state = send(created.session_id, Command::kGetSessionState);
  Expect(state.dictionary_epoch > updated.dictionary_epoch && !accept(ticket, state),
         "peer destruction invalidates delayed dictionary-dependent work");
  updated = send(created.session_id, Command::kSetCandidatePageSize, 7);
  ticket = begin(updated);
  static_cast<void>(send(created.session_id, Command::kCloseSession));
  state = send(created.session_id, Command::kGetSessionState);
  Expect(state.status == ipc::Status::kSessionNotFound && !accept(ticket, state),
         "closed source session rejects delayed work instead of reusing cached state");
  std::cout << "ziliu_rime_engine_tests: response binding OK (synthetic field, real Rime, codec)\n";
}
#endif

}  // namespace

int main(int argument_count, char* arguments[]) {
  Expect(argument_count > 0 && arguments[0] != nullptr,
         "Rime test executable path should be available");
  // Validate isolation before initialization can touch a real profile. Each
  // run gets a fresh dictionary; retain older runs for failure investigation.
  const auto test_root = UserDataPath();
  Expect(!test_root.empty(), "Rime tests require an isolated user-data directory");
  const auto run_path = test_root /
      (L"run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::error_code run_error;
  std::filesystem::create_directories(test_root, run_error);
  Expect(!run_error, "Rime test root should be creatable");
  Expect(std::filesystem::create_directory(run_path, run_error) && !run_error,
         "Rime test run must have a new unique directory");
  Expect(SetEnvironmentVariableW(L"ZILIU_RIME_USER_DATA_DIR", run_path.c_str()) != FALSE,
         "Rime test runtime must use its fresh isolated directory");
  Expect(_putenv_s("ZILIU_RIME_USER_DATA_DIR", run_path.string().c_str()) == 0 &&
             UserDataPath() == run_path,
         "CRT fixture readers must use the same fresh directory as the Windows runtime");
  const std::string_view test_mode = argument_count > 1 ? arguments[1] : "";
  Expect(test_mode.empty() || test_mode == "--user-lua-file" || test_mode == "--user-lua-dir" ||
             test_mode == "--user-lua-self-remove" || test_mode == "--dictionary-epoch" ||
             test_mode == "--response-binding" || test_mode == "--lua-search-env" ||
             test_mode == "--user-overlay" || test_mode == "--user-schema" ||
             test_mode == "--preexisting-cache" || test_mode == "--fresh-profile",
         "test mode must be one of the declared isolation cases");
#if ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST == 1
  Expect(!test_mode.empty(),
         "the diagnostic executable must be invoked with an explicit diagnostic mode");
#else
  Expect(test_mode.empty(),
         "the production executable only accepts its full ordinary regression mode");
#endif
  if (test_mode == "--user-lua-file") {
    std::ofstream inert_script(run_path / "rime.lua");
    inert_script << "-- inert test-only user override\n";
    Expect(inert_script.good(), "inert user Lua fixture must be written");
  } else if (test_mode == "--user-lua-dir") {
    Expect(std::filesystem::create_directory(run_path / "lua", run_error) && !run_error,
           "empty user Lua directory fixture must be created");
  } else if (test_mode == "--user-lua-self-remove") {
    std::ofstream self_removing_script(run_path / "rime.lua");
    self_removing_script << "local source = debug.getinfo(1, 'S').source\n"
                            "assert(source:sub(1, 1) == '@')\n"
                            "assert(os.remove(source:sub(2)))\n";
    Expect(self_removing_script.good(), "self-removing user Lua fixture must be written");
  } else if (test_mode == "--lua-search-env") {
    // Process-local only; double separator preserves normal Lua search defaults.
    Expect(SetEnvironmentVariableW(L"LUA_PATH", L";;") != FALSE &&
               _putenv_s("LUA_PATH", ";;") == 0,
           "isolated diagnostic process should expose an unsupported Lua search override");
  }
  const auto overlay_timestamps =
      test_mode == "--fresh-profile"
          ? std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>>{}
          : PrepareUnchangedOverlays(std::filesystem::absolute(arguments[0]));
  if (test_mode == "--user-overlay") {
    std::ofstream overlay(run_path / "rime_ice.custom.yaml", std::ios::app | std::ios::binary);
    overlay << "\n# isolated user customization: must survive startup\n";
    Expect(overlay.good(), "user overlay fixture must be writable");
  } else if (test_mode == "--user-schema") {
    std::ofstream schema(run_path / "unused.custom.yaml");
    schema << "patch: {}\n";
    Expect(schema.good(), "user schema fixture must be writable");
  } else if (test_mode == "--preexisting-cache") {
    Expect(std::filesystem::create_directory(run_path / "build", run_error) && !run_error,
           "existing incomplete cache fixture must be created");
    // It is safe for ordinary Rime to regenerate this cache. The diagnostic
    // admission must still remember the incomplete startup state afterward.
    std::ofstream cache(run_path / "build" / "default.yaml");
    cache << "config_version: stale-test-cache\n";
    Expect(cache.good(), "existing incomplete cache fixture must be writable");
  }
#if ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST == 1
  const auto executable_directory = std::filesystem::absolute(arguments[0]).parent_path();
  const auto source_profile = ziliu::broker::CheckRimeSourceProfile(
      executable_directory / L"rime.dll", executable_directory / L"data" / L"rime",
      ziliu::broker::kPinnedRimeSourceProfile);
  Expect(source_profile.status == ziliu::broker::RimeSourceProfileStatus::kVerifiedSourceFiles &&
             source_profile.file_count == ziliu::broker::kPinnedRimeSourceProfile.file_count,
         "diagnostic Rime tests require the independently pinned DLL and shared source profile");
#endif
  auto engine = ziliu::broker::CreateEngine();
#if ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST == 1
  if (test_mode == "--dictionary-epoch" || test_mode == "--response-binding" ||
      test_mode == "--fresh-profile") {
    const auto compiled = ziliu::broker::CheckRimeCompiledProfile(
        run_path / "build", ziliu::broker::kPinnedRimeCompiledProfile);
    if (compiled.status != ziliu::broker::RimeCompiledProfileStatus::kVerifiedCompiledFiles) {
      std::cerr << "Compiled profile status=" << static_cast<int>(compiled.status)
                << " files=" << compiled.file_count << " sha256=" << compiled.data_sha256 << '\n';
    }
    Expect(compiled.status == ziliu::broker::RimeCompiledProfileStatus::kVerifiedCompiledFiles,
           "fresh fixed-official cache must match independently pinned normalized bytes");
  }
  if (test_mode == "--user-overlay") {
    std::ifstream overlay(run_path / "rime_ice.custom.yaml", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(overlay)), {});
    Expect(contents.ends_with("\n# isolated user customization: must survive startup\n"),
           "startup must preserve the unsupported user overlay without rewriting it");
  } else if (test_mode == "--user-schema") {
    std::ifstream schema(run_path / "unused.custom.yaml", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(schema)), {});
    Expect(contents == "patch: {}\n" || contents == "patch: {}\r\n",
           "startup must preserve the unsupported user schema");
  }
  if (test_mode == "--response-binding") {
    Expect(engine != nullptr, "Rime response-binding fixture requires a real engine");
    TestRimeResponseBinding(std::move(engine));
    return EXIT_SUCCESS;
  }
#endif
#if ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST == 0
  Expect(engine != nullptr && engine->DictionaryEpoch() == 0,
         "production Rime initialization must not admit dictionary metadata");
#endif
  if (test_mode == "--dictionary-epoch" || test_mode == "--fresh-profile") {
    Expect(engine != nullptr, "ordinary Rime session should open for epoch acceptance");
    for (const auto& [overlay, expected_timestamp] : overlay_timestamps) {
      std::error_code file_error;
      const auto actual_timestamp = std::filesystem::last_write_time(overlay, file_error);
      Expect(!file_error && actual_timestamp == expected_timestamp,
             "an unchanged Rime overlay should not be rewritten during cold start");
    }
    engine->SetCandidatePageSize(7);
    for (const wchar_t letter : std::wstring_view(L"nihao")) {
      Expect(engine->ProcessLetter(letter), "Rime should consume epoch acceptance input");
    }
    const auto initial_snapshot = engine->Snapshot();
    Expect(initial_snapshot.preedit == L"ni'hao" && initial_snapshot.candidates.size() == 7 &&
               initial_snapshot.candidates.front().text == L"你好",
           "the fresh ordinary session should provide real Rime candidates");
    const auto initial_epoch = engine->DictionaryEpoch();
    Expect(initial_epoch != 0 && engine->DictionaryEpoch() == initial_epoch,
           "quiescent bundled Rime has a nonmutating generation read");
    const auto versioned = engine->SnapshotWithDictionaryEpoch();
    Expect(versioned.dictionary_epoch > initial_epoch &&
               versioned.dictionary_epoch == engine->DictionaryEpoch() &&
               versioned.snapshot.preedit == initial_snapshot.preedit &&
               versioned.snapshot.candidates == initial_snapshot.candidates,
           "lazy Rime snapshot and completed epoch must be captured together");
    auto peer = ziliu::broker::CreateEngine();
    Expect(peer != nullptr && peer->DictionaryEpoch() == engine->DictionaryEpoch() &&
               engine->DictionaryEpoch() > versioned.dictionary_epoch,
           "creating a second session advances the same global Rime generation");
    const auto before_peer_reset = engine->DictionaryEpoch();
    peer->Reset();
    Expect(engine->DictionaryEpoch() > before_peer_reset &&
               peer->DictionaryEpoch() == engine->DictionaryEpoch(),
           "another session's operation invalidates old dictionary metadata");
    const auto before_peer_destroy = engine->DictionaryEpoch();
    peer.reset();
    Expect(engine->DictionaryEpoch() > before_peer_destroy,
           "session destruction cannot flush learning without retiring the old generation");

    engine->Reset();
    for (const wchar_t letter : std::wstring_view(L"nihao")) {
      Expect(engine->ProcessLetter(letter), "Rime should consume learning acceptance input");
    }
    const auto learning_snapshot = engine->Snapshot();
    Expect(!learning_snapshot.candidates.empty() &&
               learning_snapshot.candidates.front().text == L"你好",
           "fresh ordinary session should rank 你好 first for deterministic learning");
    const auto before_learning = engine->DictionaryEpoch();
    Expect(before_learning != 0, "learning starts from a known positive generation");
    Expect(engine->Select(0) == ziliu::core::SelectionResult{true, L"你好"},
           "ordinary Rime session should select 你好 for learning acceptance");
    const auto after_learning = engine->DictionaryEpoch();
    Expect(after_learning != 0 && after_learning > before_learning,
           "real 你好 selection advances a known-positive dictionary generation");

    const auto module = GetModuleHandleW(L"rime.dll");
    Expect(module != nullptr, "real Rime module must remain loaded for deployment test");
    using GetApi = RimeApi*(__cdecl*)();
    const auto procedure = GetProcAddress(module, "rime_get_api");
    Expect(procedure != nullptr, "test should use the same published Rime API");
    auto* api = std::bit_cast<GetApi>(procedure)();
    engine.reset();
    Expect(api->start_maintenance(True) != False, "isolated full-check deployment should start");
    api->join_maintenance_thread();
    engine = ziliu::broker::CreateEngine();
    Expect(engine != nullptr && engine->DictionaryEpoch() == 0,
           "a post-startup deployment must permanently invalidate epoch, even after join");
    for (const wchar_t letter : std::wstring_view(L"nihao")) {
      Expect(engine->ProcessLetter(letter), "ordinary Rime input survives metadata invalidation");
    }
    const auto after_deploy = engine->SnapshotWithDictionaryEpoch();
    Expect(after_deploy.dictionary_epoch == 0 && after_deploy.snapshot.preedit == L"ni'hao" &&
               after_deploy.snapshot.candidates.size() > 2,
           "metadata invalidation must not disable real candidates or silently switch to Stub");
    engine->Reset();
    Expect(engine->DictionaryEpoch() == 0,
           "later synchronous calls cannot revive deployment epoch");
    std::cout << "ziliu_rime_engine_tests: dictionary epoch OK\n";
    return EXIT_SUCCESS;
  }
  if (!test_mode.empty()) {
    Expect(engine != nullptr && engine->DictionaryEpoch() == 0,
           "user Lua extension surface must disable certified dictionary metadata");
    if (test_mode == "--user-lua-self-remove") {
      Expect(!std::filesystem::exists(run_path / "rime.lua"),
             "self-removing Lua override must be vetoed before initialize can execute it");
    }
    for (const wchar_t letter : std::wstring_view(L"nihao")) {
      Expect(engine->ProcessLetter(letter), "user override veto must not disable ordinary input");
    }
    const auto plain = engine->SnapshotWithDictionaryEpoch();
    Expect(plain.dictionary_epoch == 0 && plain.snapshot.preedit == L"ni'hao" &&
               plain.snapshot.candidates.size() > 2,
           "user override veto preserves real Rime candidates, not Stub fallback");
    // Rejection must preserve actual user-dictionary learning, not just input.
    engine->Reset();
    const auto type_shi = [&] {
      for (const wchar_t key : std::wstring_view(L"shi")) {
        Expect(engine->ProcessLetter(key), "ordinary learning input survives metadata veto");
      }
    };
    type_shi();
    const auto before = engine->Snapshot();
    Expect(before.candidates.size() > 5, "learning veto case requires nonfirst candidates");
    const auto learned = before.candidates[5].text;
    engine->Reset();
    for (int attempt = 0; attempt < 8; ++attempt) {
      type_shi();
      const auto page = engine->Snapshot();
      const auto selected = std::ranges::find_if(page.candidates, [&](const auto& candidate) {
        return candidate.text == learned;
      });
      Expect(selected != page.candidates.end(), "learning target remains available");
      Expect(engine->Select(static_cast<std::size_t>(selected - page.candidates.begin())).consumed,
             "ordinary user learning selections must remain consumed");
    }
    type_shi();
    const auto after = engine->SnapshotWithDictionaryEpoch();
    const auto promoted = std::ranges::find_if(after.snapshot.candidates, [&](const auto& candidate) {
      return candidate.text == learned;
    });
    Expect(promoted != after.snapshot.candidates.end() &&
               static_cast<std::size_t>(promoted - after.snapshot.candidates.begin()) < 5 &&
               after.dictionary_epoch == 0 && engine->DictionaryEpoch() == 0,
           "learning must promote the chosen word without reviving contextual metadata");
    std::cout << "ziliu_rime_engine_tests: user override veto OK\n";
    return EXIT_SUCCESS;
  }
  for (const auto& [overlay, expected_timestamp] : overlay_timestamps) {
    std::error_code file_error;
    const auto actual_timestamp = std::filesystem::last_write_time(overlay, file_error);
    Expect(!file_error && actual_timestamp == expected_timestamp,
           "An unchanged Rime overlay should not be rewritten during cold start");
  }
  // Finish ordinary-schema warmup before inspecting restricted input. Its
  // LevelDB WAL is intentionally held exclusively while an ordinary session is open.
  engine.reset();
  const auto user_data_path = UserDataPath();
  Expect(!user_data_path.empty(), "Rime tests require an isolated user-data directory");
  auto restricted_engine = ziliu::broker::CreateEngine(true);
  Expect(restricted_engine != nullptr,
         "the restricted Rime schema must be deployed; do not fall back to ordinary learning");
  restricted_engine->SetCandidatePageSize(7);
  // Session/schema metadata may be prepared when the restricted session opens;
  // freeze the warmed state before any private input to detect input-derived writes.
  const auto before_restricted = SnapshotFiles(user_data_path);
  for (const wchar_t letter : std::wstring_view(L"nihao")) {
    Expect(restricted_engine->ProcessLetter(letter),
           "restricted local composition should consume pinyin");
  }
  const auto restricted_snapshot = restricted_engine->Snapshot();
  Expect(restricted_snapshot.preedit == L"ni'hao" &&
             !restricted_snapshot.candidates.empty() &&
             restricted_snapshot.candidates.front().text == L"你好",
         "restricted schema should use the static Rime Ice Chinese dictionary");
  Expect(restricted_engine->Select(0).consumed,
         "restricted schema should commit a local static-dictionary candidate");
  for (const wchar_t letter : std::wstring_view(L"shi")) {
    Expect(restricted_engine->ProcessLetter(letter),
           "restricted local composition should consume an ambiguous spelling");
  }
  const auto ambiguous_restricted = restricted_engine->Snapshot();
  Expect(ambiguous_restricted.candidates.size() >= 4,
         "restricted schema should expose a nonfirst static candidate");
  Expect(restricted_engine->Select(3).consumed,
         "restricted schema should commit a nonfirst candidate without learning it");
  Expect(SnapshotFiles(user_data_path) == before_restricted,
         "restricted input must not change exact Rime user-data file contents");
  restricted_engine.reset();
  Expect(SnapshotFiles(user_data_path) == before_restricted,
         "destroying a restricted session must not flush private input to user data");
  engine = ziliu::broker::CreateEngine();
  Expect(engine != nullptr, "ordinary Rime session should reopen after restricted input");
  engine->SetCandidatePageSize(7);

  for (const wchar_t letter : std::wstring_view(L"nihao")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume recovery acceptance input");
  }
  const auto recovery_snapshot = engine->Snapshot();
  Expect(recovery_snapshot.preedit == L"ni'hao",
         "the recovery build must preserve automatic pinyin separators");
  Expect(recovery_snapshot.candidates.size() == 7 &&
             recovery_snapshot.candidates.front().text == L"你好",
         "the recovery build must use real Rime candidates, not the two-word Stub");
#if ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST == 0
  const auto production_recovery = engine->SnapshotWithDictionaryEpoch();
  Expect(engine->DictionaryEpoch() == 0 && production_recovery.dictionary_epoch == 0 &&
             production_recovery.snapshot.preedit == recovery_snapshot.preedit &&
             production_recovery.snapshot.candidates == recovery_snapshot.candidates,
         "production scalar and paired epochs stay unavailable with real reopened Rime candidates");
#endif
  engine->Reset();

  // Conversion must operate on words, not a single-character replacement map.
  struct ConversionCase {
    std::wstring_view spelling;
    std::wstring_view simplified;
    std::wstring_view traditional;
    std::wstring_view incorrect_characterwise;
  };
  for (const auto& test : {
           ConversionCase{L"toufa", L"头发", L"頭髮", L"頭發"},
           ConversionCase{L"fazhan", L"发展", L"發展", L"髮展"},
           ConversionCase{L"ganzao", L"干燥", L"乾燥", L"幹燥"},
           ConversionCase{L"ganbu", L"干部", L"幹部", L"乾部"},
           ConversionCase{L"huanghou", L"皇后", L"皇后", L"皇後"},
           ConversionCase{L"houmian", L"后面", L"後面", L"后面"},
           ConversionCase{L"miantiao", L"面条", L"麪條", L"面條"},
           ConversionCase{L"miankong", L"面孔", L"面孔", L"麵孔"}}) {
    for (const wchar_t letter : test.spelling) {
      Expect(engine->ProcessLetter(letter), "Rime should consume conversion input");
    }
    const auto simplified = engine->Snapshot();
    Expect(std::ranges::any_of(simplified.candidates, [&](const auto& candidate) {
             return candidate.text == test.simplified;
           }), "the simplified phrase should be available before toggling");
    engine->SetTraditional(true);
    const auto traditional = engine->Snapshot();
    Expect(std::ranges::none_of(traditional.candidates, [&](const auto& candidate) {
             return candidate.text == test.incorrect_characterwise;
           }), "word-level conversion must not emit the wrong characterwise form");
    const auto converted = std::ranges::find_if(traditional.candidates, [&](const auto& candidate) {
      return candidate.text == test.traditional;
    });
    if (converted == traditional.candidates.end()) {
      std::wcerr << L"Conversion case: " << test.spelling << L'\n';
      for (const auto& candidate : traditional.candidates) {
        std::cerr << "candidate codepoints:";
        for (const wchar_t character : candidate.text) {
          std::cerr << " U+" << std::hex << static_cast<unsigned int>(character);
        }
        std::cerr << std::dec << '\n';
      }
    }
    Expect(converted != traditional.candidates.end(),
           "traditional mode should convert the active phrase with word-level disambiguation");
    const auto index = static_cast<std::size_t>(converted - traditional.candidates.begin());
    Expect(engine->Select(index) == ziliu::core::SelectionResult{true, std::wstring(test.traditional)},
           "committed text must match the selected traditional candidate");
    engine->Reset();
    engine->SetTraditional(false);
    for (const wchar_t letter : test.spelling) {
      Expect(engine->ProcessLetter(letter), "Rime should consume input after switching back");
    }
    const auto restored = engine->Snapshot();
    Expect(std::ranges::any_of(restored.candidates, [&](const auto& candidate) {
             return candidate.text == test.simplified;
           }), "switching back must restore simplified candidates");
    engine->Reset();
  }

  engine->SetTraditional(true);
  for (const wchar_t letter : std::wstring_view(L"fa")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume an ambiguous single-character input");
  }
  const auto ambiguous = engine->Snapshot();
  Expect(std::ranges::any_of(ambiguous.candidates, [](const auto& candidate) {
           return candidate.text == L"發";
         }) && std::ranges::any_of(ambiguous.candidates, [](const auto& candidate) {
           return candidate.text == L"髮";
         }), "an ambiguous single character must retain both traditional choices");
  engine->Reset();
  engine->SetTraditional(false);

  for (const wchar_t letter : std::wstring_view(L"shi")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume a paging test letter");
  }
  const auto first_page = engine->Snapshot();
  Expect(first_page.candidates.size() == 7, "Rime should honor the configured candidate page size");
  Expect(!first_page.has_previous_page && first_page.has_next_page,
         "the first Rime page should expose only a next-page affordance");
  const auto candidate_texts = [](const auto& snapshot) {
    std::vector<std::wstring> texts;
    texts.reserve(snapshot.candidates.size());
    for (const auto& candidate : snapshot.candidates) {
      texts.push_back(candidate.text);
    }
    return texts;
  };
  const auto contains_non_chinese = [](const auto& snapshot) {
    return std::ranges::any_of(snapshot.candidates, [](const auto& candidate) {
      return !ziliu::core::IsChineseCandidate(candidate.text);
    });
  };
  Expect(!contains_non_chinese(first_page),
         "the first visible page should contain only Chinese candidates");
  Expect(engine->PageDown(), "Rime should consume PageDown when more candidates exist");
  const auto second_page = engine->Snapshot();
  Expect(!second_page.candidates.empty(), "PageDown should retain candidate results");
  Expect(second_page.has_previous_page,
         "the second Rime page should expose a previous-page affordance");
  Expect(!contains_non_chinese(second_page),
         "later visible pages should contain only Chinese candidates");
  Expect(candidate_texts(second_page) != candidate_texts(first_page),
         "PageDown should advance to a different candidate page");
  Expect(engine->PageUp(), "Rime should consume PageUp on the second page");
  const auto returned_first_page = engine->Snapshot();
  Expect(candidate_texts(returned_first_page) == candidate_texts(first_page),
         "PageUp should return to the first candidate page");
  Expect(!returned_first_page.has_previous_page && returned_first_page.has_next_page,
         "returning to the first page should restore the pager affordances");
  engine->SetCandidateWindowPageCount(5);
  const auto expanded_first_page = engine->Snapshot();
  Expect(expanded_first_page.candidates.size() > 14 &&
             expanded_first_page.highlighted_index == 0,
         "multi-line paging should return several pages with the first row active");
  Expect(engine->PageDown(), "multi-line paging should advance to the second row");
  const auto expanded_second_page = engine->Snapshot();
  Expect(expanded_second_page.candidates == expanded_first_page.candidates &&
             expanded_second_page.highlighted_index == 7,
         "paging inside a five-row window should retain the group and move the active row");
  engine->SetCandidateWindowPageCount(1);
  engine->Reset();

  for (const wchar_t letter : std::wstring_view(L"no")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume adjacent-key typo input");
  }
  bool found_corrected_candidate = false;
  for (int page = 0; page < 16 && !found_corrected_candidate; ++page) {
    const auto correction_page = engine->Snapshot();
    found_corrected_candidate =
        std::ranges::any_of(correction_page.candidates, [](const auto& candidate) {
          return candidate.text == L"你";
        });
    if (!found_corrected_candidate && !engine->PageDown()) {
      break;
    }
  }
  Expect(found_corrected_candidate,
         "librime adjacent-key correction should offer 你 when i is mistyped as o");
  engine->Reset();

  const auto type_spelling = [&engine](std::wstring_view spelling) {
    for (const wchar_t letter : spelling) {
      Expect(engine->ProcessLetter(letter), "Rime should consume learning test input");
    }
  };
  type_spelling(L"shi");
  const auto learning_baseline = engine->Snapshot();
  const auto before_learning_epoch = engine->DictionaryEpoch();
  Expect(learning_baseline.candidates.size() >= 4,
         "Rime should expose enough candidates for a learning test");
  const std::size_t learned_candidate_index =
      (std::min)(std::size_t{5}, learning_baseline.candidates.size() - 1);
  const std::wstring learned_candidate =
      learning_baseline.candidates[learned_candidate_index].text;
  engine->Reset();
  for (int repetition = 0; repetition < 8; ++repetition) {
    type_spelling(L"shi");
    const auto learning_page = engine->Snapshot();
    const auto candidate =
        std::ranges::find_if(learning_page.candidates, [&learned_candidate](const auto& item) {
          return item.text == learned_candidate;
        });
    Expect(candidate != learning_page.candidates.end(),
           "the selected learning candidate should remain available");
    const auto candidate_index =
        static_cast<std::size_t>(candidate - learning_page.candidates.begin());
    Expect(engine->Select(candidate_index).consumed,
           "Rime should consume a user-learning selection");
  }
  type_spelling(L"shi");
  const auto learned_snapshot = engine->Snapshot();
  const auto after_learning_epoch = engine->DictionaryEpoch();
#if ZILIU_RIME_EPOCH_DIAGNOSTIC_TEST == 0
  const auto production_learning = engine->SnapshotWithDictionaryEpoch();
  Expect(after_learning_epoch == 0 && production_learning.dictionary_epoch == 0 &&
             production_learning.snapshot.candidates == learned_snapshot.candidates,
         "production scalar and paired epochs stay unavailable after real Rime learning");
#endif
  Expect(after_learning_epoch == 0 ||
             (before_learning_epoch != 0 && after_learning_epoch > before_learning_epoch),
         "learning either advances a known generation or conservatively reports unavailable");
  const auto learned_position =
      std::ranges::find_if(learned_snapshot.candidates, [&learned_candidate](const auto& item) {
        return item.text == learned_candidate;
      });
  Expect(learned_position != learned_snapshot.candidates.end(),
         "the learned candidate should remain visible");
  Expect(static_cast<std::size_t>(learned_position - learned_snapshot.candidates.begin()) <
             learned_candidate_index,
         "repeated selections should promote a learned candidate");
  engine->Reset();

  for (const wchar_t letter : std::wstring_view(L"xi")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume manual-split input");
  }
  Expect(engine->ProcessSeparator(), "Rime should consume a manual pinyin separator");
  for (const wchar_t letter : std::wstring_view(L"an")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume input after a manual separator");
  }
  const auto manual_split = engine->Snapshot();
  Expect(manual_split.preedit == L"xi'an",
         "manual pinyin splitting should be displayed with an apostrophe");
  Expect(std::ranges::any_of(manual_split.candidates,
                            [](const auto& candidate) { return candidate.text == L"西安"; }),
         "manual pinyin splitting should retain matching candidates");
  engine->Reset();

  for (const wchar_t letter : std::wstring_view(L"zhongguo")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume a pinyin letter");
  }

  const auto snapshot = engine->Snapshot();
  Expect(snapshot.preedit == L"zhong'guo",
         "automatic pinyin splitting should be displayed with an apostrophe");
  const auto china = std::ranges::find_if(snapshot.candidates, [](const auto& candidate) {
    return candidate.text == L"中国";
  });
  if (china == snapshot.candidates.end()) {
    if (snapshot.candidates.empty() ||
        (snapshot.candidates.size() == 1 && snapshot.candidates.front().text == L"zhongguo")) {
      std::cout << "SKIPPED: verified rime.dll is not staged\n";
      return 77;
    }
    Expect(false, "Rime Ice should offer 中国 for zhongguo");
  }

  const auto candidate_index = static_cast<std::size_t>(china - snapshot.candidates.begin());
  Expect(engine->Select(candidate_index) == ziliu::core::SelectionResult{true, L"中国"},
         "Rime should commit the selected candidate");
  Expect(engine->Snapshot().empty(), "Rime commit should clear the composition");

  for (const wchar_t letter : std::wstring_view(L"zhongguo")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume partial-selection input");
  }
  bool selected_first_character = false;
  for (int page = 0; page < 16 && !selected_first_character; ++page) {
    const auto selection_page = engine->Snapshot();
    const auto first_character =
        std::ranges::find_if(selection_page.candidates, [](const auto& candidate) {
          return candidate.text == L"中";
        });
    if (first_character != selection_page.candidates.end()) {
      const auto first_character_index =
          static_cast<std::size_t>(first_character - selection_page.candidates.begin());
      const auto partial_selection = engine->Select(first_character_index);
      Expect(partial_selection.consumed, "Rime should consume a single-character selection");
      Expect(partial_selection.commit.empty(),
             "Selecting the first character should not commit the remaining first choices");
      const auto remaining = engine->Snapshot();
      Expect(!remaining.empty(), "Partial selection should keep the remaining composition active");
      Expect(remaining.plain_text().starts_with(L"中"),
             "Partial selection should retain the chosen first character in preedit");
      selected_first_character = true;
      break;
    }
    if (!engine->PageDown()) {
      break;
    }
  }
  Expect(selected_first_character, "Rime Ice should offer 中 as a partial candidate for zhongguo");
  engine->Reset();

  for (const wchar_t repeated_letter : std::wstring_view(L"has")) {
    for (int index = 0; index < 63; ++index) {
      Expect(engine->ProcessLetter(repeated_letter),
             "Rime should consume long composition input for every pinyin initial");
    }
    const auto below_limit = engine->Snapshot();
    Expect(below_limit.plain_text().size() == 63,
           "A 63-letter composition should retain all typed input");
    Expect(!below_limit.candidates.empty(),
           "Candidates should remain visible below the 64-letter limit");
    Expect(ziliu::core::IsChineseCandidate(below_limit.candidates.front().text),
           "Automatic Rime segments should remain attached to a Chinese candidate");

    Expect(engine->ProcessLetter(repeated_letter), "Rime should consume the 64th pinyin letter");
    const auto at_limit = engine->Snapshot();
    Expect(at_limit.plain_text().size() == 64,
           "The 64th pinyin letter should remain in the preedit");
    Expect(at_limit.candidates.empty(),
           "Candidates should be hidden at the 64-letter limit");
    Expect(engine->ProcessLetter(repeated_letter),
           "Input beyond the limit should be consumed and rejected");
    Expect(engine->Snapshot().plain_text().size() == 64,
           "Input beyond the limit should not change the preedit");
    Expect(engine->Backspace(), "Backspace should reduce the 64-letter composition");
    const auto restored_below_limit = engine->Snapshot();
    Expect(restored_below_limit.plain_text().size() == 63,
           "Backspace should restore a 63-letter composition");
    Expect(!restored_below_limit.candidates.empty(),
           "Backspace should restore candidates below the limit");
    engine->Reset();
  }

  for (const wchar_t letter : std::wstring_view(L"ziliu")) {
    Expect(engine->ProcessLetter(letter), "Rime should consume the Ziliu spelling");
  }
  const auto ziliu_snapshot = engine->Snapshot();
  Expect(!ziliu_snapshot.candidates.empty(), "Ziliu overlay should offer candidates");
  Expect(ziliu_snapshot.candidates.front().text == L"字流",
         "Ziliu overlay should rank 字流 first");
  const auto ziliu = std::ranges::find_if(ziliu_snapshot.candidates, [](const auto& candidate) {
    return candidate.text == L"字流";
  });
  Expect(ziliu != ziliu_snapshot.candidates.end(), "Ziliu overlay should offer 字流 for ziliu");
  Expect(engine->Select(static_cast<std::size_t>(ziliu - ziliu_snapshot.candidates.begin())) ==
             ziliu::core::SelectionResult{true, L"字流"},
         "Ziliu overlay should commit 字流");
  std::cout << "ziliu_rime_engine_tests: OK\n";
  return EXIT_SUCCESS;
}
