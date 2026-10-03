#include "ziliu/broker/rime_user_profile.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class Fixture final {
 public:
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("ziliu-user-profile-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(stamp));
    if (!std::filesystem::create_directory(root_))
      throw std::runtime_error("could not claim unique fixture root");
    owns_root_ = true;
    user_ = root_ / "user";
    shared_ = root_ / "shared";
    std::filesystem::create_directories(user_);
    std::filesystem::create_directories(shared_);
    Write(shared_ / "default.custom.yaml", "patch: []\n");
    Write(shared_ / "rime_ice.custom.yaml", "patch: []\n");
  }
  ~Fixture() {
    if (owns_root_) { std::error_code ec; std::filesystem::remove_all(root_, ec); }
  }
  const std::filesystem::path& user() const { return user_; }
  const std::filesystem::path& shared() const { return shared_; }
  static void Write(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) throw std::runtime_error("fixture write failed");
  }
  static std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
 private:
  std::filesystem::path root_;
  std::filesystem::path user_;
  std::filesystem::path shared_;
  bool owns_root_ = false;
};

using ziliu::broker::CheckRimeUserProfile;
using ziliu::broker::RimeUserProfileStatus;

void Expect(Fixture& fixture, RimeUserProfileStatus expected, const char* message) {
  Check(CheckRimeUserProfile(fixture.user(), fixture.shared()) == expected, message);
}

void AcceptsEmptyAndGeneratedState() {
  Fixture fixture;
  Expect(fixture, RimeUserProfileStatus::kValid, "empty profile");
  const std::string state =
      "var:\r\n"
      "  last_build_time: 1790523187\r\n"
      "  previously_selected_schema: ziliu_private\r\n"
      "  schema_access_time: {rime_ice: 1790523187, ziliu_private: 1790523150}\r\n"
      "  option: {ascii_punct: true, traditionalization: false, emoji: true, full_shape: false, search_single_char: true}\r\n";
  Fixture::Write(fixture.user() / "user.yaml", state);
  Fixture::Write(fixture.user() / "installation.yaml",
      "distribution_code_name: ziliu\n"
      "distribution_name: Ziliu\n"
      "distribution_version: \"0.1.0-alpha.1\"\n"
      "install_time: \"Sun Sep 27 23:33:07 2026\"\n"
      "installation_id: \"123e4567-e89b-12d3-a456-426614174000\"\n"
      "rime_version: 1.17.0\n");
  std::filesystem::create_directories(fixture.user() / "build");
  Fixture::Write(fixture.user() / "build" / "compiled.bin", "compiled data");
  std::filesystem::create_directories(fixture.user() / "rime_ice.userdb");
  Fixture::Write(fixture.user() / "rime_ice.userdb" / "user.db", "learning data");
  Fixture::Write(fixture.user() / "default.custom.yaml", Fixture::Read(fixture.shared() / "default.custom.yaml"));
  Expect(fixture, RimeUserProfileStatus::kValid, "generated profile");
  Check(Fixture::Read(fixture.user() / "user.yaml") == state, "checker changed user.yaml");
  Check(Fixture::Read(fixture.user() / "rime_ice.userdb" / "user.db") == "learning data",
        "checker changed learning data");
}

void AcceptsBlockMapsAndSpacePaddedTimes() {
  Fixture fixture;
  Fixture::Write(fixture.user() / "user.yaml",
      "var:\n"
      "  previously_selected_schema: rime_ice\n"
      "  schema_access_time:\n"
      "    rime_ice: 1\n"
      "    ziliu_private: 2\n"
      "  option:\n"
      "    ascii_punct: false\n"
      "    emoji: true\n");
  Fixture::Write(fixture.user() / "installation.yaml",
      "installation_id: \"123e4567-e89b-12d3-a456-426614174000\"\n"
      "install_time: \"Mon Sep  7 01:02:03 2026\"\n");
  Expect(fixture, RimeUserProfileStatus::kValid, "block maps or ctime padding rejected");
}

void RejectsUnknownEntriesAndCustomMismatch() {
  Fixture fixture;
  Fixture::Write(fixture.user() / "rogue.lua", "return true");
  Expect(fixture, RimeUserProfileStatus::kUnsafeEntry, "Lua payload rejected");
  std::filesystem::remove(fixture.user() / "rogue.lua");
  Fixture::Write(fixture.user() / "ziliu_private.schema.yaml", "schema: injected\n");
  Expect(fixture, RimeUserProfileStatus::kUnsafeEntry, "schema payload rejected");
  std::filesystem::remove(fixture.user() / "ziliu_private.schema.yaml");
  std::filesystem::create_directory(fixture.user() / "unknown-schema");
  Expect(fixture, RimeUserProfileStatus::kUnsafeEntry, "unknown directory rejected");
  std::filesystem::remove(fixture.user() / "unknown-schema");
  Fixture::Write(fixture.user() / "default.custom.yaml", "patch: [injected]\n");
  Expect(fixture, RimeUserProfileStatus::kCustomMismatch, "custom overlay mismatch");
}

void RejectsMalformedUserYaml() {
  const std::vector<std::string> invalid = {
      "var:\n  option: {ascii_punct: true, ascii_punct: false}\n",
      "var:\n  previously_selected_schema: other_schema\n",
      "var:\n  schema_access_time: {unknown_schema: 1}\n",
      "var:\n  option: {ascii_punct: yes}\n",
      "var:\n  option: {ascii_punct: true}\n  option: {emoji: false}\n",
      "var:\n  option: {ascii_punct: true}\n  evil: true\n",
      "var:\n  option: {ascii_punct: &x true}\n",
      "var:\n  option: {ascii_punct: !tag true}\n",
      "var:\n    option: true\n",
      "var:\n  last_build_time: -1\n",
      "var:\n  last_build_time: 18446744073709551616\n",
      "var:\n  option:\n    emoji: true\n    emoji: false\n",
      "var:\n  option: {emoji:true}\n",
      "var:\n  option:\n    emoji:true\n",
      "var:\n  last_build_time:1\n",
      "var:\n  last_build_time: 1\nvar:\n",
  };
  for (const auto& value : invalid) {
    Fixture fixture;
    Fixture::Write(fixture.user() / "user.yaml", value);
    Expect(fixture, RimeUserProfileStatus::kInvalidUserYaml, "malformed user.yaml accepted");
  }
  Fixture nul_fixture;
  Fixture::Write(nul_fixture.user() / "user.yaml", std::string("var:\n  x: y\0z\n", 14));
  Expect(nul_fixture, RimeUserProfileStatus::kInvalidUserYaml, "NUL user.yaml accepted");
  Fixture tab_fixture;
  Fixture::Write(tab_fixture.user() / "user.yaml", "var:\n\tlast_build_time: 1\n");
  Expect(tab_fixture, RimeUserProfileStatus::kInvalidUserYaml, "tab user.yaml accepted");
  Fixture oversized_fixture;
  Fixture::Write(oversized_fixture.user() / "user.yaml", std::string(64 * 1024 + 1, 'x'));
  Expect(oversized_fixture, RimeUserProfileStatus::kReadFailure, "oversized user.yaml accepted");
  Fixture locked_fixture;
  Fixture::Write(locked_fixture.user() / "user.yaml", "var:\n");
  HANDLE locked = CreateFileW((locked_fixture.user() / "user.yaml").c_str(), GENERIC_READ, 0,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(locked != INVALID_HANDLE_VALUE, "could not acquire exclusive fixture handle");
  Expect(locked_fixture, RimeUserProfileStatus::kReadFailure, "exclusively-open user.yaml accepted");
  CloseHandle(locked);
}

void RejectsOperationalInstallationOverrides() {
  Fixture fixture;
  Fixture::Write(fixture.user() / "installation.yaml",
      "installation_id: \"123e4567-e89b-12d3-a456-426614174000\"\n"
      "sync_dir: C:/outside\n");
  Expect(fixture, RimeUserProfileStatus::kInvalidInstallationYaml, "sync_dir override accepted");
}

void RejectsReparseEntriesWhenAvailable() {
  Fixture fixture;
  std::filesystem::create_directory(fixture.user() / "build");
  const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | SYMBOLIC_LINK_FLAG_DIRECTORY;
  if (!CreateSymbolicLinkW((fixture.user() / "build" / "link").c_str(), fixture.shared().c_str(), flags)) {
    const DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
      std::cout << "SKIP: directory symlink creation unavailable\n";
      return;
    }
    throw std::runtime_error("unexpected directory symlink creation failure");
  }
  Expect(fixture, RimeUserProfileStatus::kUnsafeEntry, "reparse build child accepted");
}

void RejectsReparseRootsWhenAvailable() {
  Fixture fixture;
  const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | SYMBOLIC_LINK_FLAG_DIRECTORY;
  const auto user_link = fixture.user().parent_path() / "user-link";
  if (!CreateSymbolicLinkW(user_link.c_str(), fixture.user().c_str(), flags)) {
    const DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
      std::cout << "SKIP: root symlink creation unavailable\n";
      return;
    }
    throw std::runtime_error("unexpected user root symlink creation failure");
  }
  Check(CheckRimeUserProfile(user_link, fixture.shared()) == RimeUserProfileStatus::kInvalidRoot,
        "reparse user root accepted");
  const auto shared_link = fixture.shared().parent_path() / "shared-link";
  if (!CreateSymbolicLinkW(shared_link.c_str(), fixture.shared().c_str(), flags))
    throw std::runtime_error("could not create shared-root test symlink");
  Check(CheckRimeUserProfile(fixture.user(), shared_link) == RimeUserProfileStatus::kInvalidRoot,
        "reparse shared root accepted");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"empty and generated state", AcceptsEmptyAndGeneratedState},
      {"block maps and ctime padding", AcceptsBlockMapsAndSpacePaddedTimes},
      {"unknown entries and overlay mismatch", RejectsUnknownEntriesAndCustomMismatch},
      {"malformed user yaml", RejectsMalformedUserYaml},
      {"installation overrides", RejectsOperationalInstallationOverrides},
      {"reparse entries", RejectsReparseEntriesWhenAvailable},
      {"reparse roots", RejectsReparseRootsWhenAvailable},
  };
  try {
    for (const auto& [name, test] : tests) {
      test();
      std::cout << "PASS: " << name << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
