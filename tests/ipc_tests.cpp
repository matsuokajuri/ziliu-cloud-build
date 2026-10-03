#include "ziliu/core/engine.h"
#include "ziliu/core/ipc_protocol.h"
#include "ziliu/core/settings.h"
#include "ziliu/ipc/pipe_client.h"
#include "ziliu/ipc/pipe_server.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class DelayedEngine final : public ziliu::core::Engine {
 public:
  DelayedEngine() : engine_(ziliu::core::CreateStubEngine()) {}

  void Reset() override { engine_->Reset(); }

  bool ProcessLetter(wchar_t letter) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return engine_->ProcessLetter(letter);
  }

  bool ProcessSeparator() override { return engine_->ProcessSeparator(); }
  bool Backspace() override { return engine_->Backspace(); }
  bool PageUp() override { return engine_->PageUp(); }
  bool PageDown() override { return engine_->PageDown(); }

  void SetCandidatePageSize(std::size_t page_size) override {
    engine_->SetCandidatePageSize(page_size);
  }

  void SetCandidateWindowPageCount(std::size_t page_count) override {
    engine_->SetCandidateWindowPageCount(page_count);
  }

  void SetTraditional(bool enabled) override { engine_->SetTraditional(enabled); }

  void SetChineseCandidatesOnly(bool enabled) override {
    engine_->SetChineseCandidatesOnly(enabled);
  }

  ziliu::core::SelectionResult Select(std::size_t candidate_index) override {
    return engine_->Select(candidate_index);
  }

  [[nodiscard]] ziliu::core::CompositionSnapshot Snapshot() const override {
    return engine_->Snapshot();
  }

 private:
  std::unique_ptr<ziliu::core::Engine> engine_;
};

std::unique_ptr<ziliu::core::Engine> CreateDelayedEngine(bool restricted) {
  static_cast<void>(restricted);
  return std::make_unique<DelayedEngine>();
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::optional<ziliu::core::ipc::Response> ExchangeVersion(
    const std::wstring& pipe_name, const ziliu::core::ipc::Request& request,
    std::uint16_t version) {
  std::vector<std::byte> request_bytes;
  if (!ziliu::core::ipc::EncodeRequest(request, version, &request_bytes) ||
      !WaitNamedPipeW(pipe_name.c_str(), 1000)) {
    return std::nullopt;
  }
  HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
    CloseHandle(pipe);
    return std::nullopt;
  }
  std::vector<std::byte> response_bytes(ziliu::core::ipc::kMaximumMessageBytes);
  DWORD response_size = 0;
  const BOOL exchanged = TransactNamedPipe(
      pipe, request_bytes.data(), static_cast<DWORD>(request_bytes.size()), response_bytes.data(),
      static_cast<DWORD>(response_bytes.size()), &response_size, nullptr);
  CloseHandle(pipe);
  if (!exchanged) {
    return std::nullopt;
  }
  ziliu::core::ipc::Response response;
  std::uint16_t response_version = 0;
  if (!ziliu::core::ipc::DecodeResponse(
          std::span<const std::byte>(response_bytes.data(), response_size), &response,
          &response_version) ||
      response.request_id != request.request_id || response_version != version) {
    return std::nullopt;
  }
  return response;
}

}  // namespace

int main() {
  using ziliu::core::ipc::Command;
  using ziliu::core::ipc::Request;
  using ziliu::core::ipc::Response;
  using ziliu::core::ipc::Status;

  Response expected_pager_response;
  expected_pager_response.request_id = 7;
  expected_pager_response.session_id = 11;
  expected_pager_response.consumed = true;
  expected_pager_response.snapshot.preedit = L"shi";
  expected_pager_response.snapshot.candidates = {{L"是", L"shi", 1.0}};
  expected_pager_response.snapshot.has_previous_page = true;
  expected_pager_response.snapshot.has_next_page = true;
  expected_pager_response.input_revision = 12;
  expected_pager_response.candidate_revision = 19;
  std::vector<std::byte> encoded_pager_response;
  Expect(ziliu::core::ipc::EncodeResponse(expected_pager_response, &encoded_pager_response),
         "pager availability should encode into an IPC response");
  Response decoded_pager_response;
  Expect(ziliu::core::ipc::DecodeResponse(encoded_pager_response, &decoded_pager_response),
         "pager availability should decode from an IPC response");
  Expect(decoded_pager_response.snapshot.has_previous_page &&
             decoded_pager_response.snapshot.has_next_page &&
             decoded_pager_response.input_revision == 12 &&
             decoded_pager_response.candidate_revision == 19,
         "pager flags and v9 revisions should survive the IPC round trip");
  Response v10_response = expected_pager_response;
  v10_response.broker_instance = {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
  std::vector<std::byte> v10_wire;
  Expect(ziliu::core::ipc::EncodeResponse(v10_response, 10, &v10_wire),
         "a v10 response should encode its broker instance tail");
  Response v10_decoded;
  Expect(ziliu::core::ipc::DecodeResponse(v10_wire, &v10_decoded) &&
             v10_decoded.broker_instance == v10_response.broker_instance,
         "both broker instance halves should survive v10 round trip");
  auto truncated_v10_wire = v10_wire;
  truncated_v10_wire.pop_back();
  Response unchanged_v10_response;
  unchanged_v10_response.request_id = 991;
  unchanged_v10_response.broker_instance = {17, 23};
  std::uint16_t unchanged_v10_version = 71;
  Expect(!ziliu::core::ipc::DecodeResponse(truncated_v10_wire, &unchanged_v10_response,
                                           &unchanged_v10_version) &&
             unchanged_v10_response.request_id == 991 &&
             unchanged_v10_response.broker_instance == ziliu::core::BrokerInstanceId{17, 23} &&
             unchanged_v10_version == 71,
         "truncated v10 instance tail should fail without modifying outputs");
  auto missing_v10_tail_wire = v10_wire;
  missing_v10_tail_wire.resize(missing_v10_tail_wire.size() - 16);
  Expect(!ziliu::core::ipc::DecodeResponse(missing_v10_tail_wire, &unchanged_v10_response),
         "a full missing v10 instance tail should be rejected");
  auto trailing_v10_wire = v10_wire;
  trailing_v10_wire.push_back(std::byte{0});
  Expect(!ziliu::core::ipc::DecodeResponse(trailing_v10_wire, &unchanged_v10_response),
         "trailing bytes after v10 instance tail should be rejected");
  for (const auto instance : {ziliu::core::BrokerInstanceId{1, 0},
                               ziliu::core::BrokerInstanceId{0, 1}}) {
    v10_response.broker_instance = instance;
    Expect(ziliu::core::ipc::EncodeResponse(v10_response, 10, &v10_wire) &&
               ziliu::core::ipc::DecodeResponse(v10_wire, &v10_decoded) &&
               v10_decoded.broker_instance == instance,
           "either single nonzero v10 instance half is valid");
  }
  Response unavailable_revisions;
  std::vector<std::byte> unavailable_wire;
  Expect(ziliu::core::ipc::EncodeResponse(unavailable_revisions, &unavailable_wire),
         "a v9 response with both revisions unavailable should encode");
  Response unavailable_decoded;
  Expect(ziliu::core::ipc::DecodeResponse(unavailable_wire, &unavailable_decoded) &&
             unavailable_decoded.input_revision == 0 &&
             unavailable_decoded.candidate_revision == 0,
         "a v9 response with both revisions unavailable should decode");

  Response legacy_identity_source = expected_pager_response;
  legacy_identity_source.broker_instance = {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
  legacy_identity_source.dictionary_epoch = 97;
  for (std::uint16_t version = 4; version <= 9; ++version) {
    std::vector<std::byte> old_wire;
    Expect(ziliu::core::ipc::EncodeResponse(legacy_identity_source, version, &old_wire),
           "legacy response should retain its original wire layout");
    Response old_decoded;
    std::uint16_t old_version = 0;
    Expect(ziliu::core::ipc::DecodeResponse(old_wire, &old_decoded, &old_version) &&
               old_version == version &&
               (version == 9 ? old_decoded.input_revision == expected_pager_response.input_revision &&
                                   old_decoded.candidate_revision ==
                                       expected_pager_response.candidate_revision
                             : old_decoded.input_revision == 0 &&
                                   old_decoded.candidate_revision == 0) &&
               !old_decoded.broker_instance.valid() && old_decoded.dictionary_epoch == 0,
           "v4-v9 responses should preserve revision compatibility and omit instance identity");
  }

  Request state_query{42, 17, Command::kGetSessionState, 0};
  std::vector<std::byte> state_query_wire;
  Expect(ziliu::core::ipc::EncodeRequest(state_query, &state_query_wire),
         "session metadata query should encode in protocol v12");
  Request decoded_state_query;
  Expect(ziliu::core::ipc::DecodeRequest(state_query_wire, &decoded_state_query) &&
             decoded_state_query.command == Command::kGetSessionState,
         "session metadata query should round-trip in protocol v12");
  for (std::uint16_t version = 4; version <= 11; ++version) {
    std::vector<std::byte> old_ping;
    Request decoded_ping;
    Expect(ziliu::core::ipc::EncodeRequest(Request{41, 0, Command::kPing, 0}, version, &old_ping) &&
               ziliu::core::ipc::DecodeRequest(old_ping, &decoded_ping) &&
               decoded_ping.command == Command::kPing,
           "ordinary requests must remain compatible with every v4-v11 peer");
    Expect(!ziliu::core::ipc::EncodeRequest(state_query, version, &old_ping),
           "session metadata query must not encode for a pre-v12 peer");
    auto forged_query = state_query_wire;
    forged_query[4] = static_cast<std::byte>(version & 0xFFU);
    forged_query[5] = static_cast<std::byte>((version >> 8U) & 0xFFU);
    Expect(!ziliu::core::ipc::DecodeRequest(forged_query, &decoded_state_query),
           "forged pre-v12 metadata query must be rejected by the decoder");
  }

  for (const std::uint64_t epoch : {std::uint64_t{0}, std::uint64_t{97}, UINT64_MAX}) {
    Response current = legacy_identity_source;
    current.dictionary_epoch = epoch;
    std::vector<std::byte> wire;
    Response decoded;
    std::uint16_t version = 0;
    Expect(ziliu::core::ipc::EncodeResponse(current, 11, &wire) &&
               ziliu::core::ipc::DecodeResponse(wire, &decoded, &version) && version == 11 &&
               decoded.dictionary_epoch == epoch && decoded.broker_instance == current.broker_instance,
           "v11 preserves unavailable and nonzero dictionary generations without reinterpretation");
    for (std::size_t missing = 1; missing <= sizeof(std::uint64_t); ++missing) {
      auto truncated = wire;
      truncated.resize(truncated.size() - missing);
      decoded.dictionary_epoch = 73;
      version = 79;
      Expect(!ziliu::core::ipc::DecodeResponse(truncated, &decoded, &version) &&
                 decoded.dictionary_epoch == 73 && version == 79,
             "every v11 tail truncation must fail without publishing partial output");
    }
    wire.push_back(std::byte{0});
    Expect(!ziliu::core::ipc::DecodeResponse(wire, &decoded), "v11 trailing data must fail");
    Expect(ziliu::core::ipc::EncodeResponse(current, 10, &wire) &&
               ziliu::core::ipc::DecodeResponse(wire, &decoded, &version) && version == 10 &&
               decoded.dictionary_epoch == 0 && decoded.broker_instance == current.broker_instance,
           "v10 preserves instance but cannot supply a dictionary epoch");
  }

  Response partial_pair = expected_pager_response;
  partial_pair.candidate_revision = 0;
  std::vector<std::byte> malformed_revision_wire;
  Expect(!ziliu::core::ipc::EncodeResponse(partial_pair, 9, &malformed_revision_wire),
         "protocol v9 must reject a partial revision pair on encode");
  Expect(ziliu::core::ipc::EncodeResponse(expected_pager_response, 9, &malformed_revision_wire),
         "valid v9 response should encode for malformed-wire checks");
  Response unchanged_response;
  unchanged_response.request_id = 999;
  unchanged_response.input_revision = 31;
  unchanged_response.candidate_revision = 32;
  auto malformed_pair_wire = malformed_revision_wire;
  std::fill(malformed_pair_wire.end() - 8, malformed_pair_wire.end(), std::byte{0});
  std::uint16_t unchanged_version = 77;
  Expect(!ziliu::core::ipc::DecodeResponse(malformed_pair_wire, &unchanged_response,
                                           &unchanged_version) &&
             unchanged_response.request_id == 999 && unchanged_response.input_revision == 31 &&
             unchanged_response.candidate_revision == 32 && unchanged_version == 77,
         "partial revision pair should fail without changing output parameters");
  auto truncated_v9_wire = malformed_revision_wire;
  truncated_v9_wire.pop_back();
  Expect(!ziliu::core::ipc::DecodeResponse(truncated_v9_wire, &unchanged_response),
         "truncated v9 revision pair must be rejected");
  auto trailing_v9_wire = malformed_revision_wire;
  trailing_v9_wire.push_back(std::byte{0});
  Expect(!ziliu::core::ipc::DecodeResponse(trailing_v9_wire, &unchanged_response),
         "trailing bytes after v9 revisions must be rejected");

  Request legacy_request{41, 0, Command::kPing, 0};
  std::vector<std::byte> encoded_legacy_request;
  Expect(ziliu::core::ipc::EncodeRequest(
             legacy_request,
             ziliu::core::ipc::kOldestCompatibleProtocolVersion,
             &encoded_legacy_request),
         "the current broker should encode the previous compatible request");
  Request decoded_legacy_request;
  std::uint16_t decoded_legacy_request_version = 0;
  Expect(ziliu::core::ipc::DecodeRequest(
             encoded_legacy_request, &decoded_legacy_request,
             &decoded_legacy_request_version) &&
             decoded_legacy_request_version ==
                 ziliu::core::ipc::kOldestCompatibleProtocolVersion &&
             decoded_legacy_request.request_id == legacy_request.request_id,
         "the current broker should accept the previous compatible request");

  Response legacy_response = expected_pager_response;
  legacy_response.request_id = legacy_request.request_id;
  std::vector<std::byte> encoded_legacy_response;
  Expect(ziliu::core::ipc::EncodeResponse(
             legacy_response,
             ziliu::core::ipc::kOldestCompatibleProtocolVersion,
             &encoded_legacy_response),
         "the current broker should encode a response in the caller's version");
  Response decoded_legacy_response;
  std::uint16_t decoded_legacy_response_version = 0;
  Expect(ziliu::core::ipc::DecodeResponse(
             encoded_legacy_response, &decoded_legacy_response,
             &decoded_legacy_response_version) &&
             decoded_legacy_response_version ==
                 ziliu::core::ipc::kOldestCompatibleProtocolVersion &&
             decoded_legacy_response.request_id == legacy_request.request_id &&
             !decoded_legacy_response.snapshot.has_previous_page &&
             !decoded_legacy_response.snapshot.has_next_page,
         "v4 responses should omit pager flags while retaining the shared payload");

  const std::wstring pipe_name =
      L"\\\\.\\pipe\\Ziliu.Tests." + std::to_wstring(GetCurrentProcessId());
  ziliu::core::Settings preferences;
  preferences.input_mode_switch_key = ziliu::core::InputModeSwitchKey::kControl;
  preferences.candidate_layout = ziliu::core::CandidateLayout::kHorizontal;
  preferences.candidate_count = 7;
  preferences.candidate_chinese_font_family = "霞鹜文楷";
  preferences.custom_theme_scale_with_windows = false;
  preferences.active_theme_id = "sogou.test";
  const std::string configured = ziliu::core::SerializeSettings(preferences);
  std::atomic<int> settings_state = 0;
  std::atomic<int> settings_reads = 0;
  std::atomic<int> theme_reads = 0;
  std::atomic<int> menu_reads = 0;
  std::atomic<int> menu_actions = 0;
  ziliu::ipc::PipeServer server(pipe_name, CreateDelayedEngine,
      [&]() -> std::optional<std::string> {
        ++settings_reads;
        if (settings_state.load() == 2) {
          return std::nullopt;
        }
        return settings_state.load() == 0 ? configured : ziliu::core::SerializeSettings({});
      },
      [&](std::string_view theme_id, std::string_view resource,
          std::uint32_t offset) -> std::optional<std::vector<std::byte>> {
        ++theme_reads;
        if (theme_id != "sogou.test") {
          return std::nullopt;
        }
        if (resource.empty() && offset == 0) {
          return std::vector<std::byte>{std::byte{'{'}, std::byte{'}'}};
        }
        if (resource == "assets/a.png" && offset == 0) {
          return std::vector<std::byte>{std::byte{0}, std::byte{0xFF}};
        }
        if (resource == "assets/a.png" && offset == 2) {
          return std::vector<std::byte>{};
        }
        return std::nullopt;
      },
      [&](std::int32_t x, std::int32_t y) {
        ++menu_reads;
        return x == -100 && y == 700;
      },
      [&](std::uint32_t action) {
        ++menu_actions;
        return action == 1 || action == 2;
      });
  std::jthread server_thread([&server] { Expect(server.Run() == 0, "server should stop cleanly"); });
  ziliu::ipc::PipeClient client(pipe_name);

  std::uint64_t request_id = 1;
  std::optional<ziliu::core::ipc::Response> response;
  for (int attempt = 0; attempt < 100 && !response.has_value(); ++attempt) {
    response = client.Exchange(Request{request_id, 0, Command::kCreateSession, 0});
    if (!response.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ++request_id;
  Expect(response.has_value(), "create session should receive a response");
  Expect(response->status == Status::kOk && response->session_id != 0,
         "create session should return an id");
  Expect(response->broker_instance.valid(), "current pipe should publish a valid broker instance");
  Expect(response->dictionary_epoch == 0,
         "unversioned engine must cross the real pipe with unavailable dictionary metadata");
  const auto broker_instance = response->broker_instance;
  Expect(response->input_revision == 1 && response->candidate_revision == 1,
         "v9 pipe should carry actual initial engine revisions");
  const std::uint64_t session_id = response->session_id;
  response = client.Exchange(Request{request_id++, session_id, Command::kGetSessionState, 0});
  Expect(response.has_value() && response->status == Status::kOk &&
             response->input_revision == 1 && response->candidate_revision == 1 &&
             response->dictionary_epoch == 0 && response->snapshot.empty() &&
             !response->consumed && response->commit.empty() &&
             response->broker_instance == broker_instance,
         "real pipe metadata query should return only live scalar state and broker identity");

  const auto legacy_pipe_create = ExchangeVersion(
      pipe_name, Request{request_id++, 0, Command::kCreateSession, 0}, 8);
  Expect(legacy_pipe_create.has_value() && legacy_pipe_create->status == Status::kOk &&
             legacy_pipe_create->input_revision == 0 &&
             legacy_pipe_create->candidate_revision == 0 &&
             !legacy_pipe_create->broker_instance.valid(),
         "a real v8 pipe exchange should preserve legacy framing and omit v10 identity");
  response = client.Exchange(
      Request{request_id++, legacy_pipe_create->session_id, Command::kCloseSession, 0});
  Expect(response.has_value() && response->status == Status::kOk,
         "v9 client should close the session created by a v8 pipe request");
  Expect(response->broker_instance == broker_instance,
         "close response should carry the stable broker instance");

  response = client.Exchange(Request{request_id++, 0, Command::kGetSettings, 0});
  Expect(response.has_value() && response->status == Status::kOk &&
             response->session_id == 0 && response->commit.empty() &&
             ziliu::core::ParseSettings(response->settings_text) == preferences &&
             response->broker_instance == broker_instance,
         "authenticated settings should preserve candidate appearance preferences");
  settings_state = 1;
  response = client.Exchange(Request{request_id++, 0, Command::kGetSettings, 0});
  Expect(response.has_value() && response->status == Status::kOk &&
             ziliu::core::ParseSettings(response->settings_text) == ziliu::core::Settings{},
         "settings changes should be visible without recreating a session");
  settings_state = 2;
  response = client.Exchange(Request{request_id++, 0, Command::kGetSettings, 0});
  Expect(response.has_value() && response->status == Status::kInternalError,
         "settings read failure must not be reported as successful default settings");
  settings_state = 0;

  Request theme_request{request_id++, 0, Command::kGetThemeResource, 0};
  theme_request.theme_id = "sogou.test";
  theme_request.resource = "assets/a.png";
  std::vector<std::byte> theme_wire;
  Expect(!ziliu::core::ipc::EncodeRequest(theme_request, 7, &theme_wire),
         "theme transfer must require protocol v8");
  Expect(ziliu::core::ipc::EncodeRequest(theme_request, &theme_wire),
         "theme transfer should encode in protocol v8");
  Request decoded_theme_request;
  Expect(ziliu::core::ipc::DecodeRequest(theme_wire, &decoded_theme_request) &&
             decoded_theme_request.theme_id == theme_request.theme_id &&
             decoded_theme_request.resource == theme_request.resource,
         "theme identity and resource path should survive the wire round trip");
  response = client.Exchange(theme_request);
  Expect(response.has_value() && response->status == Status::kOk &&
             response->theme_chunk ==
                 std::vector<std::byte>{std::byte{0}, std::byte{0xFF}},
         "raw theme bytes must survive the authenticated pipe");
  theme_request.request_id = request_id++;
  theme_request.value = 2;
  response = client.Exchange(theme_request);
  Expect(response.has_value() && response->status == Status::kOk &&
             response->theme_chunk.empty(), "theme resource EOF should be explicit");
  theme_request.request_id = request_id++;
  theme_request.theme_id = "sogou.other";
  response = client.Exchange(theme_request);
  Expect(response.has_value() && response->status == Status::kInvalidRequest,
         "a resource outside the active theme should not be served");
  Expect(theme_reads.load() == 3, "only authenticated theme requests reach the provider");

  Request menu_request{request_id++, 0, Command::kOpenQuickMenu, 0};
  menu_request.point_x = -100;
  menu_request.point_y = 700;
  Expect(!ziliu::core::ipc::EncodeRequest(menu_request, 7, &theme_wire),
         "quick-menu launch must require protocol v8");
  Expect(ziliu::core::ipc::EncodeRequest(menu_request, &theme_wire),
         "quick-menu launch should encode in protocol v8");
  Request decoded_menu_request;
  Expect(ziliu::core::ipc::DecodeRequest(theme_wire, &decoded_menu_request) &&
             decoded_menu_request.point_x == -100 &&
             decoded_menu_request.point_y == 700,
         "signed monitor coordinates must survive the wire round trip");
  response = client.Exchange(menu_request);
  Expect(response.has_value() && response->status == Status::kOk &&
             menu_reads.load() == 1,
         "authenticated client should be able to open the user-session menu");
  menu_request.request_id = request_id++;
  menu_request.session_id = session_id;
  response = client.Exchange(menu_request);
  Expect(response.has_value() && response->status == Status::kUnsupported &&
             menu_reads.load() == 1,
         "menu launch may not borrow an engine session identifier");

  Request action_request{request_id++, 0, Command::kRunMenuAction, 1};
  Expect(!ziliu::core::ipc::EncodeRequest(action_request, 7, &theme_wire),
         "menu actions require protocol v8");
  response = client.Exchange(action_request);
  Expect(response.has_value() && response->status == Status::kOk &&
             menu_actions.load() == 1,
         "authenticated menu action should reach its provider");
  action_request.request_id = request_id++;
  action_request.value = 3;
  response = client.Exchange(action_request);
  Expect(response.has_value() && response->status == Status::kUnsupported &&
             menu_actions.load() == 1,
         "unknown menu action must not reach its provider");
  action_request.request_id = request_id++;
  action_request.session_id = session_id;
  action_request.value = 2;
  response = client.Exchange(action_request);
  Expect(response.has_value() && response->status == Status::kUnsupported &&
             menu_actions.load() == 1,
         "menu action may not borrow an engine session identifier");

  Response settings_response;
  settings_response.settings_text = configured;
  std::vector<std::byte> settings_bytes;
  Expect(ziliu::core::ipc::EncodeResponse(settings_response, 5, &settings_bytes),
         "old v5 clients should still receive their original wire format");
  Response old_settings_response;
  Expect(ziliu::core::ipc::DecodeResponse(settings_bytes, &old_settings_response) &&
             old_settings_response.settings_text.empty(), "v5 must omit v6 settings payload");
  Expect(ziliu::core::ipc::EncodeResponse(settings_response, &settings_bytes),
         "v6 settings should encode");
  settings_bytes.pop_back();
  Expect(!ziliu::core::ipc::DecodeResponse(settings_bytes, &old_settings_response),
         "truncated settings must be rejected");
  settings_response.settings_text.assign(16385, 'x');
  Expect(!ziliu::core::ipc::EncodeResponse(settings_response, &settings_bytes),
         "settings payload must have a fixed size limit");
  settings_response.settings_text = "\xff";
  Expect(ziliu::core::ipc::EncodeResponse(settings_response, &settings_bytes) &&
             !ziliu::core::ipc::DecodeResponse(settings_bytes, &old_settings_response),
         "invalid UTF-8 settings must be rejected");
  Expect(!ziliu::core::ipc::EncodeRequest(Request{1, 0, Command::kGetSettings, 0}, 5,
                                         &settings_bytes), "settings query requires v6");
  const Request restricted_create{2, 0, Command::kCreateSession, 1};
  Expect(!ziliu::core::ipc::EncodeRequest(restricted_create, 6, &settings_bytes),
         "restricted session requires protocol v7");
  Expect(ziliu::core::ipc::EncodeRequest(restricted_create, &settings_bytes),
         "restricted session should encode in protocol v7");
  settings_bytes[4] = std::byte{6};
  settings_bytes[5] = std::byte{0};
  Request downgraded_restricted;
  Expect(!ziliu::core::ipc::DecodeRequest(settings_bytes, &downgraded_restricted),
         "the current parser must reject a forged v6 restricted-create request");

  std::uint64_t expected_input_revision = 1;
  for (const wchar_t letter : std::wstring_view(L"ziliu")) {
    response = client.Exchange(
        Request{request_id++, session_id, Command::kInputLetter,
                static_cast<std::uint32_t>(letter)});
    Expect(response.has_value(), "a 25 ms letter response should not time out");
    Expect(response->status == Status::kOk, "letter request should keep the session");
    Expect(response->session_id == session_id, "letter response should match the session");
    Expect(response->consumed, "letters should be consumed");
    Expect(response->dictionary_epoch == 0,
           "successful input must not fabricate a dictionary generation in transport");
    ++expected_input_revision;
    Expect(response->input_revision == expected_input_revision &&
               response->candidate_revision == expected_input_revision,
           "v9 pipe should carry the producer's evolving revisions, not request ids");
    Expect(response->broker_instance == broker_instance,
           "every input response should retain the current broker instance");
  }
  Expect(response->snapshot.preedit == L"ziliu", "preedit should cross the pipe");
  Expect(!response->snapshot.candidates.empty() &&
             response->snapshot.candidates.front().text == L"字流",
         "UTF-8 candidates should cross the pipe");
  response = client.Exchange(Request{request_id++, session_id, Command::kGetSessionState, 0});
  Expect(response.has_value() && response->status == Status::kOk &&
             response->input_revision == expected_input_revision &&
             response->candidate_revision == expected_input_revision &&
             response->snapshot.empty() && response->broker_instance == broker_instance,
         "real pipe metadata query should reflect current revisions without composition payload");

  response = client.Exchange(Request{request_id++, session_id, Command::kSelectCandidate, 0});
  Expect(response.has_value() && response->commit == L"字流", "selection should commit 字流");
  Expect(response->snapshot.empty(), "commit should clear the composition");
  Expect(response->input_revision == expected_input_revision + 1 &&
             response->candidate_revision == expected_input_revision + 1,
         "selection should transport advanced revisions with the cleared composition");

  response = client.Exchange(Request{request_id++, session_id, Command::kCloseSession, 0});
  Expect(response.has_value() && response->status == Status::kOk,
         "close session should succeed");
  Expect(response->broker_instance == broker_instance,
         "session close should carry the same broker instance");
  response = client.Exchange(Request{request_id++, session_id, Command::kGetSessionState, 0});
  Expect(response.has_value() && response->status == Status::kSessionNotFound,
         "real pipe metadata query should fail normally after session close");

  // An anonymous SQOS client must not gain access through the AppContainer ACE.
  Expect(WaitNamedPipeW(pipe_name.c_str(), 1000) != FALSE, "pipe should be available");
  HANDLE anonymous = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
      nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
  Expect(anonymous != INVALID_HANDLE_VALUE, "anonymous test should reach authentication");
  DWORD mode = PIPE_READMODE_MESSAGE;
  Expect(SetNamedPipeHandleState(anonymous, &mode, nullptr, nullptr) != FALSE,
         "anonymous test should use message mode");
  std::vector<std::byte> ping;
  const int reads_before_anonymous = settings_reads.load();
  Expect(ziliu::core::ipc::EncodeRequest(Request{request_id++, 0, Command::kGetSettings, 0}, &ping),
         "anonymous ping should encode");
  std::byte reply[256]{};
  DWORD reply_size = 0;
  const BOOL anonymous_result = TransactNamedPipe(anonymous, ping.data(),
      static_cast<DWORD>(ping.size()), reply, sizeof(reply), &reply_size, nullptr);
  CloseHandle(anonymous);
  Expect(!anonymous_result, "anonymous client must be disconnected without an engine response");
  Expect(settings_reads.load() == reads_before_anonymous,
         "anonymous client must not invoke the settings provider");
  response = client.Exchange(Request{request_id++, 0, Command::kPing, 0});
  Expect(response.has_value() && response->status == Status::kOk &&
             response->broker_instance == broker_instance,
         "authorized client should still work after rejecting anonymous client");

  Expect(WaitNamedPipeW(pipe_name.c_str(), 1000) != FALSE,
         "pipe should be available for malformed-request check");
  HANDLE malformed_pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
  Expect(malformed_pipe != INVALID_HANDLE_VALUE, "malformed-request client should connect");
  mode = PIPE_READMODE_MESSAGE;
  Expect(SetNamedPipeHandleState(malformed_pipe, &mode, nullptr, nullptr) != FALSE,
         "malformed-request client should use message mode");
  std::array<std::byte, 1> malformed_request{std::byte{0}};
  std::array<std::byte, 256> malformed_reply{};
  DWORD malformed_reply_size = 0;
  const BOOL malformed_result = TransactNamedPipe(
      malformed_pipe, malformed_request.data(),
      static_cast<DWORD>(malformed_request.size()), malformed_reply.data(),
      static_cast<DWORD>(malformed_reply.size()), &malformed_reply_size, nullptr);
  CloseHandle(malformed_pipe);
  Response malformed_response;
  Expect(malformed_result != FALSE &&
             ziliu::core::ipc::DecodeResponse(
                 std::span<const std::byte>(malformed_reply.data(), malformed_reply_size),
                 &malformed_response) &&
             malformed_response.status == Status::kInvalidRequest &&
             !malformed_response.broker_instance.valid(),
         "malformed requests must not expose the broker instance");

  server.Stop();
  server_thread.join();

  const std::wstring restarted_pipe_name = pipe_name + L".Restart";
  ziliu::ipc::PipeServer restarted_server(restarted_pipe_name, CreateDelayedEngine);
  std::jthread restarted_thread(
      [&restarted_server] { Expect(restarted_server.Run() == 0, "restarted server should stop cleanly"); });
  ziliu::ipc::PipeClient restarted_client(restarted_pipe_name);
  request_id = 1;
  response.reset();
  for (int attempt = 0; attempt < 100 && !response.has_value(); ++attempt) {
    response = restarted_client.Exchange(Request{request_id, 0, Command::kCreateSession, 0});
    if (!response.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  Expect(response.has_value() && response->status == Status::kOk && response->session_id == session_id,
         "a new SessionHost should recreate the same numeric session id");
  const auto restarted_instance = response->broker_instance;
  Expect(restarted_instance.valid() && restarted_instance != broker_instance,
         "a newly constructed default server should have a distinct valid instance id");
  const auto restarted_session_id = response->session_id;
  response = restarted_client.Exchange(Request{request_id++, 0, Command::kPing, 0});
  Expect(response.has_value() && response->broker_instance == restarted_instance,
         "restarted server ping should carry its stable lifetime id");
  response = restarted_client.Exchange(
      Request{request_id++, restarted_session_id, Command::kInputLetter, L'a'});
  Expect(response.has_value() && response->status == Status::kOk &&
             response->broker_instance == restarted_instance,
         "restarted server should carry its lifetime id on ordinary input");
  response = restarted_client.Exchange(
      Request{request_id++, restarted_session_id, Command::kCloseSession, 0});
  Expect(response.has_value() && response->status == Status::kOk &&
             response->broker_instance == restarted_instance,
         "restarted server should carry its lifetime id on session close");
  restarted_server.Stop();
  restarted_thread.join();

  const std::wstring unavailable_pipe_name = pipe_name + L".Unavailable";
  ziliu::ipc::PipeServer unavailable_server(unavailable_pipe_name,
                                             ziliu::core::CreateStubEngineForSession, {}, {}, {}, {},
                                             [] { return ziliu::core::BrokerInstanceId{}; });
  std::jthread unavailable_thread([&unavailable_server] {
    Expect(unavailable_server.Run() == 0, "unavailable-id server should stop cleanly");
  });
  ziliu::ipc::PipeClient unavailable_client(unavailable_pipe_name);
  request_id = 1;
  response.reset();
  for (int attempt = 0; attempt < 100 && !response.has_value(); ++attempt) {
    response = unavailable_client.Exchange(Request{request_id, 0, Command::kCreateSession, 0});
    if (!response.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  Expect(response.has_value() && response->status == Status::kOk &&
             !response->broker_instance.valid() && response->dictionary_epoch == 1,
         "failed instance-id provider should leave identity unavailable");
  const auto unavailable_session_id = response->session_id;
  response = unavailable_client.Exchange(
      Request{request_id + 1, unavailable_session_id, Command::kInputLetter, L'a'});
  Expect(response.has_value() && response->status == Status::kOk &&
             !response->broker_instance.valid() && response->consumed && response->dictionary_epoch == 1,
         "ordinary input should continue when instance-id generation is unavailable");
  const auto v10_create = ExchangeVersion(unavailable_pipe_name,
      Request{request_id + 2, 0, Command::kCreateSession, 0}, 10);
  Expect(v10_create.has_value() && v10_create->status == Status::kOk &&
             v10_create->dictionary_epoch == 0,
         "real v10 exchange must omit even a known immutable dictionary generation");
  unavailable_server.Stop();
  unavailable_thread.join();

  std::cout << "ziliu_ipc_tests: OK\n";
  return EXIT_SUCCESS;
}
