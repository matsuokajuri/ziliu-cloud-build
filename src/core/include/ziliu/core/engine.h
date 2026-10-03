#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ziliu::core {

inline constexpr std::size_t kMaximumPinyinLetters = 64;

struct Candidate {
  std::wstring text;
  std::wstring annotation;
  double score = 0.0;

  bool operator==(const Candidate&) const = default;
};

struct CompositionSnapshot {
  std::wstring preedit;
  std::vector<Candidate> candidates;
  std::size_t highlighted_index = 0;
  bool has_previous_page = false;
  bool has_next_page = false;

  [[nodiscard]] bool empty() const noexcept { return preedit.empty(); }
  [[nodiscard]] std::wstring plain_text() const {
    std::wstring result;
    result.reserve(preedit.size());
    for (const wchar_t character : preedit) {
      if (character != L'\'' && character != L' ' && character != L'\t' && character != L'\r' &&
          character != L'\n') {
        result.push_back(character);
      }
    }
    return result;
  }
};

struct SelectionResult {
  bool consumed = false;
  std::wstring commit;

  bool operator==(const SelectionResult&) const = default;
};

struct VersionedCompositionSnapshot {
  CompositionSnapshot snapshot;
  std::uint64_t dictionary_epoch = 0;
};

[[nodiscard]] bool IsChineseCandidate(std::wstring_view text) noexcept;

class Engine {
 public:
  virtual ~Engine() = default;

  virtual void Reset() = 0;
  virtual bool ProcessLetter(wchar_t letter) = 0;
  virtual bool ProcessSeparator() = 0;
  virtual bool Backspace() = 0;
  virtual bool PageUp() = 0;
  virtual bool PageDown() = 0;
  virtual void SetCandidatePageSize(std::size_t page_size) = 0;
  virtual void SetCandidateWindowPageCount(std::size_t page_count) = 0;
  virtual void SetTraditional(bool enabled) = 0;
  virtual void SetChineseCandidatesOnly(bool enabled) = 0;
  virtual SelectionResult Select(std::size_t candidate_index) = 0;
  [[nodiscard]] virtual CompositionSnapshot Snapshot() const = 0;
  // Zero means no trustworthy dictionary generation is available. Nonzero
  // generations must never be reused after a dictionary mutation, including
  // learning in another session or background work. Scope: broker + session.
  // Return zero while mutation may be in flight; recovery must not revive an
  // earlier generation. Exhaustion must fail closed rather than wrap/reuse.
  // This read must be nonblocking and must not itself mutate engine state.
  // Consumers still need a fresh check; it is not a lock or persistent version.
  [[nodiscard]] virtual std::uint64_t DictionaryEpoch() const noexcept { return 0; }
  // Engines whose lazy reads can mutate shared state must override this and
  // capture the completed generation within their serialized operation scope.
  [[nodiscard]] virtual VersionedCompositionSnapshot SnapshotWithDictionaryEpoch() const {
    const auto before = DictionaryEpoch();
    VersionedCompositionSnapshot result{Snapshot(), 0};
    const auto after = DictionaryEpoch();
    if (before != 0 && before == after) result.dictionary_epoch = after;
    return result;
  }
};

// Temporary deterministic engine used to validate the Windows shell before
// librime is connected. It deliberately contains no persistence or network IO.
[[nodiscard]] std::unique_ptr<Engine> CreateStubEngine();
[[nodiscard]] std::unique_ptr<Engine> CreateStubEngineForSession(bool restricted);

}  // namespace ziliu::core
