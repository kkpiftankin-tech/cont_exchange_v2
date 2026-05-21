#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/retry_replay_session_uc.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"

namespace {

using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::app::RetryReplaySessionRequest;
using cex::backtest::app::RetryReplaySessionResponse;
using cex::backtest::app::RetryReplaySessionUseCase;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

struct FakeSessionRepo final : public ReplaySessionRepositoryPort {
  std::vector<ReplaySession> storage;
  bool throw_on_create{false};
  std::optional<ReplaySession> get_response;

  ReplaySession Create(const ReplaySession& session) override {
    if (throw_on_create) {
      throw std::runtime_error("create failed");
    }
    storage.push_back(session);
    return session;
  }

  std::optional<ReplaySession> GetById(const std::string& session_id) override {
    for (const auto& s : storage) {
      if (s.session_id == session_id) {
        return s;
      }
    }
    return get_response;
  }

  std::vector<ReplaySession> List(const ReplaySessionListFilter&) override { return {}; }
  bool UpdateState(const std::string&, const ReplaySessionStatePatch&) override { return true; }
  std::vector<ReplaySession> GetRetryChain(const std::string&) override { return {}; }
};

ReplaySession MakeSession(const std::string& session_id, ReplaySessionStatus status) {
  ReplaySession s;
  s.session_id = session_id;
  s.user_id = "user-123";
  s.name = "test-session";
  s.strategy_json = "{}";
  s.date_range_from = std::chrono::system_clock::now();
  s.date_range_to = std::chrono::system_clock::now() + std::chrono::hours(1);
  s.solver_config_id = "solver-1";
  s.risk_limits_id = "risk-1";
  s.fee_model_json = "{}";
  s.session_config_snapshot_json = std::nullopt;
  s.status = status;
  s.total_batches = std::nullopt;
  s.progress_batches = 0;
  s.created_at = std::chrono::system_clock::now();
  return s;
}

bool test_retry_creates_new_session_with_parent_id_for_failed() {
  FakeSessionRepo repo;
  auto original = MakeSession("orig-1", ReplaySessionStatus::kFailed);
  repo.storage.push_back(original);

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "orig-1";
  req.user_id = "user-123";

  auto response = uc.Execute(req);

  if (!Check(response.ok, "retry should succeed")) return false;
  if (!Check(!response.new_session_id.empty(), "new_session_id should not be empty")) return false;
  if (!Check(response.created_session.has_value(), "created_session should be returned")) return false;

  const auto new_session_opt = repo.GetById(response.new_session_id);
  if (!Check(new_session_opt.has_value(), "new session should exist in repo")) return false;

  const auto& new_session = *new_session_opt;
  if (!Check(new_session.retry_parent_id.has_value(), "retry_parent_id should be set")) return false;
  if (!Check(*new_session.retry_parent_id == "orig-1", "retry_parent_id should point to original")) return false;
  if (!Check(new_session.status == ReplaySessionStatus::kPending, "new session status should be pending")) return false;
  if (!Check(new_session.user_id == "user-123", "user_id should be preserved")) return false;

  return true;
}

bool test_retry_creates_new_session_with_parent_id_for_cancelled() {
  FakeSessionRepo repo;
  auto original = MakeSession("orig-cancelled", ReplaySessionStatus::kCancelled);
  repo.storage.push_back(original);

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "orig-cancelled";
  req.user_id = "user-123";

  auto response = uc.Execute(req);

  if (!Check(response.ok, "retry should succeed for cancelled session")) return false;
  if (!Check(!response.new_session_id.empty(), "new_session_id should not be empty")) return false;
  if (!Check(response.created_session.has_value(), "created_session should be returned")) return false;
  if (!Check(response.created_session->retry_parent_id.value_or("") == "orig-cancelled",
             "retry_parent_id should point to cancelled original")) return false;

  return true;
}

bool test_retry_uses_requested_new_session_id() {
  FakeSessionRepo repo;
  auto original = MakeSession("orig-requested-id", ReplaySessionStatus::kFailed);
  repo.storage.push_back(original);

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "orig-requested-id";
  req.user_id = "user-123";
  req.new_session_id = "retry-session-from-gateway";

  auto response = uc.Execute(req);

  if (!Check(response.ok, "retry should succeed with requested id")) return false;
  if (!Check(response.new_session_id == "retry-session-from-gateway",
             "response should use requested retry id")) return false;
  const auto created = repo.GetById("retry-session-from-gateway");
  if (!Check(created.has_value(), "requested retry id should be persisted")) return false;
  return Check(created->retry_parent_id.value_or("") == "orig-requested-id",
               "requested retry id keeps parent link");
}

bool test_retry_preserves_reproducibility_snapshot_and_inputs() {
  FakeSessionRepo repo;
  auto original = MakeSession("orig-snapshot", ReplaySessionStatus::kCancelled);
  original.strategy_json =
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":0.5,"qmax":100}])";
  original.fee_model_json = R"({"makerfeerate":0.0002,"takerfeerate":0.0005})";
  original.session_config_snapshot_json =
      R"({"snapshot_version":1,"reward_config":{"body_json":"{\"mode\":\"-IS\"}"},"random_seed":42})";
  original.total_batches = 17;
  repo.storage.push_back(original);

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "orig-snapshot";
  req.user_id = "user-123";
  req.new_session_id = "retry-snapshot";

  const auto response = uc.Execute(req);
  if (!Check(response.ok, "retry with snapshot should succeed")) return false;
  const auto created = repo.GetById("retry-snapshot");
  if (!Check(created.has_value(), "retry snapshot session persisted")) return false;
  if (!Check(created->strategy_json == original.strategy_json,
             "retry preserves strategy")) return false;
  if (!Check(created->date_range_from == original.date_range_from &&
                 created->date_range_to == original.date_range_to,
             "retry preserves date range")) return false;
  if (!Check(created->session_config_snapshot_json ==
                 original.session_config_snapshot_json,
             "retry preserves config snapshot")) return false;
  if (!Check(created->fee_model_json == original.fee_model_json,
             "retry preserves fee model snapshot")) return false;
  if (!Check(created->total_batches == original.total_batches,
             "retry preserves total batch count")) return false;
  if (!Check(created->progress_batches == 0,
             "retry resets progress")) return false;
  return Check(created->status == ReplaySessionStatus::kPending,
               "retry starts pending");
}

bool test_retry_fails_for_non_failed_or_cancelled() {
  FakeSessionRepo repo;
  auto original = MakeSession("orig-2", ReplaySessionStatus::kRunning);
  repo.storage.push_back(original);

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "orig-2";
  req.user_id = "user-123";

  auto response = uc.Execute(req);

  if (!Check(!response.ok, "retry should fail for running session")) return false;
  if (!Check(response.error_code == "invalid_state", "error_code should be invalid_state")) return false;

  return true;
}

bool test_retry_fails_for_completed_session() {
  FakeSessionRepo repo;
  auto original = MakeSession("orig-completed", ReplaySessionStatus::kCompleted);
  repo.storage.push_back(original);

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "orig-completed";
  req.user_id = "user-123";

  auto response = uc.Execute(req);

  if (!Check(!response.ok, "retry should fail for completed session")) return false;
  if (!Check(response.error_code == "invalid_state", "error_code should be invalid_state")) return false;

  return true;
}

bool test_retry_fails_for_nonexistent_session() {
  FakeSessionRepo repo;

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "nonexistent";
  req.user_id = "user-123";

  auto response = uc.Execute(req);

  if (!Check(!response.ok, "retry should fail for nonexistent session")) return false;
  if (!Check(response.error_code == "not_found", "error_code should be not_found")) return false;

  return true;
}

bool test_retry_fails_for_wrong_user() {
  FakeSessionRepo repo;
  auto original = MakeSession("orig-3", ReplaySessionStatus::kFailed);
  original.user_id = "owner-456";
  repo.storage.push_back(original);

  RetryReplaySessionUseCase::Dependencies deps;
  deps.session_repo = &repo;
  RetryReplaySessionUseCase uc(deps);

  RetryReplaySessionRequest req;
  req.original_session_id = "orig-3";
  req.user_id = "wrong-user";

  auto response = uc.Execute(req);

  if (!Check(!response.ok, "retry should fail for wrong user")) return false;
  if (!Check(response.error_code == "permission_denied", "error_code should be permission_denied")) return false;

  return true;
}

}  // namespace

int main() {
  bool all_passed = true;

  auto run = [&](const char* name, bool (*fn)()) {
    std::cout << "Running: " << name << std::endl;
    if (!fn()) {
      std::cerr << "  FAILED: " << name << std::endl;
      all_passed = false;
    }
  };

  run("test_retry_creates_new_session_with_parent_id_for_failed", test_retry_creates_new_session_with_parent_id_for_failed);
  run("test_retry_creates_new_session_with_parent_id_for_cancelled", test_retry_creates_new_session_with_parent_id_for_cancelled);
  run("test_retry_uses_requested_new_session_id", test_retry_uses_requested_new_session_id);
  run("test_retry_preserves_reproducibility_snapshot_and_inputs", test_retry_preserves_reproducibility_snapshot_and_inputs);
  run("test_retry_fails_for_non_failed_or_cancelled", test_retry_fails_for_non_failed_or_cancelled);
  run("test_retry_fails_for_completed_session", test_retry_fails_for_completed_session);
  run("test_retry_fails_for_nonexistent_session", test_retry_fails_for_nonexistent_session);
  run("test_retry_fails_for_wrong_user", test_retry_fails_for_wrong_user);

  if (!all_passed) return EXIT_FAILURE;
  std::cout << "[OK] backtest_retry_replay_session_uc_test passed" << std::endl;
  return EXIT_SUCCESS;
}
