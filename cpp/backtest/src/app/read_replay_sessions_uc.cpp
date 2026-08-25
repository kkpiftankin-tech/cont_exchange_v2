#include "app/read_replay_sessions_uc.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace cex::backtest::app {
namespace {

constexpr const char* kValidationError = "validation_error";
constexpr const char* kNotFoundError = "not_found";
constexpr const char* kDependencyError = "dependency_error";
constexpr const char* kRepositoryError = "repository_error";

constexpr std::int32_t kDefaultLimit = 50;
constexpr std::int32_t kDefaultOffset = 0;
constexpr std::int32_t kMaxLimit = 500;

bool IsBlank(const std::string& value) {
  return value.empty() ||
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return std::isspace(ch) != 0;
         });
}

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::optional<ReplaySessionStatus> ParseStatus(const std::string& status_raw) {
  const std::string status = ToLowerAscii(status_raw);
  if (status == "pending") return ReplaySessionStatus::kPending;
  if (status == "running") return ReplaySessionStatus::kRunning;
  if (status == "completed") return ReplaySessionStatus::kCompleted;
  if (status == "failed") return ReplaySessionStatus::kFailed;
  if (status == "cancelled") return ReplaySessionStatus::kCancelled;
  return std::nullopt;
}

std::string StatusToString(ReplaySessionStatus status) {
  switch (status) {
    case ReplaySessionStatus::kPending: return "pending";
    case ReplaySessionStatus::kRunning: return "running";
    case ReplaySessionStatus::kCompleted: return "completed";
    case ReplaySessionStatus::kFailed: return "failed";
    case ReplaySessionStatus::kCancelled: return "cancelled";
  }
  return "pending";
}

double ComputeProgress(ReplaySessionStatus status,
                       std::int32_t progress_batches,
                       std::optional<std::int32_t> total_batches) {
  if (total_batches.has_value() && *total_batches > 0) {
    const double progress =
        static_cast<double>(progress_batches) * 100.0 / static_cast<double>(*total_batches);
    return std::clamp(progress, 0.0, 100.0);
  }
  if (status == ReplaySessionStatus::kCompleted) {
    return 100.0;
  }
  return 0.0;
}

ReadReplaySessions::ReplaySessionView ToView(const ReplaySession& session) {
  ReadReplaySessions::ReplaySessionView view;
  view.sessionid = session.session_id;
  view.userid = session.user_id;
  view.name = session.name;
  view.status = StatusToString(session.status);
  view.progress =
      ComputeProgress(session.status, session.progress_batches, session.total_batches);
  view.progressbatches = session.progress_batches;
  view.totalbatches = session.total_batches;
  view.createdat = session.created_at;
  view.startedat = session.started_at;
  view.completedat = session.completed_at;
  view.daterangefrom = session.date_range_from;
  view.daterangeto = session.date_range_to;
  view.solverconfigid = session.solver_config_id;
  view.risklimitsid = session.risk_limits_id;
  view.feemodel = session.fee_model_json;
  view.strategy = session.strategy_json;
  view.sessionconfigsnapshot = session.session_config_snapshot_json;
  view.errordetails = session.error_details;
  return view;
}

ReadReplaySessions::ListResult ListError(std::string error_code,
                                         std::string error_message,
                                         std::int32_t limit,
                                         std::int32_t offset) {
  ReadReplaySessions::ListResult result;
  result.ok = false;
  result.limit = limit;
  result.offset = offset;
  result.error_code = std::move(error_code);
  result.error_message = std::move(error_message);
  return result;
}

ReadReplaySessions::GetResult GetError(std::string error_code,
                                       std::string error_message) {
  ReadReplaySessions::GetResult result;
  result.ok = false;
  result.error_code = std::move(error_code);
  result.error_message = std::move(error_message);
  return result;
}

}  // namespace

ReadReplaySessions::ReadReplaySessions(Dependencies deps) : deps_(std::move(deps)) {}

ReadReplaySessions::ListResult ReadReplaySessions::ListReplaySessions(
    const ListRequest& request) const {
  const std::int32_t requested_limit = request.limit.value_or(kDefaultLimit);
  const std::int32_t requested_offset = request.offset.value_or(kDefaultOffset);

  if (deps_.session_repo == nullptr) {
    return ListError(kDependencyError,
                     "ReadReplaySessions dependencies are missing",
                     requested_limit,
                     requested_offset);
  }

  if (request.user_id.has_value() && IsBlank(*request.user_id)) {
    return ListError(kValidationError,
                     "user_id must not be blank",
                     requested_limit,
                     requested_offset);
  }

  std::optional<ReplaySessionStatus> status;
  if (request.status.has_value()) {
    if (IsBlank(*request.status)) {
      return ListError(kValidationError,
                       "status must not be blank",
                       requested_limit,
                       requested_offset);
    }
    status = ParseStatus(*request.status);
    if (!status.has_value()) {
      return ListError(
          kValidationError,
          "status must be one of: pending, running, completed, failed, cancelled",
          requested_limit,
          requested_offset);
    }
  }

  if (requested_limit <= 0) {
    return ListError(kValidationError,
                     "limit must be > 0",
                     requested_limit,
                     requested_offset);
  }
  if (requested_offset < 0) {
    return ListError(kValidationError,
                     "offset must be >= 0",
                     requested_limit,
                     requested_offset);
  }
  if (request.created_from.has_value() && request.created_to.has_value() &&
      *request.created_from > *request.created_to) {
    return ListError(kValidationError,
                     "created_from must be <= created_to",
                     requested_limit,
                     requested_offset);
  }
  if (request.replay_from.has_value() && request.replay_to.has_value() &&
      *request.replay_from > *request.replay_to) {
    return ListError(kValidationError,
                     "replay_from must be <= replay_to",
                     requested_limit,
                     requested_offset);
  }

  const std::int32_t effective_limit = std::min(requested_limit, kMaxLimit);
  const std::int32_t effective_offset = requested_offset;

  ReplaySessionListFilter filter;
  if (request.user_id.has_value()) {
    filter.user_id = *request.user_id;
  }
  if (status.has_value()) {
    filter.status = *status;
  }
  filter.created_from = request.created_from;
  filter.created_to = request.created_to;
  filter.replay_from = request.replay_from;
  filter.replay_to = request.replay_to;
  filter.limit = effective_limit;
  filter.offset = effective_offset;

  try {
    const auto sessions = deps_.session_repo->List(filter);

    ListResult result;
    result.ok = true;
    result.limit = effective_limit;
    result.offset = effective_offset;
    result.returned = static_cast<std::int32_t>(sessions.size());
    result.items.reserve(sessions.size());
    for (const auto& session : sessions) {
      result.items.push_back(ToView(session));
    }
    return result;
  } catch (const std::exception& ex) {
    return ListError(
        kRepositoryError,
        std::string("Replay session list failed: ") + ex.what(),
        effective_limit,
        effective_offset);
  } catch (...) {
    return ListError(kRepositoryError,
                     "Replay session list failed",
                     effective_limit,
                     effective_offset);
  }
}

ReadReplaySessions::GetResult ReadReplaySessions::GetReplaySession(
    const GetRequest& request) const {
  if (deps_.session_repo == nullptr) {
    return GetError(kDependencyError, "ReadReplaySessions dependencies are missing");
  }
  if (IsBlank(request.session_id)) {
    return GetError(kValidationError, "session_id is required");
  }

  try {
    const auto session = deps_.session_repo->GetById(request.session_id);
    if (!session.has_value()) {
      return GetError(kNotFoundError, "Replay session not found");
    }

    GetResult result;
    result.ok = true;
    result.session = ToView(*session);
    return result;
  } catch (const std::exception& ex) {
    return GetError(kRepositoryError,
                    std::string("Replay session get failed: ") + ex.what());
  } catch (...) {
    return GetError(kRepositoryError, "Replay session get failed");
  }
}

}  // namespace cex::backtest::app

