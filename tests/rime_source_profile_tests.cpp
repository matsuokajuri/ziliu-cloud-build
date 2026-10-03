#include "ziliu/broker/rime_source_profile.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <algorithm>
#include <iterator>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

namespace {

class Fixture final {
 public:
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp_root = std::filesystem::absolute(std::filesystem::temp_directory_path());
    root_ = temp_root /
            ("ziliu-rime-source-profile-" + std::to_string(stamp));
    runtime_ = root_ / "runtime";
    data_ = root_ / "data";
    build_ = root_ / "compiled";
    if (!std::filesystem::create_directory(root_)) throw std::runtime_error("fixture root collision");
    owns_root_ = true;
    std::filesystem::create_directory(runtime_);
    std::filesystem::create_directory(data_);
    std::filesystem::create_directory(build_);
    Write(runtime_ / "rime.dll", "dll-fixture");
    Write(data_ / "schema.json", "{}");
  }
  ~Fixture() { if (owns_root_) { std::error_code ec; std::filesystem::remove_all(root_, ec); } }
  std::filesystem::path dll() const { return runtime_ / "rime.dll"; }
  const std::filesystem::path& data() const { return data_; }
  const std::filesystem::path& build() const { return build_; }
  const std::filesystem::path& root() const { return root_; }
  std::filesystem::path RuntimeFile(const std::string& name) const { return runtime_ / name; }
  std::filesystem::path DataFile(const std::string& name) const { return data_ / name; }
  static void Write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("could not create fixture file");
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) throw std::runtime_error("could not write fixture file");
  }

 private:
  std::filesystem::path root_;
  std::filesystem::path runtime_;
  std::filesystem::path data_;
  std::filesystem::path build_;
  bool owns_root_ = false;
};

constexpr std::string_view kDllHash =
    "0d8ea22b4ac347f44c619c52341e0107a726afeb28a4ee172e6e09d7ce51e38e";
constexpr std::string_view kDataHash =
    "e842c1cab00e31669835528b63f39dc912ea1d8b9b1cc702afdc33f62878f37c";
constexpr ziliu::broker::RimeSourceProfileExpected kExpected{kDllHash, kDataHash, 1};

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void CheckStatus(ziliu::broker::RimeSourceProfileStatus expected,
                 ziliu::broker::RimeSourceProfileStatus actual, const char* message) {
  Check(expected == actual, message);
}

void CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus expected,
                         ziliu::broker::RimeCompiledProfileStatus actual,
                         const char* message) {
  Check(expected == actual, message);
}

std::uint32_t FixtureCrc32(std::string_view bytes) {
  std::uint32_t crc = 0;
  for (unsigned char value : bytes) {
    crc ^= value;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
  }
  return crc ^ 0xffffffffu;
}

std::string CompiledYaml(const std::string& schema_id, const std::string& time_a = "111",
                         const std::string& time_b = "222") {
  return "__build_info:\r\n"
         "  rime_version: 1.17.0\r\n"
         "  timestamps:\r\n"
         "    default.yaml: " + time_a + "\r\n"
         "    " + schema_id + ".schema.yaml: " + time_b + "\r\n"
         "schema_id: " + schema_id + "\r\n"
         "config_version: 1";
}

void SetFixtureLe32(std::string* bytes, std::size_t offset, std::uint32_t value) {
  Check(bytes->size() >= offset + sizeof(value), "prism fixture header too short");
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    (*bytes)[offset + index] = static_cast<char>((value >> (index * 8)) & 0xffu);
  }
}

std::string CompiledPrism(std::string_view schema, char payload_tag) {
  std::string prism(316, '\0');
  constexpr std::string_view kFormat = "Rime::Prism/4.0";
  prism.replace(0, kFormat.size(), kFormat);
  SetFixtureLe32(&prism, 32, 0x12345678u);
  SetFixtureLe32(&prism, 36, FixtureCrc32(schema));
  SetFixtureLe32(&prism, 40, 7);
  SetFixtureLe32(&prism, 44, 8);
  SetFixtureLe32(&prism, 48, 9);
  prism.append("payload-");
  prism.push_back(payload_tag);
  return prism;
}

std::string ReadFixtureFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("could not read fixture file");
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteCompiledFixture(Fixture& fixture) {
  const std::array<std::pair<const char*, const char*>, 4> schemas = {{{
      "melt_eng", "melt_eng"},
      {"radical_pinyin", "radical_pinyin"},
      {"rime_ice", "rime_ice"},
      {"ziliu_private", "ziliu_private"},
  }};
  Fixture::Write(fixture.build() / "default.yaml", CompiledYaml("melt_eng"));
  for (const auto& [file_stem, schema_id] : schemas) {
    const std::string yaml = CompiledYaml(schema_id);
    Fixture::Write(fixture.build() / (std::string(file_stem) + ".schema.yaml"), yaml);
  }
  const std::string melt_schema = ReadFixtureFile(fixture.build() / "melt_eng.schema.yaml");
  const std::string radical_schema = ReadFixtureFile(fixture.build() / "radical_pinyin.schema.yaml");
  const std::string ice_schema = ReadFixtureFile(fixture.build() / "rime_ice.schema.yaml");
  Fixture::Write(fixture.build() / "melt_eng.prism.bin", CompiledPrism(melt_schema, 'm'));
  Fixture::Write(fixture.build() / "radical_pinyin.prism.bin", CompiledPrism(radical_schema, 'r'));
  Fixture::Write(fixture.build() / "rime_ice.prism.bin", CompiledPrism(ice_schema, 'i'));
  for (const char* stem : {"melt_eng", "radical_pinyin", "rime_ice"}) {
    Fixture::Write(fixture.build() / (std::string(stem) + ".reverse.bin"),
                   std::string("reverse-") + stem);
    Fixture::Write(fixture.build() / (std::string(stem) + ".table.bin"),
                   std::string("table-") + stem);
  }
}

constexpr std::string_view kCompiledFixtureHash =
    "2685d7b5cd9d5526e3e1f2a99dbd31832afe44459ce6dbf50e8e4d06aad67816";
constexpr ziliu::broker::RimeCompiledProfileExpected kCompiledExpected{
    kCompiledFixtureHash, 14};

void RebuildFixturePrisms(Fixture& fixture) {
  const std::array<std::pair<const char*, char>, 3> bindings = {{{
      "melt_eng", 'm'}, {"radical_pinyin", 'r'}, {"rime_ice", 'i'},
  }};
  for (const auto& [stem, tag] : bindings) {
    const std::string schema = ReadFixtureFile(fixture.build() / (std::string(stem) + ".schema.yaml"));
    Fixture::Write(fixture.build() / (std::string(stem) + ".prism.bin"),
                   CompiledPrism(schema, tag));
  }
}

void ChangeFixtureTimestamps(Fixture& fixture) {
  constexpr std::array<const char*, 5> yaml_names = {
      "default.yaml", "melt_eng.schema.yaml", "radical_pinyin.schema.yaml",
      "rime_ice.schema.yaml", "ziliu_private.schema.yaml"};
  for (const char* name : yaml_names) {
    std::string yaml = ReadFixtureFile(fixture.build() / name);
    const std::size_t first = yaml.find("111");
    const std::size_t second = yaml.find("222");
    Check(first != std::string::npos && second != std::string::npos, "fixture timestamps missing");
    yaml.replace(second, 3, "9876543210987654321");
    yaml.replace(first, 3, "18446744073709551615");
    Fixture::Write(fixture.build() / name, yaml);
  }
  RebuildFixturePrisms(fixture);
}

void VerifiesKnownFixtureAndReturnsOnlyDigests() {
  Fixture fixture;
  const auto result = ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected);
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kVerifiedSourceFiles, result.status, "fixture status");
  Check(result.file_count == 1, "fixture file count");
  Check(result.runtime_sha256 == kDllHash, "fixture dll digest");
  Check(result.data_sha256 == kDataHash, "fixture data digest");
}

void DistinguishesDllDataAndCountMismatches() {
  Fixture fixture;
  auto wrong_dll = kExpected;
  wrong_dll.runtime_sha256 = "wrong";
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kRuntimeMismatch,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), wrong_dll).status, "dll mismatch");
  auto wrong_data = kExpected;
  wrong_data.data_sha256 = "wrong";
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kDataMismatch,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), wrong_data).status, "data mismatch");
  wrong_data = kExpected;
  wrong_data.file_count = 2;
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kDataMismatch,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), wrong_data).status, "count mismatch");
}

void ResourceModificationAdditionAndRemovalChangeProfile() {
  Fixture fixture;
  Fixture::Write(fixture.DataFile("schema.json"), "changed");
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kDataMismatch,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status, "modified resource");
  Fixture::Write(fixture.DataFile("schema.json"), "{}");
  Fixture::Write(fixture.DataFile("extra.yaml"), "x");
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kDataMismatch,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status, "added resource");
  std::filesystem::remove(fixture.DataFile("extra.yaml"));
  std::filesystem::remove(fixture.DataFile("schema.json"));
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kDataMismatch,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status, "removed resource");
}

void RejectsCompiledBuildAndUnknownPluginEntries() {
  Fixture fixture;
  std::filesystem::create_directory(fixture.DataFile("build"));
  Fixture::Write(fixture.DataFile("build/override.yaml"), "x");
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kUnsafeEntry,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status, "build override");
  std::filesystem::remove_all(fixture.DataFile("build"));
  std::filesystem::create_directory(fixture.DataFile("others"));
  Fixture::Write(fixture.DataFile("others/plugin.dll"), "x");
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kUnsafeEntry,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status, "plugin binary");
}

void RejectsMissingRelativeAndNonDirectoryRoots() {
  Fixture fixture;
  const std::filesystem::path relative = "relative-rime-data";
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kInvalidRoot,
              ziliu::broker::CheckRimeSourceProfile(relative / "rime.dll", fixture.data(), kExpected).status, "relative root");
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kInvalidRoot,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.RuntimeFile("missing"), kExpected).status, "missing root");
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kInvalidRoot,
              ziliu::broker::CheckRimeSourceProfile(fixture.data(), fixture.data(), kExpected).status, "dll is directory");
}

void RejectsNonRuntimeFilesOutsideDocumentedMetadataLocations() {
  Fixture fixture;
  Fixture::Write(fixture.DataFile("unexpected.bin"), "x");
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kUnsafeEntry,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status, "unknown file");
}

void IncludesUnicodeResourceNamesInUtf8ByteOrder() {
  Fixture fixture;
  std::filesystem::create_directory(fixture.DataFile("others"));
  Fixture::Write(fixture.data() / std::filesystem::path(std::u8string(u8"others/中文.txt")), "zh");
  const ziliu::broker::RimeSourceProfileExpected expected{
      kDllHash, "8060117a6297a0b23fc084ca3d2b2c5d5638d513462e026b97d1781ee8b7cc7d", 2};
  const auto result = ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), expected);
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kVerifiedSourceFiles, result.status, "unicode path profile");
}

void RejectsOversizedDllAndResourceWithoutWritingLargePayloads() {
  Fixture fixture;
  constexpr std::uintmax_t oversized = 128ull * 1024 * 1024 + 1;
  std::filesystem::resize_file(fixture.dll(), oversized);
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kLimitExceeded,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status,
              "oversized dll");

  Fixture resource_fixture;
  std::filesystem::resize_file(resource_fixture.DataFile("schema.json"), oversized);
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kLimitExceeded,
              ziliu::broker::CheckRimeSourceProfile(resource_fixture.dll(), resource_fixture.data(), kExpected).status,
              "oversized resource");
}

void RejectsDirectoryTreesDeeperThanTheBound() {
  Fixture fixture;
  auto directory = fixture.data();
  for (int depth = 1; depth <= 17; ++depth) {
    directory /= "d" + std::to_string(depth);
    std::filesystem::create_directory(directory);
  }
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kLimitExceeded,
              ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status,
              "overly deep tree");
}

void RejectsSourceLockedAgainstNewReaders() {
  Fixture fixture;
  HANDLE lock = CreateFileW(fixture.DataFile("schema.json").c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(lock != INVALID_HANDLE_VALUE, "could not open exclusive fixture handle");
  const auto status = ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status;
  CloseHandle(lock);
  CheckStatus(ziliu::broker::RimeSourceProfileStatus::kReadFailure, status, "locked source file");
}

void RejectsRootAndEntrySymlinksWhenWindowsAllowsCreatingThem() {
  Fixture fixture;
  const auto root_link = fixture.RuntimeFile("data-link");
  const DWORD unprivileged = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (CreateSymbolicLinkW(root_link.c_str(), fixture.data().c_str(),
                          unprivileged | SYMBOLIC_LINK_FLAG_DIRECTORY)) {
    CheckStatus(ziliu::broker::RimeSourceProfileStatus::kInvalidRoot,
                ziliu::broker::CheckRimeSourceProfile(fixture.dll(), root_link, kExpected).status,
                "data root symlink");
  } else {
    std::cout << "SKIP: directory symlink creation unavailable\n";
  }

  const auto entry_link = fixture.DataFile("alias.json");
  if (CreateSymbolicLinkW(entry_link.c_str(), fixture.DataFile("schema.json").c_str(), unprivileged)) {
    CheckStatus(ziliu::broker::RimeSourceProfileStatus::kUnsafeEntry,
                ziliu::broker::CheckRimeSourceProfile(fixture.dll(), fixture.data(), kExpected).status,
                "resource entry symlink");
  } else {
    std::cout << "SKIP: file symlink creation unavailable\n";
  }
}

void VerifiesCompiledFixtureWithFixedDigest() {
  Check(FixtureCrc32("") == 0xffffffffu, "empty CRC-32 vector");
  Check(FixtureCrc32("123456789") == 0xd202d277u, "Rime CRC-32 vector");
  Fixture fixture;
  WriteCompiledFixture(fixture);
  constexpr std::array<const char*, 5> yaml_names = {
      "default.yaml", "melt_eng.schema.yaml", "radical_pinyin.schema.yaml",
      "rime_ice.schema.yaml", "ziliu_private.schema.yaml"};
  for (const char* name : yaml_names)
    Check(ReadFixtureFile(fixture.build() / name).back() != '\n', "fixture must omit final newline");
  const auto result = ziliu::broker::CheckRimeCompiledProfile(fixture.build(), kCompiledExpected);
  if (result.data_sha256 != kCompiledFixtureHash)
    std::cerr << "compiled fixture digest: " << result.data_sha256 << '\n';
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kVerifiedCompiledFiles,
                      result.status, "compiled fixture status");
  Check(result.file_count == 14, "compiled fixture file count");
  Check(result.data_sha256 == kCompiledFixtureHash, "compiled fixture digest");
}

void TimestampOnlyChangesAndSharedPrismChecksumRemainStable() {
  Fixture fixture;
  WriteCompiledFixture(fixture);
  const auto before = ziliu::broker::CheckRimeCompiledProfile(fixture.build(), kCompiledExpected);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kVerifiedCompiledFiles,
                      before.status, "compiled baseline");
  ChangeFixtureTimestamps(fixture);
  const auto after = ziliu::broker::CheckRimeCompiledProfile(fixture.build(), kCompiledExpected);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kVerifiedCompiledFiles,
                      after.status, "timestamp-only compiled profile");
  Check(after.data_sha256 == before.data_sha256, "timestamp-only change altered canonical digest");

  std::string ice_prism = ReadFixtureFile(fixture.build() / "rime_ice.prism.bin");
  const std::string private_schema = ReadFixtureFile(fixture.build() / "ziliu_private.schema.yaml");
  SetFixtureLe32(&ice_prism, 36, FixtureCrc32(private_schema));
  Fixture::Write(fixture.build() / "rime_ice.prism.bin", ice_prism);
  const auto shared = ziliu::broker::CheckRimeCompiledProfile(fixture.build(), kCompiledExpected);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kVerifiedCompiledFiles,
                      shared.status, "shared prism private schema checksum");
}

void RejectsCompiledYamlMetadataAndBodyTampering() {
  Fixture bad_version;
  WriteCompiledFixture(bad_version);
  std::string yaml = ReadFixtureFile(bad_version.build() / "melt_eng.schema.yaml");
  const std::size_t version = yaml.find("1.17.0");
  Check(version != std::string::npos, "fixture version missing");
  yaml.replace(version, 6, "1.17.1");
  Fixture::Write(bad_version.build() / "melt_eng.schema.yaml", yaml);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kMalformedConfig,
                      ziliu::broker::CheckRimeCompiledProfile(bad_version.build(), kCompiledExpected).status,
                      "changed Rime version");

  Fixture duplicate;
  WriteCompiledFixture(duplicate);
  yaml = ReadFixtureFile(duplicate.build() / "default.yaml");
  yaml += "\r\n__build_info:\r\n  rime_version: 1.17.0\r\n";
  Fixture::Write(duplicate.build() / "default.yaml", yaml);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kMalformedConfig,
                      ziliu::broker::CheckRimeCompiledProfile(duplicate.build(), kCompiledExpected).status,
                      "duplicate root metadata");

  Fixture bad_timestamp;
  WriteCompiledFixture(bad_timestamp);
  yaml = ReadFixtureFile(bad_timestamp.build() / "default.yaml");
  const std::size_t timestamp = yaml.find("111");
  Check(timestamp != std::string::npos, "fixture timestamp missing");
  yaml.replace(timestamp, 3, "18446744073709551616");
  Fixture::Write(bad_timestamp.build() / "default.yaml", yaml);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kMalformedConfig,
                      ziliu::broker::CheckRimeCompiledProfile(bad_timestamp.build(), kCompiledExpected).status,
                      "timestamp overflow");

  Fixture body;
  WriteCompiledFixture(body);
  yaml = ReadFixtureFile(body.build() / "melt_eng.schema.yaml");
  yaml.replace(yaml.find("schema_id: melt_eng"), std::string("schema_id: melt_eng").size(),
               "schema_id: changed");
  Fixture::Write(body.build() / "melt_eng.schema.yaml", yaml);
  RebuildFixturePrisms(body);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kDataMismatch,
                      ziliu::broker::CheckRimeCompiledProfile(body.build(), kCompiledExpected).status,
                      "changed schema body with matching prism CRC");
}

void RejectsMalformedTimestampLayoutAndLineEndings() {
  Fixture unsorted;
  WriteCompiledFixture(unsorted);
  std::string yaml = ReadFixtureFile(unsorted.build() / "default.yaml");
  const std::string before = "    default.yaml: 111\r\n    melt_eng.schema.yaml: 222\r\n";
  const std::string after = "    melt_eng.schema.yaml: 222\r\n    default.yaml: 111\r\n";
  const std::size_t block = yaml.find(before);
  Check(block != std::string::npos, "fixture timestamp map missing");
  yaml.replace(block, before.size(), after);
  Fixture::Write(unsorted.build() / "default.yaml", yaml);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kMalformedConfig,
                      ziliu::broker::CheckRimeCompiledProfile(unsorted.build(), kCompiledExpected).status,
                      "unsorted timestamp keys");

  Fixture mixed;
  WriteCompiledFixture(mixed);
  yaml = ReadFixtureFile(mixed.build() / "default.yaml");
  const std::size_t newline = yaml.find("\r\n");
  Check(newline != std::string::npos, "fixture CRLF missing");
  yaml.replace(newline, 2, "\n");
  Fixture::Write(mixed.build() / "default.yaml", yaml);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kMalformedConfig,
                      ziliu::broker::CheckRimeCompiledProfile(mixed.build(), kCompiledExpected).status,
                      "mixed line endings");

  Fixture stray_cr;
  WriteCompiledFixture(stray_cr);
  yaml = ReadFixtureFile(stray_cr.build() / "default.yaml");
  yaml.push_back('\r');
  Fixture::Write(stray_cr.build() / "default.yaml", yaml);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kMalformedConfig,
                      ziliu::broker::CheckRimeCompiledProfile(stray_cr.build(), kCompiledExpected).status,
                      "stray CR byte");

  Fixture added_newline;
  WriteCompiledFixture(added_newline);
  yaml = ReadFixtureFile(added_newline.build() / "default.yaml");
  yaml += "\r\n";
  Fixture::Write(added_newline.build() / "default.yaml", yaml);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kDataMismatch,
                      ziliu::broker::CheckRimeCompiledProfile(added_newline.build(), kCompiledExpected).status,
                      "final newline must remain in fingerprint");
}

void RejectsInvalidPrismAndPreservesAllNonChecksumBytes() {
  Fixture bad_crc;
  WriteCompiledFixture(bad_crc);
  std::string prism = ReadFixtureFile(bad_crc.build() / "melt_eng.prism.bin");
  prism[36] ^= 1;
  Fixture::Write(bad_crc.build() / "melt_eng.prism.bin", prism);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kInvalidArtifact,
                      ziliu::broker::CheckRimeCompiledProfile(bad_crc.build(), kCompiledExpected).status,
                      "invalid prism schema checksum");

  Fixture payload;
  WriteCompiledFixture(payload);
  prism = ReadFixtureFile(payload.build() / "rime_ice.prism.bin");
  prism.back() ^= 1;
  Fixture::Write(payload.build() / "rime_ice.prism.bin", prism);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kDataMismatch,
                      ziliu::broker::CheckRimeCompiledProfile(payload.build(), kCompiledExpected).status,
                      "changed prism payload");

  Fixture header;
  WriteCompiledFixture(header);
  prism = ReadFixtureFile(header.build() / "radical_pinyin.prism.bin");
  prism[0] = 'X';
  Fixture::Write(header.build() / "radical_pinyin.prism.bin", prism);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kInvalidArtifact,
                      ziliu::broker::CheckRimeCompiledProfile(header.build(), kCompiledExpected).status,
                      "invalid prism format");
}

void RejectsCompiledInventoryAndInputLimits() {
  Fixture missing;
  WriteCompiledFixture(missing);
  std::filesystem::remove(missing.build() / "rime_ice.table.bin");
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kDataMismatch,
                      ziliu::broker::CheckRimeCompiledProfile(missing.build(), kCompiledExpected).status,
                      "missing compiled file");

  Fixture extra;
  WriteCompiledFixture(extra);
  Fixture::Write(extra.build() / "user.yaml", "user state must not be read");
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kUnsafeEntry,
                      ziliu::broker::CheckRimeCompiledProfile(extra.build(), kCompiledExpected).status,
                      "unexpected user state file");

  Fixture directory;
  WriteCompiledFixture(directory);
  std::filesystem::create_directory(directory.build() / "nested");
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kUnsafeEntry,
                      ziliu::broker::CheckRimeCompiledProfile(directory.build(), kCompiledExpected).status,
                      "compiled subdirectory");

  Fixture oversized;
  WriteCompiledFixture(oversized);
  std::filesystem::resize_file(oversized.build() / "default.yaml", 128ull * 1024 * 1024 + 1);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kLimitExceeded,
                      ziliu::broker::CheckRimeCompiledProfile(oversized.build(), kCompiledExpected).status,
                      "oversized compiled file");

  Fixture wrong_expected;
  WriteCompiledFixture(wrong_expected);
  auto expected = kCompiledExpected;
  expected.file_count = 13;
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kDataMismatch,
                      ziliu::broker::CheckRimeCompiledProfile(wrong_expected.build(), expected).status,
                      "compiled expected count mismatch");
}

void RejectsInvalidCompiledRootsLocksAndReparseEntries() {
  Fixture fixture;
  WriteCompiledFixture(fixture);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kInvalidRoot,
                      ziliu::broker::CheckRimeCompiledProfile("relative-build", kCompiledExpected).status,
                      "relative compiled root");

  HANDLE lock = CreateFileW((fixture.build() / "default.yaml").c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(lock != INVALID_HANDLE_VALUE, "could not exclusively lock compiled fixture");
  const auto locked_status =
      ziliu::broker::CheckRimeCompiledProfile(fixture.build(), kCompiledExpected).status;
  CloseHandle(lock);
  CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kReadFailure,
                      locked_status, "locked compiled file");

  const auto link = fixture.root() / "compiled-link";
  const DWORD unprivileged = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (CreateSymbolicLinkW(link.c_str(), fixture.build().c_str(),
                          unprivileged | SYMBOLIC_LINK_FLAG_DIRECTORY)) {
    CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kInvalidRoot,
                        ziliu::broker::CheckRimeCompiledProfile(link, kCompiledExpected).status,
                        "compiled root symlink");
  } else {
    std::cout << "SKIP: directory symlink creation unavailable\n";
  }

  const auto file_link = fixture.build() / "rime_ice.table.bin";
  std::filesystem::remove(file_link);
  if (CreateSymbolicLinkW(file_link.c_str(), (fixture.build() / "melt_eng.table.bin").c_str(),
                          unprivileged)) {
    CheckCompiledStatus(ziliu::broker::RimeCompiledProfileStatus::kUnsafeEntry,
                        ziliu::broker::CheckRimeCompiledProfile(fixture.build(), kCompiledExpected).status,
                        "compiled entry symlink");
  } else {
    std::cout << "SKIP: file symlink creation unavailable\n";
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"known fixture", VerifiesKnownFixtureAndReturnsOnlyDigests},
      {"mismatch distinctions", DistinguishesDllDataAndCountMismatches},
      {"resource mutations", ResourceModificationAdditionAndRemovalChangeProfile},
      {"compiled and plugin entries", RejectsCompiledBuildAndUnknownPluginEntries},
      {"invalid roots", RejectsMissingRelativeAndNonDirectoryRoots},
      {"unknown files", RejectsNonRuntimeFilesOutsideDocumentedMetadataLocations},
      {"unicode resource path", IncludesUnicodeResourceNamesInUtf8ByteOrder},
      {"oversized files", RejectsOversizedDllAndResourceWithoutWritingLargePayloads},
      {"directory depth limit", RejectsDirectoryTreesDeeperThanTheBound},
      {"locked source", RejectsSourceLockedAgainstNewReaders},
      {"symlink rejection", RejectsRootAndEntrySymlinksWhenWindowsAllowsCreatingThem},
      {"compiled fixed digest", VerifiesCompiledFixtureWithFixedDigest},
      {"compiled timestamp stability", TimestampOnlyChangesAndSharedPrismChecksumRemainStable},
      {"compiled metadata and body tampering", RejectsCompiledYamlMetadataAndBodyTampering},
      {"compiled YAML syntax", RejectsMalformedTimestampLayoutAndLineEndings},
      {"compiled prism validation", RejectsInvalidPrismAndPreservesAllNonChecksumBytes},
      {"compiled inventory and limits", RejectsCompiledInventoryAndInputLimits},
      {"compiled root and handle safety", RejectsInvalidCompiledRootsLocksAndReparseEntries},
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
