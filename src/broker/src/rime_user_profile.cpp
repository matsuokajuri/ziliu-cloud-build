#include "ziliu/broker/rime_user_profile.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ziliu::broker {
namespace {

constexpr std::size_t kMaximumStateBytes = 64 * 1024;
constexpr std::size_t kMaximumTreeEntries = 32768;
constexpr std::size_t kMaximumTreeDepth = 32;

bool IsReparse(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes == INVALID_FILE_ATTRIBUTES ||
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool HasReparseAncestor(const std::filesystem::path& path) {
  const auto absolute = std::filesystem::absolute(path);
  std::filesystem::path current;
  for (const auto& component : absolute) {
    current /= component;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
  }
  return false;
}

bool IsDirectory(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsRegularFile(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool ReadBounded(const std::filesystem::path& path, std::string* bytes) {
  struct ScopedHandle final {
    explicit ScopedHandle(HANDLE value) : handle(value) {}
    ~ScopedHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HANDLE handle;
  } file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                     OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT |
                                        FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (file.handle == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER size{};
  bool ok = GetFileInformationByHandle(file.handle, &info) && GetFileSizeEx(file.handle, &size) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && size.QuadPart >= 0 &&
            static_cast<std::uint64_t>(size.QuadPart) <= kMaximumStateBytes;
  bytes->clear();
  if (ok) {
    bytes->reserve(static_cast<std::size_t>(size.QuadPart));
    std::array<char, 4096> buffer{};
    for (;;) {
      DWORD read = 0;
      if (!ReadFile(file.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        ok = false;
        break;
      }
      if (read == 0) break;
      if (bytes->size() + read > kMaximumStateBytes) {
        ok = false;
        break;
      }
      bytes->append(buffer.data(), read);
    }
  }
  return ok;
}

bool IsUnsigned(std::string_view value) {
  if (value.empty()) return false;
  std::uint64_t parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') return false;
    const unsigned digit = static_cast<unsigned>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  return true;
}

bool IsSchema(std::string_view value) {
  return value == "rime_ice" || value == "ziliu_private";
}

bool IsOption(std::string_view value) {
  return value == "ascii_punct" || value == "traditionalization" || value == "emoji" ||
         value == "full_shape" || value == "search_single_char";
}

bool ParseFlowMap(std::string_view value, bool access_times) {
  if (value.size() < 2 || value.front() != '{' || value.back() != '}') return false;
  value.remove_prefix(1);
  value.remove_suffix(1);
  std::set<std::string> seen;
  while (!value.empty()) {
    const auto comma = value.find(',');
    auto entry = value.substr(0, comma);
    const auto colon = entry.find(':');
    if (colon == std::string_view::npos || entry.find(':', colon + 1) != std::string_view::npos)
      return false;
    const auto key = entry.substr(0, colon);
    auto item = entry.substr(colon + 1);
    if (item.empty() || item.front() != ' ') return false;
    item.remove_prefix(1);
    if (!item.empty() && item.front() == ' ') return false;
    if (!key.empty() && key.front() == ' ') return false;
    if (key.empty() || !seen.emplace(key).second) return false;
    if (access_times ? (!IsSchema(key) || !IsUnsigned(item))
                     : (!IsOption(key) || (item != "true" && item != "false"))) return false;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
    if (value.empty()) return false;
    if (value.front() == ' ') value.remove_prefix(1);
    if (value.empty() || value.front() == ' ') return false;
  }
  return true;
}

bool ParseNestedMap(const std::vector<std::string_view>& lines, std::size_t* index,
                    bool access_times) {
  std::set<std::string> keys;
  while (*index < lines.size()) {
    const auto line = lines[*index];
    if (line.empty()) { ++*index; continue; }
    if (line.size() < 7 || line.substr(0, 4) != "    ") break;
    const auto colon = line.find(':', 4);
    if (colon == std::string_view::npos || colon == 4 || line.find(':', colon + 1) != std::string_view::npos)
      return false;
    const auto key = line.substr(4, colon - 4);
    auto value = line.substr(colon + 1);
    if (value.empty() || value.front() != ' ') return false;
    value.remove_prefix(1);
    if (value.empty() || value.front() == ' ') return false;
    if (key.find_first_of(" \t{}[]&*!|>'\"%#@`\r\n") != std::string_view::npos ||
        !keys.emplace(key).second) return false;
    if (access_times ? (!IsSchema(key) || !IsUnsigned(value))
                     : (!IsOption(key) || (value != "true" && value != "false"))) return false;
    ++*index;
  }
  return true;
}

bool ParseUserYaml(std::string_view data) {
  if (data.size() > kMaximumStateBytes || data.find('\0') != std::string_view::npos ||
      data.find('\t') != std::string_view::npos) return false;
  if (data.starts_with("\xef\xbb\xbf")) data.remove_prefix(3);
  std::vector<std::string_view> lines;
  while (!data.empty()) {
    const auto end = data.find('\n');
    auto line = data.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.find('\r') != std::string_view::npos) return false;
    if (line.find_first_not_of(' ') != std::string_view::npos &&
        line.find_first_not_of(' ') != 0 && line.find_first_not_of(' ') != 2 &&
        line.find_first_not_of(' ') != 4) return false;
    if (!line.empty() && line.front() == ' ' && line.find_first_not_of(' ') == std::string_view::npos)
      return false;
    lines.push_back(line);
    if (end == std::string_view::npos) { data = {}; break; }
    data.remove_prefix(end + 1);
  }
  if (lines.empty()) return true;
  std::size_t i = 0;
  bool saw_var = false;
  if (lines.back().empty()) lines.pop_back();
  if (lines.empty()) return true;
  if (lines[i++] != "var:") return false;
  saw_var = true;
  std::set<std::string> var_keys;
  while (i < lines.size()) {
    auto line = lines[i];
    if (line.empty()) { ++i; continue; }
    if (line.size() < 4 || line.substr(0, 2) != "  ") return false;
    const auto colon = line.find(':', 2);
    if (colon == std::string_view::npos || colon == 2) return false;
    const auto key = line.substr(2, colon - 2);
    auto value = line.substr(colon + 1);
    if (!value.empty()) {
      if (value.front() != ' ') return false;
      value.remove_prefix(1);
      if (!value.empty() && value.front() == ' ') return false;
    }
    if (key.find_first_of(" \t{}[]&*!|>'\"%#@`\r\n") != std::string_view::npos ||
        !var_keys.emplace(key).second) return false;
    ++i;
    if (key == "last_build_time") {
      if (!IsUnsigned(value)) return false;
    } else if (key == "previously_selected_schema") {
      if (!IsSchema(value)) return false;
    } else if (key == "schema_access_time" || key == "option") {
      const bool access = key == "schema_access_time";
      if (!value.empty()) {
        if (!ParseFlowMap(value, access)) return false;
      } else if (!ParseNestedMap(lines, &i, access)) return false;
    } else {
      return false;
    }
  }
  return saw_var;
}

bool IsUuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'a' && value[i] <= 'f') ||
                 (value[i] >= 'A' && value[i] <= 'F'))) return false;
  }
  return true;
}

bool IsGeneratedTime(std::string_view value) {
  if (value.size() != 24 || value[3] != ' ' || value[7] != ' ' || value[10] != ' ' ||
      value[13] != ':' || value[16] != ':' || value[19] != ' ') return false;
  constexpr std::array<std::string_view, 7> weekdays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  constexpr std::array<std::string_view, 12> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (std::find(weekdays.begin(), weekdays.end(), value.substr(0, 3)) == weekdays.end() ||
      std::find(months.begin(), months.end(), value.substr(4, 3)) == months.end()) return false;
  if (!((value[8] >= '0' && value[8] <= '3') || value[8] == ' ')) return false;
  if (value[8] == ' ' && (value[9] < '1' || value[9] > '9')) return false;
  for (std::size_t i : {9u, 11u, 12u, 14u, 15u, 17u, 18u, 20u, 21u, 22u, 23u})
    if (value[i] < '0' || value[i] > '9') return false;
  const unsigned day = (value[8] == ' ' ? 0u : static_cast<unsigned>(value[8] - '0')) * 10u +
                       static_cast<unsigned>(value[9] - '0');
  const unsigned hour = static_cast<unsigned>(value[11] - '0') * 10u + static_cast<unsigned>(value[12] - '0');
  const unsigned minute = static_cast<unsigned>(value[14] - '0') * 10u + static_cast<unsigned>(value[15] - '0');
  const unsigned second = static_cast<unsigned>(value[17] - '0') * 10u + static_cast<unsigned>(value[18] - '0');
  if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
  return true;
}

bool ParseInstallationYaml(std::string_view data) {
  if (data.size() > kMaximumStateBytes || data.find('\0') != std::string_view::npos ||
      data.find('\t') != std::string_view::npos) return false;
  if (data.starts_with("\xef\xbb\xbf")) data.remove_prefix(3);
  std::set<std::string> keys;
  bool has_id = false;
  std::size_t position = 0;
  while (position < data.size()) {
    const auto end = data.find('\n', position);
    auto line = data.substr(position, end == std::string_view::npos ? data.size() - position : end - position);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) { if (end == std::string_view::npos) break; position = end + 1; continue; }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0 || line.substr(0, colon).find_first_of(" \t") != std::string_view::npos)
      return false;
    const std::string key(line.substr(0, colon));
    if (!keys.emplace(key).second) return false;
    auto value = line.substr(colon + 1);
    if (value.empty() || value.front() != ' ') return false;
    value.remove_prefix(1);
    if (!value.empty() && value.front() == ' ') return false;
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value.remove_prefix(1); value.remove_suffix(1);
    }
    if (key == "distribution_code_name") { if (value != "ziliu") return false; }
    else if (key == "distribution_name") { if (value != "Ziliu") return false; }
    else if (key == "distribution_version") { if (value != "0.1.0-alpha.1") return false; }
    else if (key == "installation_id") { if (!IsUuid(value)) return false; has_id = true; }
    else if (key == "install_time" || key == "update_time") {
      if (!IsGeneratedTime(value)) return false;
    } else if (key == "rime_version") { if (value != "1.17.0") return false; }
    else return false;
    if (end == std::string_view::npos) break;
    position = end + 1;
  }
  return has_id;
}

bool InspectTreeNoReparse(const std::filesystem::path& root) {
  struct Pending { std::filesystem::path path; std::size_t depth; };
  std::vector<Pending> pending{{root, 0}};
  std::size_t entries = 0;
  while (!pending.empty()) {
    auto current = std::move(pending.back());
    pending.pop_back();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(current.path, ec), end; !ec && it != end; it.increment(ec)) {
      if (++entries > kMaximumTreeEntries || IsReparse(it->path())) return false;
      const auto status = it->symlink_status(ec);
      if (ec || std::filesystem::is_symlink(status)) return false;
      if (std::filesystem::is_directory(status)) {
        if (current.depth >= kMaximumTreeDepth) return false;
        pending.push_back({it->path(), current.depth + 1});
      } else if (!std::filesystem::is_regular_file(status)) return false;
    }
    if (ec) return false;
  }
  return true;
}

}  // namespace

RimeUserProfileStatus CheckRimeUserProfile(
    const std::filesystem::path& user_root,
    const std::filesystem::path& pinned_shared_root) noexcept {
  try {
    if (user_root.empty() || !user_root.is_absolute() || pinned_shared_root.empty() ||
        !pinned_shared_root.is_absolute() || HasReparseAncestor(user_root) ||
        HasReparseAncestor(pinned_shared_root) || !IsDirectory(pinned_shared_root))
      return RimeUserProfileStatus::kInvalidRoot;
    const auto shared = user_root.lexically_normal() == pinned_shared_root.lexically_normal();
    if (shared) return RimeUserProfileStatus::kInvalidRoot;
    std::error_code ec;
    if (!std::filesystem::exists(user_root, ec)) {
      return ec ? RimeUserProfileStatus::kInvalidRoot : RimeUserProfileStatus::kValid;
    }
    if (ec || !IsDirectory(user_root)) return RimeUserProfileStatus::kInvalidRoot;

    const std::set<std::wstring> allowed = {L"build", L"rime_ice.userdb", L"user.yaml",
        L"installation.yaml", L"default.custom.yaml", L"rime_ice.custom.yaml"};
    std::set<std::wstring> found;
    std::size_t entries = 0;
    for (std::filesystem::directory_iterator it(user_root, ec), end; !ec && it != end; it.increment(ec)) {
      if (++entries > 64 || IsReparse(it->path())) return RimeUserProfileStatus::kUnsafeEntry;
      const auto name = it->path().filename().native();
      if (!allowed.contains(name) || !found.emplace(name).second) return RimeUserProfileStatus::kUnsafeEntry;
      const auto status = it->symlink_status(ec);
      if (ec || std::filesystem::is_symlink(status)) return RimeUserProfileStatus::kUnsafeEntry;
      if (name == L"build" || name == L"rime_ice.userdb") {
        if (!std::filesystem::is_directory(status) || !InspectTreeNoReparse(it->path()))
          return RimeUserProfileStatus::kUnsafeEntry;
      } else if (!std::filesystem::is_regular_file(status)) return RimeUserProfileStatus::kUnsafeEntry;
    }
    if (ec) return RimeUserProfileStatus::kReadFailure;

    for (const auto* overlay : {L"default.custom.yaml", L"rime_ice.custom.yaml"}) {
      const auto user_file = user_root / overlay;
      const auto shared_file = pinned_shared_root / overlay;
      if (found.contains(overlay)) {
        if (!IsRegularFile(shared_file)) return RimeUserProfileStatus::kCustomMismatch;
        std::string a, b;
        if (!ReadBounded(user_file, &a) || !ReadBounded(shared_file, &b))
          return RimeUserProfileStatus::kReadFailure;
        if (a != b) return RimeUserProfileStatus::kCustomMismatch;
      }
    }
    if (found.contains(L"user.yaml")) {
      std::string bytes;
      if (!ReadBounded(user_root / "user.yaml", &bytes)) return RimeUserProfileStatus::kReadFailure;
      if (!ParseUserYaml(bytes)) return RimeUserProfileStatus::kInvalidUserYaml;
    }
    if (found.contains(L"installation.yaml")) {
      std::string bytes;
      if (!ReadBounded(user_root / "installation.yaml", &bytes)) return RimeUserProfileStatus::kReadFailure;
      if (!ParseInstallationYaml(bytes)) return RimeUserProfileStatus::kInvalidInstallationYaml;
    }
    return RimeUserProfileStatus::kValid;
  } catch (...) {
    return RimeUserProfileStatus::kReadFailure;
  }
}

}  // namespace ziliu::broker
