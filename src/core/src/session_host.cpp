#include "ziliu/core/session_host.h"

#include <cwctype>
#include <utility>

namespace ziliu::core {

SessionHost::SessionHost(EngineFactory engine_factory)
    : engine_factory_(std::move(engine_factory)) {}

std::uint64_t SessionHost::CreateSession(bool restricted) {
  // Unsigned increment below can reach zero only after issuing UINT64_MAX.
  // Exhaustion is terminal for this host: never reuse a closed session's ID.
  if (next_session_id_ == 0) {
    return 0;
  }
  auto engine = engine_factory_ ? engine_factory_(restricted) : nullptr;
  if (engine == nullptr) {
    return 0;
  }
  const std::uint64_t session_id = next_session_id_++;
  sessions_.emplace(session_id, Session(std::move(engine)));
  return session_id;
}

ipc::Response SessionHost::Handle(const ipc::Request& request) {
  ipc::Response response;
  response.request_id = request.request_id;
  response.session_id = request.session_id;

  if (request.command == ipc::Command::kPing) {
    return response;
  }
  if (request.command == ipc::Command::kCreateSession) {
    if (request.session_id != 0 || request.value > 1) {
      response.status = ipc::Status::kInvalidRequest;
      return response;
    }
    response.session_id = CreateSession(request.value != 0);
    if (response.session_id == 0) {
      response.status = ipc::Status::kInternalError;
    } else {
      const Session& session = sessions_.at(response.session_id);
      response.input_revision = session.input_revision;
      response.candidate_revision = session.candidate_revision;
      response.dictionary_epoch = session.engine->DictionaryEpoch();
    }
    return response;
  }

  const auto found = sessions_.find(request.session_id);
  if (found == sessions_.end()) {
    response.status = ipc::Status::kSessionNotFound;
    return response;
  }
  Session& session = found->second;
  Engine& engine = *session.engine;
  if (request.command == ipc::Command::kGetSessionState) {
    if (request.value != 0 || !request.theme_id.empty() || !request.resource.empty() ||
        request.point_x != 0 || request.point_y != 0) {
      response.status = ipc::Status::kInvalidRequest;
      return response;
    }
    response.input_revision = session.input_revision;
    response.candidate_revision = session.candidate_revision;
    response.dictionary_epoch = engine.DictionaryEpoch();
    return response;
  }
  const auto advance_revisions = [&session](bool input_changed) {
    static_cast<void>(AdvanceSessionRevisions(&session.input_revision,
                                              &session.candidate_revision,
                                              input_changed));
  };

  switch (request.command) {
    case ipc::Command::kCloseSession:
      sessions_.erase(found);
      return response;
    case ipc::Command::kReset:
      advance_revisions(true);
      engine.Reset();
      response.consumed = true;
      break;
    case ipc::Command::kInputLetter: {
      if (request.value > static_cast<std::uint32_t>(WCHAR_MAX)) {
        response.status = ipc::Status::kInvalidRequest;
        return response;
      }
      const auto letter = static_cast<wchar_t>(request.value);
      if (std::iswalpha(letter) != 0) {
        advance_revisions(true);
        response.consumed = engine.ProcessLetter(letter);
      }
      break;
    }
    case ipc::Command::kBackspace:
      advance_revisions(true);
      response.consumed = engine.Backspace();
      break;
    case ipc::Command::kPageUp:
      advance_revisions(false);
      response.consumed = engine.PageUp();
      break;
    case ipc::Command::kPageDown:
      advance_revisions(false);
      response.consumed = engine.PageDown();
      break;
    case ipc::Command::kSetCandidatePageSize:
      if (request.value == 0 || request.value > ipc::kMaximumCandidatesPerPage) {
        response.status = ipc::Status::kInvalidRequest;
        return response;
      }
      advance_revisions(false);
      engine.SetCandidatePageSize(request.value);
      response.consumed = true;
      break;
    case ipc::Command::kSetCandidateWindowPageCount:
      if (request.value == 0 || request.value > ipc::kMaximumCandidateWindowPages) {
        response.status = ipc::Status::kInvalidRequest;
        return response;
      }
      advance_revisions(false);
      engine.SetCandidateWindowPageCount(request.value);
      response.consumed = true;
      break;
    case ipc::Command::kSetTraditional:
      advance_revisions(false);
      engine.SetTraditional(request.value != 0);
      response.consumed = true;
      break;
    case ipc::Command::kSetChineseCandidatesOnly:
      advance_revisions(false);
      engine.SetChineseCandidatesOnly(request.value != 0);
      response.consumed = true;
      break;
    case ipc::Command::kInputSeparator:
      advance_revisions(true);
      response.consumed = engine.ProcessSeparator();
      break;
    case ipc::Command::kSelectCandidate: {
      advance_revisions(true);
      auto selection = engine.Select(request.value);
      response.consumed = selection.consumed;
      response.commit = std::move(selection.commit);
      break;
    }
    case ipc::Command::kPing:
    case ipc::Command::kCreateSession:
      break;
    case ipc::Command::kGetSettings:
    case ipc::Command::kGetThemeResource:
    case ipc::Command::kOpenQuickMenu:
    case ipc::Command::kRunMenuAction:
    case ipc::Command::kGetSessionState:
      response.status = ipc::Status::kUnsupported;
      return response;
    default:
      response.status = ipc::Status::kUnsupported;
      return response;
  }
  response.input_revision = session.input_revision;
  response.candidate_revision = session.candidate_revision;
  auto versioned = engine.SnapshotWithDictionaryEpoch();
  response.snapshot = std::move(versioned.snapshot);
  response.dictionary_epoch = versioned.dictionary_epoch;
  return response;
}

}  // namespace ziliu::core
