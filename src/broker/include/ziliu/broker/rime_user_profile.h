#pragma once

#include <filesystem>

namespace ziliu::broker {

enum class RimeUserProfileStatus {
  kValid,
  kInvalidRoot,
  kUnsafeEntry,
  kReadFailure,
  kInvalidUserYaml,
  kInvalidInstallationYaml,
  kCustomMismatch,
};

// Read-only startup precondition. It validates only the narrowly supported
// user-state surface; build and userdb contents are deliberately not certified.
[[nodiscard]] RimeUserProfileStatus CheckRimeUserProfile(
    const std::filesystem::path& user_root,
    const std::filesystem::path& pinned_shared_root) noexcept;

}  // namespace ziliu::broker
