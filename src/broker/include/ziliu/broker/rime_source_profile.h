#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ziliu::broker {

enum class RimeSourceProfileStatus {
  kVerifiedSourceFiles,
  kInvalidRoot,
  kReadFailure,
  kUnsafeEntry,
  kLimitExceeded,
  kRuntimeMismatch,
  kDataMismatch,
};

struct RimeSourceProfileResult {
  RimeSourceProfileStatus status = RimeSourceProfileStatus::kReadFailure;
  std::size_t file_count = 0;
  std::string runtime_sha256;
  std::string data_sha256;
};

struct RimeSourceProfileExpected {
  std::string_view runtime_sha256;
  std::string_view data_sha256;
  std::size_t file_count;
};

enum class RimeCompiledProfileStatus {
  kVerifiedCompiledFiles,
  kInvalidRoot,
  kReadFailure,
  kUnsafeEntry,
  kLimitExceeded,
  kMalformedConfig,
  kInvalidArtifact,
  kDataMismatch,
};

struct RimeCompiledProfileResult {
  RimeCompiledProfileStatus status = RimeCompiledProfileStatus::kReadFailure;
  std::size_t file_count = 0;
  std::string data_sha256;
};

struct RimeCompiledProfileExpected {
  std::string_view data_sha256;
  std::size_t file_count;
};

// Bounded, read-only source-file check. This is a point-in-time precondition,
// not a continuous immutability guarantee or complete runtime admission check.
[[nodiscard]] RimeSourceProfileResult CheckRimeSourceProfile(
    const std::filesystem::path& runtime_dll,
    const std::filesystem::path& shared_data,
    const RimeSourceProfileExpected& expected) noexcept;

// Bounded read-only fingerprint of the pinned librime compiled cache. A
// verified result is only a point-in-time precondition, not runtime admission.
[[nodiscard]] RimeCompiledProfileResult CheckRimeCompiledProfile(
    const std::filesystem::path& absolute_build_root,
    const RimeCompiledProfileExpected& expected) noexcept;

}  // namespace ziliu::broker
