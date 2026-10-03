#include "ziliu/broker/rime_source_profile.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ziliu::broker {
namespace {

constexpr std::size_t kMaximumEntries = 4096;
constexpr std::uint64_t kMaximumFileBytes = 128ull * 1024 * 1024;
constexpr std::uint64_t kMaximumAggregateBytes = 256ull * 1024 * 1024;
constexpr std::size_t kMaximumDepth = 16;
constexpr std::size_t kCompiledProfileFileCount = 14;
constexpr std::size_t kPrismSchemaChecksumOffset = 36;
constexpr std::size_t kPrismHeaderSize = 32 + 7 * sizeof(std::uint32_t) + 256;
constexpr std::array<std::string_view, kCompiledProfileFileCount> kCompiledProfileFiles = {
    "default.yaml",
    "melt_eng.prism.bin",
    "melt_eng.reverse.bin",
    "melt_eng.schema.yaml",
    "melt_eng.table.bin",
    "radical_pinyin.prism.bin",
    "radical_pinyin.reverse.bin",
    "radical_pinyin.schema.yaml",
    "radical_pinyin.table.bin",
    "rime_ice.prism.bin",
    "rime_ice.reverse.bin",
    "rime_ice.schema.yaml",
    "rime_ice.table.bin",
    "ziliu_private.schema.yaml",
};

class Sha256 final {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
      return;
    DWORD object_size = 0;
    DWORD returned = 0;
    if (BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                          &returned, 0) < 0 || object_size == 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      algorithm_ = nullptr;
      return;
    }
    if (object_size > object_.size()) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      algorithm_ = nullptr;
      return;
    }
    if (BCryptCreateHash(algorithm_, &hash_, object_.data(), object_size, nullptr, 0, 0) < 0) {
      BCryptCloseAlgorithmProvider(algorithm_, 0);
      algorithm_ = nullptr;
    }
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  bool good() const { return hash_ != nullptr; }
  bool Update(const void* bytes, std::size_t size) {
    return BCryptHashData(hash_, const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)),
                          static_cast<ULONG>(size), 0) >= 0;
  }
  bool Finish(std::string* output) {
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
      return false;
    static constexpr char kHex[] = "0123456789abcdef";
    output->clear();
    output->reserve(64);
    for (UCHAR value : digest) {
      output->push_back(kHex[value >> 4]);
      output->push_back(kHex[value & 0x0f]);
    }
    return true;
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  std::array<UCHAR, 4096> object_{};
};

class ScopedHandle final {
 public:
  explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
  ~ScopedHandle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  HANDLE get() const { return handle_; }
 private:
  HANDLE handle_;
};

bool IsReparse(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool HasReparseAncestor(const std::filesystem::path& path) {
  std::filesystem::path absolute = std::filesystem::absolute(path);
  std::filesystem::path current;
  for (const auto& part : absolute) {
    current /= part;
    if (IsReparse(current)) return true;
  }
  return false;
}

bool ToUtf8Relative(const std::filesystem::path& relative, std::string* output) {
  const auto encoded = relative.generic_u8string();
  output->clear();
  output->reserve(encoded.size());
  for (char8_t value : encoded) {
    output->push_back(static_cast<char>(value));
  }
  return true;
}

struct TextLine final {
  std::size_t begin = 0;
  std::size_t content_size = 0;
  std::size_t end = 0;
};

bool SplitStrictLines(std::string_view bytes, std::vector<TextLine>* lines) {
  if (bytes.empty()) return false;
  bool saw_line_ending = false;
  bool crlf = false;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    const std::size_t newline = bytes.find('\n', begin);
    if (newline == std::string_view::npos) {
      const std::string_view final_line = bytes.substr(begin);
      if (final_line.find('\r') != std::string_view::npos) return false;
      lines->push_back({begin, final_line.size(), bytes.size()});
      break;
    }
    const bool current_crlf = newline > begin && bytes[newline - 1] == '\r';
    if (saw_line_ending && current_crlf != crlf) return false;
    saw_line_ending = true;
    crlf = current_crlf;
    const std::size_t content_end = current_crlf ? newline - 1 : newline;
    if (bytes.substr(begin, content_end - begin).find('\r') != std::string_view::npos) return false;
    lines->push_back({begin, content_end - begin, newline + 1});
    begin = newline + 1;
  }
  return saw_line_ending && !lines->empty();
}

std::string_view LineText(std::string_view bytes, const TextLine& line) {
  return bytes.substr(line.begin, line.content_size);
}

bool IsLegalResourceKey(std::string_view key) {
  if (key.empty() || key.front() == '/' || key.back() == '/') return false;
  std::size_t segment_begin = 0;
  for (std::size_t index = 0; index <= key.size(); ++index) {
    if (index == key.size() || key[index] == '/') {
      const std::string_view segment = key.substr(segment_begin, index - segment_begin);
      if (segment.empty() || segment == "." || segment == "..") return false;
      segment_begin = index + 1;
      continue;
    }
    const unsigned char value = static_cast<unsigned char>(key[index]);
    const bool ascii_letter = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    const bool ascii_digit = value >= '0' && value <= '9';
    if (!ascii_letter && !ascii_digit && value != '_' && value != '-' && value != '.') return false;
  }
  return true;
}

bool NormalizeCompiledYaml(std::string_view bytes, std::string* normalized) {
  std::vector<TextLine> lines;
  if (!SplitStrictLines(bytes, &lines) || LineText(bytes, lines.front()) != "__build_info:") return false;
  if (lines.size() < 5 || LineText(bytes, lines[1]) != "  rime_version: 1.17.0" ||
      LineText(bytes, lines[2]) != "  timestamps:") return false;

  std::vector<std::pair<std::size_t, std::size_t>> timestamp_values;
  std::string previous_key;
  std::size_t body_line = lines.size();
  for (std::size_t index = 3; index < lines.size(); ++index) {
    const std::string_view line = LineText(bytes, lines[index]);
    if (line.starts_with("    ")) {
      const std::string_view entry = line.substr(4);
      const std::size_t separator = entry.find(": ");
      if (separator == std::string_view::npos) return false;
      const std::string_view key = entry.substr(0, separator);
      const std::string_view value = entry.substr(separator + 2);
      if (!IsLegalResourceKey(key) || (!previous_key.empty() && key <= previous_key) || value.empty()) return false;
      std::uint64_t timestamp = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), timestamp, 10);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
      previous_key.assign(key);
      const std::size_t value_offset = lines[index].begin + 4 + separator + 2;
      timestamp_values.emplace_back(value_offset, value.size());
      continue;
    }
    if (line.empty() || line.front() == ' ' || line.front() == '\t') return false;
    body_line = index;
    break;
  }
  if (timestamp_values.empty() || body_line == lines.size()) return false;

  bool has_body_root = false;
  for (std::size_t index = body_line; index < lines.size(); ++index) {
    const std::string_view line = LineText(bytes, lines[index]);
    if (line.empty()) continue;
    if (line.front() == ' ' || line.front() == '\t') continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    const std::string_view key = line.substr(0, colon);
    if (key == "__build_info") return false;
    has_body_root = true;
  }
  if (!has_body_root) return false;

  *normalized = std::string(bytes);
  for (auto it = timestamp_values.rbegin(); it != timestamp_values.rend(); ++it) {
    normalized->replace(it->first, it->second, "0");
  }
  return true;
}

std::uint32_t RimeCrc32(std::string_view bytes) {
  std::uint32_t crc = 0;
  for (unsigned char value : bytes) {
    crc ^= value;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return crc ^ 0xffffffffu;
}

std::uint32_t ReadLittleEndian32(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

bool HasPrismHeader(std::string_view bytes) {
  if (bytes.size() < kPrismHeaderSize) return false;
  constexpr std::string_view kFormat = "Rime::Prism/4.0";
  if (bytes.substr(0, kFormat.size()) != kFormat || bytes[kFormat.size()] != '\0') return false;
  for (std::size_t index = kFormat.size() + 1; index < 32; ++index) {
    if (bytes[index] != '\0') return false;
  }
  return true;
}

bool IsLowerHexDigest(std::string_view digest) {
  if (digest.size() != 64) return false;
  return std::all_of(digest.begin(), digest.end(), [](char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  });
}

RimeCompiledProfileStatus ReadCompiledFile(const std::filesystem::path& path,
                                           std::string* bytes,
                                           std::uint64_t* aggregate_bytes) {
  if (IsReparse(path)) return RimeCompiledProfileStatus::kUnsafeEntry;
  ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) return RimeCompiledProfileStatus::kReadFailure;
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER opened_size{};
  if (!GetFileInformationByHandle(file.get(), &info) || !GetFileSizeEx(file.get(), &opened_size))
    return RimeCompiledProfileStatus::kReadFailure;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return RimeCompiledProfileStatus::kUnsafeEntry;
  if (opened_size.QuadPart < 0 || static_cast<std::uint64_t>(opened_size.QuadPart) > kMaximumFileBytes)
    return RimeCompiledProfileStatus::kLimitExceeded;

  bytes->clear();
  bytes->reserve(static_cast<std::size_t>(opened_size.QuadPart));
  std::array<char, 64 * 1024> buffer{};
  std::uint64_t file_bytes = 0;
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
      return RimeCompiledProfileStatus::kReadFailure;
    if (read == 0) break;
    file_bytes += read;
    *aggregate_bytes += read;
    if (file_bytes > kMaximumFileBytes || *aggregate_bytes > kMaximumAggregateBytes)
      return RimeCompiledProfileStatus::kLimitExceeded;
    bytes->append(buffer.data(), read);
  }
  if (file_bytes != static_cast<std::uint64_t>(opened_size.QuadPart))
    return RimeCompiledProfileStatus::kReadFailure;
  return RimeCompiledProfileStatus::kVerifiedCompiledFiles;
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
  return value;
}

bool IsRuntimeExtension(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  static const std::set<std::string> allowed = {
      ".json", ".lua", ".opencc", ".png", ".tsv", ".txt", ".webp", ".yaml", ".yml", ".db"};
  return allowed.contains(LowerAscii(path.substr(dot)));
}

bool IsExecutableExtension(const std::string& path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  const std::string ext = LowerAscii(path.substr(dot));
  return ext == ".dll" || ext == ".exe" || ext == ".so" || ext == ".sys" || ext == ".com" ||
         ext == ".cmd" || ext == ".bat" || ext == ".ps1" || ext == ".py" || ext == ".pyd" ||
         ext == ".dylib";
}

bool IsAllowedMetadata(const std::string& path) {
  if (path == ".gitignore" || path == "AGENTS.md" || path == "LICENSE" ||
      path == "README.md" || path == "build/.gitkeep") return true;
  if (!path.starts_with("others/") || IsExecutableExtension(path)) return false;
  const auto slash = path.find_last_of('/');
  const auto dot = path.find_last_of('.');
  const std::string basename = path.substr(slash == std::string::npos ? 0 : slash + 1);
  const std::string extension = dot == std::string::npos ? "" : LowerAscii(path.substr(dot));
  return basename == "Makefile" || basename == ".luacheckrc" || extension == ".md" ||
         extension == ".html" || extension == ".go" || extension == ".mod" ||
         extension == ".sum" || extension == ".sh";
}

RimeSourceProfileStatus HashRoot(const std::filesystem::path& root,
                                 std::string* result_hash, std::size_t* file_count) {
  std::error_code ec;
  if (root.empty() || !root.is_absolute() || HasReparseAncestor(root))
    return RimeSourceProfileStatus::kInvalidRoot;
  const auto root_status = std::filesystem::status(root, ec);
  if (ec || !std::filesystem::is_directory(root_status)) return RimeSourceProfileStatus::kInvalidRoot;

  std::vector<std::pair<std::string, std::filesystem::path>> files;
  std::vector<std::wstring> folded;
  std::size_t entries = 0;
  std::uint64_t aggregate_bytes = 0;
  std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, ec), end;
  if (ec) return RimeSourceProfileStatus::kReadFailure;
  for (; it != end; it.increment(ec)) {
    if (ec) return RimeSourceProfileStatus::kReadFailure;
    if (++entries > kMaximumEntries) return RimeSourceProfileStatus::kLimitExceeded;
    const auto rel = it->path().lexically_relative(root);
    std::string name;
    if (!ToUtf8Relative(rel, &name)) return RimeSourceProfileStatus::kUnsafeEntry;
    const auto slash = name.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "" : name.substr(0, slash);
    const std::string filename = slash == std::string::npos ? name : name.substr(slash + 1);
    if (IsReparse(it->path())) return RimeSourceProfileStatus::kUnsafeEntry;
    if (parent.empty() && (filename == ".git" || filename == ".github")) {
      it.disable_recursion_pending();
      continue;
    }
    const auto status = it->symlink_status(ec);
    if (ec) return RimeSourceProfileStatus::kReadFailure;
    if (std::filesystem::is_directory(status)) {
      std::size_t depth = static_cast<std::size_t>(std::distance(rel.begin(), rel.end()));
      if (depth > kMaximumDepth) return RimeSourceProfileStatus::kLimitExceeded;
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) return RimeSourceProfileStatus::kUnsafeEntry;
    if ((name.starts_with("build/") || name == "build") && name != "build/.gitkeep")
      return RimeSourceProfileStatus::kUnsafeEntry;
    if (!IsRuntimeExtension(name)) {
      if (!IsAllowedMetadata(name)) return RimeSourceProfileStatus::kUnsafeEntry;
      continue;
    }
    const std::wstring wide_name = rel.generic_wstring();
    for (const auto& prior : folded) {
      if (CompareStringOrdinal(wide_name.data(), static_cast<int>(wide_name.size()),
                               prior.data(), static_cast<int>(prior.size()), TRUE) == CSTR_EQUAL)
        return RimeSourceProfileStatus::kUnsafeEntry;
    }
    folded.push_back(wide_name);
    const auto size = std::filesystem::file_size(it->path(), ec);
    if (ec) return RimeSourceProfileStatus::kReadFailure;
    if (size > kMaximumFileBytes || aggregate_bytes > kMaximumAggregateBytes - size)
      return RimeSourceProfileStatus::kLimitExceeded;
    aggregate_bytes += size;
    files.emplace_back(std::move(name), it->path());
  }
  if (ec) return RimeSourceProfileStatus::kReadFailure;
  // Python's pin generation sorts UTF-8 path bytes; compare unsigned bytes for
  // the same deterministic ordering regardless of whether char is signed.
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                                        [](unsigned char x, unsigned char y) { return x < y; });
  });

  Sha256 aggregate;
  if (!aggregate.good()) return RimeSourceProfileStatus::kReadFailure;
  std::uint64_t actual_aggregate_bytes = 0;
  for (const auto& [name, path] : files) {
    ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return RimeSourceProfileStatus::kReadFailure;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER opened_size{};
    if (!GetFileInformationByHandle(file.get(), &info) || !GetFileSizeEx(file.get(), &opened_size) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      return RimeSourceProfileStatus::kUnsafeEntry;
    }
    if (opened_size.QuadPart < 0 || static_cast<std::uint64_t>(opened_size.QuadPart) > kMaximumFileBytes)
      return RimeSourceProfileStatus::kLimitExceeded;
    Sha256 individual;
    if (!individual.good()) return RimeSourceProfileStatus::kReadFailure;
    std::array<UCHAR, 64 * 1024> buffer{};
    std::uint64_t read_total = 0;
    for (;;) {
      DWORD read = 0;
      if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        return RimeSourceProfileStatus::kReadFailure;
      }
      if (read == 0) break;
      read_total += read;
      actual_aggregate_bytes += read;
      if (read_total > kMaximumFileBytes || actual_aggregate_bytes > kMaximumAggregateBytes ||
          !individual.Update(buffer.data(), read)) {
        return read_total > kMaximumFileBytes ? RimeSourceProfileStatus::kLimitExceeded
             : actual_aggregate_bytes > kMaximumAggregateBytes ? RimeSourceProfileStatus::kLimitExceeded
                                                               : RimeSourceProfileStatus::kReadFailure;
      }
    }
    std::string digest;
    if (!individual.Finish(&digest)) return RimeSourceProfileStatus::kReadFailure;
    const std::string record = name + " " + digest + "\n";
    if (!aggregate.Update(record.data(), record.size())) return RimeSourceProfileStatus::kReadFailure;
  }
  if (!aggregate.Finish(result_hash)) return RimeSourceProfileStatus::kReadFailure;
  *file_count = files.size();
  return RimeSourceProfileStatus::kVerifiedSourceFiles;
}

}  // namespace

RimeSourceProfileResult CheckRimeSourceProfile(
    const std::filesystem::path& runtime_dll, const std::filesystem::path& shared_data,
    const RimeSourceProfileExpected& expected) noexcept {
  RimeSourceProfileResult result;
  try {
    std::error_code ec;
    if (runtime_dll.empty() || !runtime_dll.is_absolute() || HasReparseAncestor(runtime_dll)) {
      result.status = RimeSourceProfileStatus::kInvalidRoot;
      return result;
    }
    const auto dll_status = std::filesystem::symlink_status(runtime_dll, ec);
    if (ec || !std::filesystem::is_regular_file(dll_status) || IsReparse(runtime_dll)) {
      result.status = RimeSourceProfileStatus::kInvalidRoot;
      return result;
    }
    const auto dll_size = std::filesystem::file_size(runtime_dll, ec);
    if (ec) { result.status = RimeSourceProfileStatus::kReadFailure; return result; }
    if (dll_size > kMaximumFileBytes) { result.status = RimeSourceProfileStatus::kLimitExceeded; return result; }
    ScopedHandle dll(CreateFileW(runtime_dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (dll.get() == INVALID_HANDLE_VALUE) { result.status = RimeSourceProfileStatus::kReadFailure; return result; }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER opened_dll_size{};
    if (!GetFileInformationByHandle(dll.get(), &info) || !GetFileSizeEx(dll.get(), &opened_dll_size) ||
        opened_dll_size.QuadPart < 0 || static_cast<std::uint64_t>(opened_dll_size.QuadPart) > kMaximumFileBytes ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      result.status = RimeSourceProfileStatus::kUnsafeEntry; return result;
    }
    Sha256 dll_hash;
    std::array<UCHAR, 64 * 1024> dll_buffer{};
    bool dll_ok = dll_hash.good();
    std::uint64_t dll_bytes = 0;
    for (;;) {
      DWORD read = 0;
      if (!dll_ok || !ReadFile(dll.get(), dll_buffer.data(), static_cast<DWORD>(dll_buffer.size()), &read, nullptr)) {
        dll_ok = false; break;
      }
      if (read == 0) break;
      dll_bytes += read;
      if (dll_bytes > kMaximumFileBytes) { dll_ok = false; break; }
      if (!dll_hash.Update(dll_buffer.data(), read)) { dll_ok = false; break; }
    }
    if (!dll_ok || !dll_hash.Finish(&result.runtime_sha256)) {
      result.status = RimeSourceProfileStatus::kReadFailure; return result;
    }
    auto status = HashRoot(shared_data, &result.data_sha256, &result.file_count);
    if (status != RimeSourceProfileStatus::kVerifiedSourceFiles) { result.status = status; return result; }
    if (result.runtime_sha256 != expected.runtime_sha256) { result.status = RimeSourceProfileStatus::kRuntimeMismatch; return result; }
    if (result.data_sha256 != expected.data_sha256 || result.file_count != expected.file_count) {
      result.status = RimeSourceProfileStatus::kDataMismatch;
      return result;
    }
    result.status = RimeSourceProfileStatus::kVerifiedSourceFiles;
  } catch (...) {
    result.status = RimeSourceProfileStatus::kReadFailure;
  }
  return result;
}

RimeCompiledProfileResult CheckRimeCompiledProfile(
    const std::filesystem::path& absolute_build_root,
    const RimeCompiledProfileExpected& expected) noexcept {
  RimeCompiledProfileResult result;
  try {
    std::error_code ec;
    if (absolute_build_root.empty() || !absolute_build_root.is_absolute() ||
        HasReparseAncestor(absolute_build_root)) {
      result.status = RimeCompiledProfileStatus::kInvalidRoot;
      return result;
    }
    const auto root_status = std::filesystem::symlink_status(absolute_build_root, ec);
    if (ec || !std::filesystem::is_directory(root_status) || IsReparse(absolute_build_root)) {
      result.status = RimeCompiledProfileStatus::kInvalidRoot;
      return result;
    }

    std::array<std::filesystem::path, kCompiledProfileFileCount> paths{};
    std::array<bool, kCompiledProfileFileCount> seen{};
    std::size_t entry_count = 0;
    std::filesystem::directory_iterator it(absolute_build_root,
                                           std::filesystem::directory_options::none, ec);
    if (ec) {
      result.status = RimeCompiledProfileStatus::kReadFailure;
      return result;
    }
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        result.status = RimeCompiledProfileStatus::kReadFailure;
        return result;
      }
      if (++entry_count > kCompiledProfileFileCount) {
        result.status = RimeCompiledProfileStatus::kUnsafeEntry;
        return result;
      }
      if (IsReparse(it->path())) {
        result.status = RimeCompiledProfileStatus::kUnsafeEntry;
        return result;
      }
      const auto status = it->symlink_status(ec);
      if (ec) {
        result.status = RimeCompiledProfileStatus::kReadFailure;
        return result;
      }
      if (!std::filesystem::is_regular_file(status)) {
        result.status = RimeCompiledProfileStatus::kUnsafeEntry;
        return result;
      }
      std::string name;
      if (!ToUtf8Relative(it->path().filename(), &name)) {
        result.status = RimeCompiledProfileStatus::kUnsafeEntry;
        return result;
      }
      const auto found = std::find(kCompiledProfileFiles.begin(), kCompiledProfileFiles.end(), name);
      if (found == kCompiledProfileFiles.end()) {
        result.status = RimeCompiledProfileStatus::kUnsafeEntry;
        return result;
      }
      const std::size_t index = static_cast<std::size_t>(found - kCompiledProfileFiles.begin());
      if (seen[index]) {
        result.status = RimeCompiledProfileStatus::kUnsafeEntry;
        return result;
      }
      seen[index] = true;
      paths[index] = it->path();
    }
    if (ec) {
      result.status = RimeCompiledProfileStatus::kReadFailure;
      return result;
    }
    if (entry_count != kCompiledProfileFileCount ||
        !std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) {
      result.status = RimeCompiledProfileStatus::kDataMismatch;
      return result;
    }

    std::array<std::string, kCompiledProfileFileCount> raw_files{};
    std::uint64_t aggregate_bytes = 0;
    for (std::size_t index = 0; index < kCompiledProfileFileCount; ++index) {
      const auto status = ReadCompiledFile(paths[index], &raw_files[index], &aggregate_bytes);
      if (status != RimeCompiledProfileStatus::kVerifiedCompiledFiles) {
        result.status = status;
        return result;
      }
    }

    std::array<std::string, kCompiledProfileFileCount> normalized_yaml{};
    constexpr std::array<std::size_t, 5> kYamlIndices = {0, 3, 7, 11, 13};
    for (const std::size_t index : kYamlIndices) {
      if (!NormalizeCompiledYaml(raw_files[index], &normalized_yaml[index])) {
        result.status = RimeCompiledProfileStatus::kMalformedConfig;
        return result;
      }
    }

    struct PrismBinding final {
      std::size_t prism_index;
      std::size_t schema_index;
      std::size_t alternate_schema_index;
    };
    constexpr std::array<PrismBinding, 3> kPrismBindings = {{{1, 3, 3}, {5, 7, 7}, {9, 11, 13}}};
    for (const PrismBinding& binding : kPrismBindings) {
      const std::string_view prism = raw_files[binding.prism_index];
      if (!HasPrismHeader(prism)) {
        result.status = RimeCompiledProfileStatus::kInvalidArtifact;
        return result;
      }
      const std::uint32_t recorded_schema_crc =
          ReadLittleEndian32(prism, kPrismSchemaChecksumOffset);
      const bool matches_primary = recorded_schema_crc == RimeCrc32(raw_files[binding.schema_index]);
      const bool matches_alternate = binding.alternate_schema_index != binding.schema_index &&
          recorded_schema_crc == RimeCrc32(raw_files[binding.alternate_schema_index]);
      if (!matches_primary && !matches_alternate) {
        result.status = RimeCompiledProfileStatus::kInvalidArtifact;
        return result;
      }
    }

    Sha256 aggregate;
    if (!aggregate.good()) {
      result.status = RimeCompiledProfileStatus::kReadFailure;
      return result;
    }
    for (std::size_t index = 0; index < kCompiledProfileFileCount; ++index) {
      std::string_view content = raw_files[index];
      std::string prism_without_schema_crc;
      if (index == 1 || index == 5 || index == 9) {
        prism_without_schema_crc.assign(content);
        std::fill(prism_without_schema_crc.begin() + kPrismSchemaChecksumOffset,
                  prism_without_schema_crc.begin() + kPrismSchemaChecksumOffset + sizeof(std::uint32_t),
                  '\0');
        content = prism_without_schema_crc;
      } else if (index == 0 || index == 3 || index == 7 || index == 11 || index == 13) {
        content = normalized_yaml[index];
      }
      Sha256 individual;
      if (!individual.good() || !individual.Update(content.data(), content.size())) {
        result.status = RimeCompiledProfileStatus::kReadFailure;
        return result;
      }
      std::string digest;
      if (!individual.Finish(&digest)) {
        result.status = RimeCompiledProfileStatus::kReadFailure;
        return result;
      }
      const std::string record = std::string(kCompiledProfileFiles[index]) + " " + digest + "\n";
      if (!aggregate.Update(record.data(), record.size())) {
        result.status = RimeCompiledProfileStatus::kReadFailure;
        return result;
      }
    }
    if (!aggregate.Finish(&result.data_sha256)) {
      result.status = RimeCompiledProfileStatus::kReadFailure;
      return result;
    }
    result.file_count = kCompiledProfileFileCount;
    if (expected.file_count != result.file_count || !IsLowerHexDigest(expected.data_sha256) ||
        result.data_sha256 != expected.data_sha256) {
      result.status = RimeCompiledProfileStatus::kDataMismatch;
      return result;
    }
    result.status = RimeCompiledProfileStatus::kVerifiedCompiledFiles;
  } catch (...) {
    result.status = RimeCompiledProfileStatus::kReadFailure;
  }
  return result;
}

}  // namespace ziliu::broker
