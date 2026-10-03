#include "ziliu/core/engine.h"
#include "ziliu/core/ipc_protocol.h"
#include "ziliu/core/settings.h"
#include "ziliu/core/session_host.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void Type(ziliu::core::Engine& engine, std::wstring_view text) {
  for (const wchar_t letter : text) {
    Expect(engine.ProcessLetter(letter), "letter should be accepted");
  }
}

class UnversionedTestEngine : public ziliu::core::Engine {
 public:
  void Reset() override {}
  bool ProcessLetter(wchar_t) override { return true; }
  bool ProcessSeparator() override { return false; }
  bool Backspace() override { return false; }
  bool PageUp() override { return false; }
  bool PageDown() override { return false; }
  void SetCandidatePageSize(std::size_t) override {}
  void SetCandidateWindowPageCount(std::size_t) override {}
  void SetTraditional(bool) override {}
  void SetChineseCandidatesOnly(bool) override {}
  ziliu::core::SelectionResult Select(std::size_t) override { return {}; }
  ziliu::core::CompositionSnapshot Snapshot() const override { return {}; }
};

// Synthetic shared dictionary, not a Rime mutation source.
class EpochTestEngine final : public UnversionedTestEngine {
 public:
  EpochTestEngine(std::uint64_t& epoch, bool& mutate) : epoch_(epoch), mutate_(mutate) {}
  std::uint64_t DictionaryEpoch() const noexcept override { return epoch_; }
  ziliu::core::CompositionSnapshot Snapshot() const override {
    if (mutate_) ++epoch_;
    return {};
  }
 private:
  std::uint64_t& epoch_;
  bool& mutate_;
};

// A lazy engine may advance its generation while producing the snapshot. The
// paired API, not two independent scalar reads, owns that serialization boundary.
class PairedEpochTestEngine final : public UnversionedTestEngine {
 public:
  std::uint64_t DictionaryEpoch() const noexcept override { return epoch_; }
  ziliu::core::VersionedCompositionSnapshot SnapshotWithDictionaryEpoch() const override {
    ziliu::core::CompositionSnapshot snapshot;
    snapshot.preedit = L"paired";
    return {std::move(snapshot), ++epoch_};
  }
 private:
  mutable std::uint64_t epoch_ = 2;
};

void TestDictionaryAvailability() {
  using namespace ziliu::core;
  using ipc::Command;
  UnversionedTestEngine unsupported;
  Expect(unsupported.DictionaryEpoch() == 0, "new engines default to unavailable dictionary version");
  SessionHost unknown([](bool) { return std::make_unique<UnversionedTestEngine>(); });
  const auto unversioned = unknown.Handle({1, 0, Command::kCreateSession, 0});
  const auto ordinary = unknown.Handle({2, unversioned.session_id, Command::kInputLetter, L'n'});
  Expect(unversioned.dictionary_epoch == 0 && ordinary.dictionary_epoch == 0 &&
             ordinary.status == ipc::Status::kOk && ordinary.consumed,
         "unknown dictionary must not disable ordinary input or acquire a fabricated version");

  std::uint64_t epoch = 17;
  bool mutate = false;
  SessionHost host([&](bool) { return std::make_unique<EpochTestEngine>(epoch, mutate); });
  const auto first = host.Handle({1, 0, Command::kCreateSession, 0});
  const auto second = host.Handle({2, 0, Command::kCreateSession, 0});
  Expect(first.dictionary_epoch == 17 && second.dictionary_epoch == 17,
         "sessions use the actual engine dictionary generation");
  ++epoch;
  mutate = true;
  const auto metadata = host.Handle({3, first.session_id, Command::kGetSessionState, 0});
  Expect(metadata.status == ipc::Status::kOk && metadata.input_revision == 1 &&
             metadata.candidate_revision == 1 && metadata.dictionary_epoch == 18 &&
             metadata.snapshot.empty() && !metadata.consumed && metadata.commit.empty(),
         "metadata query should read live shared epoch and revisions without a snapshot");
  Expect(epoch == 18, "metadata query must not trigger a lazy snapshot-side epoch change");
  mutate = false;
  Expect(host.Handle({4, second.session_id, Command::kGetSessionState, 0}).dictionary_epoch == 18,
         "metadata query must observe a shared dictionary change across sessions");
  auto invalid_metadata = host.Handle({5, first.session_id, Command::kGetSessionState, 1});
  Expect(invalid_metadata.status == ipc::Status::kInvalidRequest,
         "metadata query must reject a nonzero value argument");
  ipc::Request themed_metadata{6, first.session_id, Command::kGetSessionState, 0};
  themed_metadata.theme_id = "forbidden";
  Expect(host.Handle(themed_metadata).status == ipc::Status::kInvalidRequest,
         "metadata query must reject theme/resource arguments");
  ipc::Request positioned_metadata{7, first.session_id, Command::kGetSessionState, 0};
  positioned_metadata.point_x = 1;
  Expect(host.Handle(positioned_metadata).status == ipc::Status::kInvalidRequest,
         "metadata query must reject point arguments");
  Expect(host.Handle({8, 999, Command::kGetSessionState, 0}).status ==
             ipc::Status::kSessionNotFound,
         "metadata query for an unknown session should fail normally");
  Expect(host.Handle({3, first.session_id, Command::kInputLetter, L'n'}).dictionary_epoch == 18 &&
             host.Handle({4, second.session_id, Command::kInputLetter, L'n'}).dictionary_epoch == 18,
         "shared dictionary changes must not be cached per session");
  mutate = true;
  Expect(host.Handle({5, first.session_id, Command::kInputLetter, L'i'}).dictionary_epoch == 0,
         "dictionary change during lazy snapshot invalidates snapshot version");
  epoch = std::numeric_limits<std::uint64_t>::max();
  Expect(host.Handle({5, first.session_id, Command::kPageDown, 0}).dictionary_epoch == 0,
         "version becoming unavailable during snapshot rejects enhancement metadata");
  Expect(host.Handle({5, first.session_id, Command::kPageDown, 0}).dictionary_epoch == 0,
         "availability appearing after snapshot starts cannot certify the earlier snapshot");
  mutate = false;
  epoch = 0;
  Expect(host.Handle({6, first.session_id, Command::kReset, 0}).dictionary_epoch == 0,
         "unavailable engine version stays unavailable in a successful response");
  Expect(host.Handle({7, first.session_id, Command::kCloseSession, 0}).dictionary_epoch == 0 &&
             host.Handle({8, first.session_id, Command::kReset, 0}).dictionary_epoch == 0 &&
             host.Handle({9, 0, Command::kPing, 0}).dictionary_epoch == 0 &&
             host.Handle({10, first.session_id, Command::kGetSessionState, 0}).status ==
                 ipc::Status::kSessionNotFound,
         "closed missing and non-engine responses have no dictionary version");

  SessionHost paired([](bool) { return std::make_unique<PairedEpochTestEngine>(); });
  const auto created = paired.Handle({10, 0, Command::kCreateSession, 0});
  const auto captured = paired.Handle({11, created.session_id, Command::kInputLetter, L'n'});
  Expect(created.dictionary_epoch == 2 && captured.dictionary_epoch == 3 &&
             captured.snapshot.preedit == L"paired",
         "SessionHost must forward the atomically paired snapshot and completed generation");
}

}  // namespace

int main() {
  TestDictionaryAvailability();
  auto engine = ziliu::core::CreateStubEngine();
  Expect(engine->DictionaryEpoch() == 1, "immutable Stub dictionary has a stable generation");

  Type(*engine, L"ZILIU");
  auto snapshot = engine->Snapshot();
  Expect(snapshot.preedit == L"ziliu", "preedit should be normalized to lowercase");
  Expect(!snapshot.candidates.empty(), "ziliu should have candidates");
  Expect(snapshot.candidates.front().text == L"字流", "字流 should be the first candidate");

  const auto selected = engine->Select(0);
  Expect(selected == ziliu::core::SelectionResult{true, L"字流"},
         "select should return the committed candidate");
  Expect(engine->Snapshot().empty(), "selection should clear the composition");
  Expect(engine->DictionaryEpoch() == 1, "Stub selection has no user dictionary learning");

  const ziliu::core::CompositionSnapshot spaced_preedit{L"你 hao", {}, 0};
  Expect(spaced_preedit.plain_text() == L"你hao",
         "plain preedit text should retain selections and remove segmentation spaces");
  const ziliu::core::CompositionSnapshot separated_preedit{L"xi'an", {}, 0};
  Expect(separated_preedit.plain_text() == L"xian",
         "plain preedit text should omit pinyin separators");

  Type(*engine, L"ni");
  Expect(engine->ProcessSeparator(), "manual pinyin separator should be accepted");
  Type(*engine, L"hao");
  snapshot = engine->Snapshot();
  Expect(snapshot.preedit == L"ni'hao" && !snapshot.candidates.empty() &&
             snapshot.candidates.front().text == L"你好",
         "manual pinyin separator should preserve candidate lookup");
  engine->Reset();

  Expect(ziliu::core::IsChineseCandidate(L"中文") &&
             ziliu::core::IsChineseCandidate(L"繁體") &&
             ziliu::core::IsChineseCandidate(L"〇") &&
             ziliu::core::IsChineseCandidate(L"𠀀") &&
             !ziliu::core::IsChineseCandidate(L"") &&
             !ziliu::core::IsChineseCandidate(L"hello") &&
             !ziliu::core::IsChineseCandidate(L"中文A") &&
             !ziliu::core::IsChineseCandidate(L"中文1") &&
             !ziliu::core::IsChineseCandidate(L"中文。") &&
             !ziliu::core::IsChineseCandidate(L"😀"),
         "Chinese candidate detection should accept only Han text");

  Type(*engine, L"nihao");
  Expect(engine->Backspace(), "backspace should consume an existing letter");
  Expect(engine->Snapshot().preedit == L"niha", "backspace should remove one letter");

  engine->Reset();
  Expect(!engine->Backspace(), "backspace should pass through on an empty composition");
  Expect(!engine->ProcessLetter(L'1'), "non-letters should pass through");
  Expect(engine->ProcessLetter(L'x') && engine->Snapshot().candidates.empty(),
         "Chinese-only filtering should be enabled by default");
  engine->SetChineseCandidatesOnly(false);
  Expect(engine->Snapshot().candidates.size() == 1 &&
             engine->Snapshot().candidates.front().text == L"x",
         "disabling Chinese-only filtering should restore non-Chinese candidates");
  engine->SetChineseCandidatesOnly(true);
  Expect(engine->Snapshot().candidates.empty(),
         "re-enabling Chinese-only filtering should immediately update candidates");
  engine->Reset();

  engine->SetChineseCandidatesOnly(false);
  for (std::size_t index = 0; index < ziliu::core::kMaximumPinyinLetters - 1; ++index) {
    Expect(engine->ProcessLetter(L'h'), "input below the pinyin limit should be consumed");
  }
  Expect(engine->Snapshot().preedit.size() == 63 && !engine->Snapshot().candidates.empty(),
         "63 pinyin letters should retain visible candidates");
  Expect(engine->ProcessLetter(L'h'), "the 64th pinyin letter should be consumed");
  Expect(engine->Snapshot().preedit.size() == 64 && engine->Snapshot().candidates.empty(),
         "the 64th pinyin letter should hide candidates");
  Expect(engine->ProcessLetter(L'h') && engine->Snapshot().preedit.size() == 64,
         "pinyin letters beyond the limit should be consumed without being appended");
  Expect(engine->Backspace() && engine->Snapshot().preedit.size() == 63 &&
             !engine->Snapshot().candidates.empty(),
         "backspace to 63 pinyin letters should restore candidates");
  engine->Reset();
  engine->SetChineseCandidatesOnly(true);

  ziliu::core::SessionHost host;
  const auto created = host.Handle({1, 0, ziliu::core::ipc::Command::kCreateSession, 0});
  Expect(created.session_id != 0 && host.session_count() == 1 &&
             created.input_revision == 1 && created.candidate_revision == 1,
         "session host should create an isolated engine");
  const auto input = host.Handle(
      {2, created.session_id, ziliu::core::ipc::Command::kInputLetter, L'z'});
  Expect(input.consumed && input.snapshot.preedit == L"z",
         "session host should route input to its engine");
  Expect(input.input_revision == 2 && input.candidate_revision == 2,
         "letter input should advance both session revisions");
  const auto filter_disabled = host.Handle(
      {3, created.session_id, ziliu::core::ipc::Command::kSetChineseCandidatesOnly, 0});
  Expect(filter_disabled.consumed && filter_disabled.snapshot.candidates.size() == 1 &&
             filter_disabled.snapshot.candidates.front().text == L"z" &&
             filter_disabled.input_revision == 2 && filter_disabled.candidate_revision == 3,
         "session host should apply Chinese-only filtering changes");
  const auto window_pages = host.Handle(
      {4, created.session_id,
       ziliu::core::ipc::Command::kSetCandidateWindowPageCount, 5});
  Expect(window_pages.consumed,
         "session host should accept a five-page candidate window");
  Expect(window_pages.input_revision == 2 && window_pages.candidate_revision == 4,
         "candidate-window configuration should advance only candidate revision");
  const auto invalid_window_pages = host.Handle(
      {5, created.session_id,
       ziliu::core::ipc::Command::kSetCandidateWindowPageCount, 6});
  Expect(invalid_window_pages.status == ziliu::core::ipc::Status::kInvalidRequest,
         "session host should reject candidate windows larger than five pages");
  Expect(invalid_window_pages.input_revision == 0 &&
             invalid_window_pages.candidate_revision == 0,
         "invalid commands should not expose revision metadata");
  const auto separator = host.Handle(
      {6, created.session_id, ziliu::core::ipc::Command::kInputSeparator, 0});
  Expect(separator.consumed && separator.snapshot.preedit == L"z'",
         "session host should route a manual pinyin separator");
  Expect(separator.input_revision == 3 && separator.candidate_revision == 5,
         "separator input should advance both revisions");

  const auto first_reset = host.Handle(
      {7, created.session_id, ziliu::core::ipc::Command::kReset, 0});
  const auto second_reset = host.Handle(
      {8, created.session_id, ziliu::core::ipc::Command::kReset, 0});
  Expect(first_reset.input_revision == 4 && first_reset.candidate_revision == 6 &&
             second_reset.input_revision == 5 && second_reset.candidate_revision == 7,
         "repeated resets should advance both generations, including empty reset");
  const auto empty_backspace = host.Handle(
      {9, created.session_id, ziliu::core::ipc::Command::kBackspace, 0});
  Expect(!empty_backspace.consumed && empty_backspace.input_revision == 6 &&
             empty_backspace.candidate_revision == 8,
         "no-op backspace invocation should advance both generations");
  const auto page = host.Handle(
      {10, created.session_id, ziliu::core::ipc::Command::kPageDown, 0});
  Expect(!page.consumed && page.input_revision == 6 && page.candidate_revision == 9,
         "no-op page operation should advance only candidate generation");
  const auto no_op_select = host.Handle(
      {11, created.session_id, ziliu::core::ipc::Command::kSelectCandidate, 99});
  Expect(!no_op_select.consumed && no_op_select.input_revision == 7 &&
             no_op_select.candidate_revision == 10,
         "no-op selection invocation should advance both generations");
  const auto invalid_character = host.Handle(
      {12, created.session_id, ziliu::core::ipc::Command::kInputLetter, L'1'});
  Expect(invalid_character.status == ziliu::core::ipc::Status::kOk &&
             !invalid_character.consumed && invalid_character.input_revision == 7 &&
             invalid_character.candidate_revision == 10,
         "non-letter input should preserve unchanged revisions without invoking the engine");
  const auto unsupported = host.Handle(
      {13, created.session_id, ziliu::core::ipc::Command::kGetSettings, 0});
  Expect(unsupported.status == ziliu::core::ipc::Status::kUnsupported &&
             unsupported.input_revision == 0 && unsupported.candidate_revision == 0,
         "unsupported session commands should not return revision metadata");
  const auto second_session = host.Handle(
      {14, 0, ziliu::core::ipc::Command::kCreateSession, 0});
  Expect(second_session.input_revision == 1 && second_session.candidate_revision == 1,
         "each session should start its own revision generations");
  const auto close = host.Handle(
      {15, created.session_id, ziliu::core::ipc::Command::kCloseSession, 0});
  Expect(close.input_revision == 0 && close.candidate_revision == 0,
         "close response should not expose revisions");
  const auto recreated = host.Handle(
      {16, 0, ziliu::core::ipc::Command::kCreateSession, 0});
  Expect(recreated.session_id != created.session_id && recreated.input_revision == 1 &&
             recreated.candidate_revision == 1,
         "a recreated session should start fresh generations under a new session id");

  std::uint64_t input_revision = 7;
  std::uint64_t candidate_revision = std::numeric_limits<std::uint64_t>::max();
  Expect(!ziliu::core::AdvanceSessionRevisions(&input_revision, &candidate_revision, false) &&
             input_revision == 0 && candidate_revision == 0,
         "candidate revision overflow should permanently invalidate both revisions");
  input_revision = std::numeric_limits<std::uint64_t>::max() - 1;
  candidate_revision = std::numeric_limits<std::uint64_t>::max() - 1;
  Expect(ziliu::core::AdvanceSessionRevisions(&input_revision, &candidate_revision, true) &&
             input_revision == std::numeric_limits<std::uint64_t>::max() &&
             candidate_revision == std::numeric_limits<std::uint64_t>::max(),
         "final representable revisions remain valid without wrapping");
  input_revision = std::numeric_limits<std::uint64_t>::max();
  candidate_revision = 9;
  Expect(!ziliu::core::AdvanceSessionRevisions(&input_revision, &candidate_revision, true) &&
             input_revision == 0 && candidate_revision == 0,
         "input revision overflow should permanently invalidate both revisions");
  Expect(!ziliu::core::AdvanceSessionRevisions(&input_revision, &candidate_revision, false) &&
             input_revision == 0 && candidate_revision == 0,
         "unavailable revisions should stay unavailable");
  input_revision = 13;
  Expect(!ziliu::core::AdvanceSessionRevisions(&input_revision, &input_revision, true) &&
             input_revision == 0,
         "aliased revision counters should fail closed instead of overflowing");
  std::vector<std::byte> request_bytes;
  const ziliu::core::ipc::Request separator_request{
      7, created.session_id, ziliu::core::ipc::Command::kInputSeparator, 0};
  Expect(ziliu::core::ipc::EncodeRequest(separator_request, &request_bytes),
         "separator request should encode");
  ziliu::core::ipc::Request decoded_request;
  Expect(ziliu::core::ipc::DecodeRequest(request_bytes, &decoded_request) &&
             decoded_request.command == ziliu::core::ipc::Command::kInputSeparator,
         "separator request should survive the IPC round trip");

  ziliu::core::ipc::Response wire_response;
  wire_response.request_id = 9;
  wire_response.session_id = created.session_id;
  wire_response.consumed = true;
  wire_response.commit = L"字流";
  wire_response.snapshot = snapshot;
  wire_response.input_revision = 17;
  wire_response.candidate_revision = 23;
  std::vector<std::byte> bytes;
  Expect(ziliu::core::ipc::EncodeResponse(wire_response, &bytes),
         "response should encode");
  ziliu::core::ipc::Response decoded;
  Expect(ziliu::core::ipc::DecodeResponse(bytes, &decoded), "response should decode");
  Expect(decoded.commit == wire_response.commit &&
             decoded.snapshot.candidates == wire_response.snapshot.candidates &&
             decoded.input_revision == wire_response.input_revision &&
             decoded.candidate_revision == wire_response.candidate_revision,
         "UTF-8 protocol round trip should preserve candidates");

  bool received_restricted = false;
  ziliu::core::SessionHost restricted_host([&received_restricted](bool restricted) {
    received_restricted = restricted;
    return ziliu::core::CreateStubEngine();
  });
  const auto restricted_created = restricted_host.Handle(
      {10, 0, ziliu::core::ipc::Command::kCreateSession, 1});
  Expect(restricted_created.status == ziliu::core::ipc::Status::kOk &&
             restricted_created.session_id != 0 && received_restricted,
         "session host should pass restricted create policy to its engine factory");
  Expect(restricted_host.Handle({11, 0, ziliu::core::ipc::Command::kCreateSession, 2}).status ==
             ziliu::core::ipc::Status::kInvalidRequest,
         "session host should reject unknown create-session policies");

  const ziliu::core::Settings defaults;
  Expect(defaults.candidate_layout == ziliu::core::CandidateLayout::kVertical &&
             defaults.candidate_count == 5 &&
             defaults.input_mode_switch_key == ziliu::core::InputModeSwitchKey::kShift &&
             defaults.punctuation_style == ziliu::core::PunctuationStyle::kFullWidth &&
             defaults.auto_pair_punctuation &&
             defaults.page_key_set == ziliu::core::PageKeySet::kCommaPeriod &&
             defaults.default_input_mode == ziliu::core::DefaultInputMode::kChinese &&
             defaults.chinese_candidates_only &&
             defaults.theme_mode == ziliu::core::ThemeMode::kSystem &&
             defaults.candidate_page_mode == ziliu::core::CandidatePageMode::kSingleLine,
         "settings defaults should match the first-run experience");

  const auto light_palette = ziliu::core::ResolveCandidatePalette(defaults, false);
  const auto dark_palette = ziliu::core::ResolveCandidatePalette(defaults, true);
  Expect(light_palette.candidate_background_color == 0xFAFAFA &&
             light_palette.candidate_text_color == 0x202124 &&
             dark_palette.candidate_background_color == 0x202124 &&
             dark_palette.candidate_text_color == 0xF5F6F7,
         "candidate palettes should follow the selected light or dark mode");
  auto custom_palette_settings = defaults;
  custom_palette_settings.custom_candidate_colors = true;
  custom_palette_settings.candidate_background_color = 0xF7FAFF;
  Expect(ziliu::core::ResolveCandidatePalette(custom_palette_settings, true) == dark_palette,
         "a light custom palette should not force a dark candidate window back to light");
  custom_palette_settings.candidate_background_color = 0x101820;
  custom_palette_settings.preedit_color = 0xE7EDF3;
  custom_palette_settings.highlighted_candidate_color = 0x80C8FF;
  custom_palette_settings.candidate_text_color = 0xDDE7F0;
  const auto custom_dark_palette =
      ziliu::core::ResolveCandidatePalette(custom_palette_settings, true);
  Expect(custom_dark_palette.candidate_background_color == 0x101820 &&
             custom_dark_palette.preedit_color == 0xE7EDF3 &&
             custom_dark_palette.highlighted_candidate_color == 0x80C8FF &&
             custom_dark_palette.candidate_text_color == 0xDDE7F0,
         "a custom palette matching the active mode should remain fully customizable");

  const auto parsed_settings = ziliu::core::ParseSettings(
      "candidate_layout=horizontal\n"
      "candidate_count=7\n"
      "input_mode_switch_key=control\n"
      "punctuation_style=half_width\n"
      "auto_pair_punctuation=false\n"
      "page_keys=brackets\n"
      "character_set=traditional\n"
      "default_input_mode=english\n"
      "chinese_candidates_only=false\n"
      "fuzzy_z_zh=true\n"
      "smart_numeric_punctuation=false\n"
      "theme_mode=dark\n"
      "active_theme_id=org.example.clean\n"
      "candidate_page_mode=multi_line\n"
      "custom_candidate_colors=true\n"
      "preedit_color=#112233\n"
      "highlighted_candidate_color=#245678\n"
      "candidate_text_color=#334455\n"
      "candidate_background_color=#F0F1F2\n"
      "custom_candidate_fonts=true\n"
      "candidate_chinese_font_family=microsoft_yahei\n"
      "candidate_english_font_family=arial\n"
      "custom_candidate_font_size=true\n"
      "candidate_font_size=20\n"
      "candidate_scale_with_text=false\n"
      "custom_theme_scale_with_windows=false\n");
  Expect(parsed_settings.candidate_layout == ziliu::core::CandidateLayout::kHorizontal &&
             parsed_settings.candidate_count == 7 &&
             parsed_settings.input_mode_switch_key ==
                 ziliu::core::InputModeSwitchKey::kControl &&
             parsed_settings.punctuation_style == ziliu::core::PunctuationStyle::kHalfWidth &&
             !parsed_settings.auto_pair_punctuation &&
             parsed_settings.page_key_set == ziliu::core::PageKeySet::kBrackets &&
             parsed_settings.character_set == ziliu::core::CharacterSet::kTraditional &&
             parsed_settings.default_input_mode == ziliu::core::DefaultInputMode::kEnglish &&
             !parsed_settings.chinese_candidates_only &&
             parsed_settings.fuzzy_z_zh && !parsed_settings.smart_numeric_punctuation &&
             parsed_settings.theme_mode == ziliu::core::ThemeMode::kDark &&
             parsed_settings.active_theme_id == "org.example.clean" &&
             parsed_settings.candidate_page_mode ==
                 ziliu::core::CandidatePageMode::kMultiLine &&
             parsed_settings.custom_candidate_colors &&
             parsed_settings.preedit_color == 0x112233 &&
             parsed_settings.highlighted_candidate_color == 0x245678 &&
             parsed_settings.candidate_text_color == 0x334455 &&
             parsed_settings.candidate_background_color == 0xF0F1F2 &&
             parsed_settings.custom_candidate_fonts &&
             parsed_settings.candidate_chinese_font_family == "Microsoft YaHei UI" &&
             parsed_settings.candidate_english_font_family == "Arial" &&
             parsed_settings.custom_candidate_font_size &&
             parsed_settings.candidate_font_size == 20 &&
             !parsed_settings.candidate_scale_with_text &&
             !parsed_settings.custom_theme_scale_with_windows,
         "settings parser should preserve all supported choices");
  Expect(ziliu::core::ParseSettings(ziliu::core::SerializeSettings(parsed_settings)) ==
             parsed_settings,
         "settings should survive a deterministic serialization round trip");
  Expect(ziliu::core::ParseSettings("candidate_layout=horizontal\n")
             .custom_theme_scale_with_windows,
         "older settings files must default custom-theme scaling to on");

  auto appearance_overrides = parsed_settings;
  appearance_overrides.active_theme_id = "org.ziliu.default";
  appearance_overrides.candidate_scale_with_text = true;
  const auto default_theme_effective =
      ziliu::core::ResolveEffectiveCandidateAppearanceSettings(appearance_overrides);
  Expect(default_theme_effective == appearance_overrides,
         "the default theme should preserve every candidate appearance override");

  appearance_overrides.active_theme_id = "sogou.missing-v1";
  const auto persisted_custom_theme = appearance_overrides;
  const auto custom_theme_effective =
      ziliu::core::ResolveEffectiveCandidateAppearanceSettings(appearance_overrides);
  Expect(!custom_theme_effective.custom_candidate_colors &&
             !custom_theme_effective.custom_candidate_fonts &&
             !custom_theme_effective.custom_candidate_font_size &&
             !custom_theme_effective.candidate_scale_with_text,
         "a custom theme identity should disable candidate appearance overrides");
  auto expected_custom_theme = appearance_overrides;
  expected_custom_theme.custom_candidate_colors = false;
  expected_custom_theme.custom_candidate_fonts = false;
  expected_custom_theme.custom_candidate_font_size = false;
  expected_custom_theme.candidate_scale_with_text = false;
  Expect(custom_theme_effective == expected_custom_theme &&
             appearance_overrides == persisted_custom_theme,
         "effective custom-theme settings should preserve values and raw settings");

  appearance_overrides.active_theme_id = "org.ziliu.default";
  const auto restored_default_theme =
      ziliu::core::ResolveEffectiveCandidateAppearanceSettings(appearance_overrides);
  Expect(restored_default_theme == appearance_overrides &&
             restored_default_theme.custom_candidate_colors &&
             restored_default_theme.custom_candidate_fonts &&
             restored_default_theme.custom_candidate_font_size &&
             restored_default_theme.candidate_scale_with_text,
         "returning to the default theme should restore persisted overrides");

  const auto custom_font_settings =
      ziliu::core::ParseSettings("candidate_chinese_font_family=霞鹜文楷\n"
                                 "candidate_english_font_family=IBM Plex Sans\n");
  Expect(custom_font_settings.candidate_chinese_font_family == "霞鹜文楷" &&
             custom_font_settings.candidate_english_font_family == "IBM Plex Sans" &&
             ziliu::core::ParseSettings(
                 ziliu::core::SerializeSettings(custom_font_settings)) == custom_font_settings,
         "settings should preserve installed font family names as UTF-8");
  Expect(ziliu::core::ParseSettings("candidate_count=99\n").candidate_count ==
             ziliu::core::kMaximumCandidateCount,
         "candidate count should be clamped to the supported range");
  Expect(ziliu::core::ParseSettings("candidate_font_size=99\n").candidate_font_size ==
             ziliu::core::kMaximumCandidateFontSize,
         "candidate font size should be clamped to the supported range");
  Expect(ziliu::core::ParseSettings("active_theme_id=../unsafe\n").active_theme_id ==
             "org.ziliu.default",
         "unsafe theme identifiers should fall back to the built-in theme");
  Expect(ziliu::core::MakeCandidatePageSlice(9, 5, 0) ==
             ziliu::core::CandidatePageSlice{0, 5} &&
             ziliu::core::MakeCandidatePageSlice(9, 5, 5) ==
                 ziliu::core::CandidatePageSlice{5, 4} &&
             ziliu::core::MakeCandidatePageSlice(9, 5, 99) ==
                 ziliu::core::CandidatePageSlice{5, 4},
         "candidate pagination should clamp to a stable visible slice");
  Expect(ziliu::core::MakeCandidatePageWindow(32, 7, 9, false) ==
             ziliu::core::CandidatePageWindow{
                 {7, 7}, {7, 7}, 1} &&
             ziliu::core::MakeCandidatePageWindow(32, 7, 9, true) ==
                 ziliu::core::CandidatePageWindow{
                     {7, 7}, {0, 32}, 5} &&
             ziliu::core::MakeCandidatePageWindow(8, 7, 7, true) ==
                 ziliu::core::CandidatePageWindow{
                     {7, 1}, {0, 8}, 2},
         "candidate page windows should collapse to the active row and expand to five rows");

  std::cout << "ziliu_core_tests: OK\n";
  return EXIT_SUCCESS;
}
