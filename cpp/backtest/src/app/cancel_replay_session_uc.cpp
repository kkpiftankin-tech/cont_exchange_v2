#include "app/cancel_replay_session_uc.hpp"

#include <exception>
#include <utility>

namespace cex::backtest::app {
namespace {

constexpr const char* kValidationError  = "validation_error";
constexpr const char* kNotFoundError    = "not_found";
constexpr const char* kInvalidStatus    = "invalid_status";
constexpr const char* kDependencyError  = "dependency_error";
constexpr const char* kRepositoryError  = "repository_error";

CancelReplaySession::Result ErrorResult(std::string code, std::string message) {
  CancelReplaySession::Result r;
  r.ok = false;
  r.error_code = std::move(code);
  r.error_message = std::move(message);
  return r;
}

std::string StatusToString(ReplaySessionStatus status) {
  switch (status) {
    case ReplaySessionStatus::kPending:   return "pending";
    case ReplaySessionStatus::kRunning:   return "running";
    case ReplaySessionStatus::kCompleted: return "completed";
    case ReplaySessionStatus::kFailed:    return "failed";
    case ReplaySessionStatus::kCancelled: return "cancelled";
  }
  return "unknown";
}

std::string BuildErrorDetails(const std::optional<std::string>& requested_by,
                              const std::optional<std::string>& reason) {
  std::string detail = "cancelled";
  if (requested_by.has_value() && !requested_by->empty()) {
    detail += " by user: " + *requested_by;
  }
  if (reason.has_value() && !reason->empty()) {
    detail += ": " + *reason;
  }
  return detail;
}

}  // namespace

CancelReplaySession::CancelReplaySession(Dependencies deps, Clock clock)
    : deps_(std::move(deps)), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = []() { return std::chrono::system_clock::now(); };
  }
}

std::chrono::system_clock::time_point CancelReplaySession::Now() const {
  return clock_();
}

bool CancelReplaySession::IsCancelled() const {
  std::lock_guard<std::mutex> lock(cancelled_mu_);
  return !cancelled_session_ids_.empty();
}

bool CancelReplaySession::IsCancelled(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(cancelled_mu_);
  return cancelled_session_ids_.find(session_id) != cancelled_session_ids_.end();
}

CancelReplaySession::Result CancelReplaySession::Run(const Request& request) {
  if (deps_.session_repo == nullptr) {
    return ErrorResult(kDependencyError, "session_repo is required");
  }
  if (request.session_id.empty()) {
    return ErrorResult(kValidationError, "session_id is required");
  }

  std::optional<ReplaySession> maybe_session;
  try {
    maybe_session = deps_.session_repo->GetById(request.session_id);
  } catch (const std::exception& ex) {
    return ErrorResult(kRepositoryError,
                       std::string("Failed to load replay session: ") + ex.what());
  }

  if (!maybe_session.has_value()) {
    return ErrorResult(kNotFoundError, "Replay session not found");
  }

  ReplaySession session = *maybe_session;

  if (session.status == ReplaySessionStatus::kCancelled) {
    Result r;
    r.ok = true;
    r.session = session;
    r.cancellation_dispatched = false;
    return r;
  }

  if (session.status != ReplaySessionStatus::kPending &&
      session.status != ReplaySessionStatus::kRunning) {
    return ErrorResult(
        kInvalidStatus,
        "Cannot cancel session in status: " + StatusToString(session.status));
  }

  if (session.status == ReplaySessionStatus::kPending) {
    {
      std::lock_guard<std::mutex> lock(cancelled_mu_);
      cancelled_session_ids_.insert(request.session_id);
    }

    ReplaySessionStatePatch patch;
    patch.status = ReplaySessionStatus::kCancelled;
    patch.completed_at = Now();
    patch.error_details = BuildErrorDetails(request.requested_by, request.reason);
    try {
      deps_.session_repo->UpdateState(request.session_id, patch);
    } catch (const std::exception& ex) {
      return ErrorResult(kRepositoryError,
                         std::string("Failed to update session state: ") + ex.what());
    }
    session.status = ReplaySessionStatus::kCancelled;
    session.completed_at = patch.completed_at;
    session.error_details = patch.error_details;

    ReplaySummary summary;
    summary.session_id = request.session_id;
    summary.total_batches =
        session.total_batches.has_value() && *session.total_batches > 0
            ? static_cast<uint32_t>(*session.total_batches)
            : 0;
    summary.processed_batches = 0;
    summary.failed_batches = 0;
    summary.partial = true;
    if (deps_.summary_store != nullptr) {
      deps_.summary_store->SaveSummary(summary);
    }
    if (deps_.event_publisher != nullptr) {
      ReplayLifecycleEvent evt;
      evt.session_id = request.session_id;
      evt.status = ReplayLifecycleStatus::kCancelled;
      evt.summary = summary;
      evt.error_details = session.error_details;
      evt.error_code = "cancelled";
      deps_.event_publisher->PublishLifecycle(evt);
    }

    Result result;
    result.ok = true;
    result.session = session;
    result.cancellation_dispatched = false;
    return result;
  }

  // Running: signal the token. RunReplaySession finishes the current batch,
  // then saves partial summary/logs and persists kCancelled.
  {
    std::lock_guard<std::mutex> lock(cancelled_mu_);
    cancelled_session_ids_.insert(request.session_id);
  }

  Result result;
  result.ok = true;
  result.session = session;
  result.cancellation_dispatched = true;
  return result;
}

}  // namespace cex::backtest::app
