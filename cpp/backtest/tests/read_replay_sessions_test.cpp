#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/read_replay_sessions_uc.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"

namespace {

using cex::backtest::app::ReadReplaySessions;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool CheckNear(double actual, double expected, const std::string& message) {
  if (std::abs(actual - expected) <= 1e-9) return true;
  std::cerr << "[FAIL] " << message << " (actual=" << actual
            << ", expected=" << expected << ")" << std::endl;
  return false;
}

std::chrono::system_clock::time_point TimeMs(int64_t ms) {
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

ReplaySession MakeSession(const std::string& session_id,
                          ReplaySessionStatus status,
                          std::optional<int32_t> total_batches,
                          int32_t progress_batches) {
  ReplaySession session;
  session.session_id = session_id;
  session.user_id = "user-42";
  session.name = "Replay " + session_id;
  session.strategy_json = R"([{"symbol":"BTCUSDT","side":"buy"}])";
  session.date_range_from = TimeMs(1700000000000);
  session.date_range_to = TimeMs(1700003600000);
  session.solver_config_id = "solver-1";
  session.risk_limits_id = "risk-1";
  session.fee_model_json = R"({"makerfeerate":0.0002})";
  session.session_config_snapshot_json = R"({"solver":{"id":"solver-1"}})";
  session.status = status;
  session.total_batches = total_batches;
  session.progress_batches = progress_batches;
  session.created_at = TimeMs(1711111111000);
  session.started_at = TimeMs(1711111112000);
  session.completed_at = TimeMs(1711111122000);
  session.error_details = "failed on batch 14";
  return session;
}

struct FakeSessionRepo final : public ReplaySessionRepositoryPort {
  int create_calls{0};
  int get_calls{0};
  int list_calls{0};

  std::string last_get_id;
  ReplaySessionListFilter last_filter;

  std::optional<ReplaySession> get_response;
  std::vector<ReplaySession> list_response;

  bool throw_on_get{false};
  bool throw_on_list{false};

  ReplaySession Create(const ReplaySession& session) override {
    ++create_calls;
    return session;
  }

  std::optional<ReplaySession> GetById(const std::string& session_id) override {
    ++get_calls;
    last_get_id = session_id;
    if (throw_on_get) {
      throw std::runtime_error("GetById failed");
    }
    return get_response;
  }

  std::vector<ReplaySession> List(const ReplaySessionListFilter& filter) override {
    ++list_calls;
    last_filter = filter;
    if (throw_on_list) {
      throw std::runtime_error("List failed");
    }
    return list_response;
  }

  bool UpdateState(const std::string&, const ReplaySessionStatePatch&) override {
    return true;
  }

  std::vector<ReplaySession> GetRetryChain(const std::string&) override {
    return {};
  }
};

bool test_list_happy_path_passes_filter_and_maps_dto() {
  FakeSessionRepo repo;
  repo.list_response = {MakeSession("sess-1", ReplaySessionStatus::kRunning, 10, 4)};

  ReadReplaySessions::Dependencies deps;
  deps.session_repo = &repo;
  ReadReplaySessions uc(deps);

  ReadReplaySessions::ListRequest req;
  req.user_id = "user-42";
  req.status = "running";
  req.created_from = TimeMs(1700000000000);
  req.created_to = TimeMs(1700007200000);
  req.replay_from = TimeMs(1700000000000);
  req.replay_to = TimeMs(1700003600000);
  req.limit = 999;   // should be capped by use case
  req.offset = 7;

  auto result = uc.ListReplaySessions(req);
  if (!Check(result.ok, "list happy path must succeed")) return false;
  if (!Check(repo.list_calls == 1, "repo.List must be called once")) return false;

  if (!Check(repo.last_filter.user_id.has_value() &&
                 *repo.last_filter.user_id == "user-42",
             "user_id filter passed to repo")) {
    return false;
  }
  if (!Check(repo.last_filter.status.has_value() &&
                 *repo.last_filter.status == ReplaySessionStatus::kRunning,
             "status filter must be parsed and passed")) {
    return false;
  }
  if (!Check(repo.last_filter.created_from == req.created_from &&
                 repo.last_filter.created_to == req.created_to,
             "created range must be passed to repo")) {
    return false;
  }
  if (!Check(repo.last_filter.replay_from == req.replay_from &&
                 repo.last_filter.replay_to == req.replay_to,
             "replay range must be passed to repo")) {
    return false;
  }
  if (!Check(repo.last_filter.limit.has_value() && *repo.last_filter.limit == 500,
             "limit must be capped at 500")) {
    return false;
  }
  if (!Check(repo.last_filter.offset.has_value() && *repo.last_filter.offset == 7,
             "offset must be passed to repo")) {
    return false;
  }

  if (!Check(result.limit == 500 && result.offset == 7 && result.returned == 1,
             "result pagination metadata must be set")) {
    return false;
  }
  if (!Check(result.items.size() == 1, "result must contain one mapped item")) return false;
  const auto& item = result.items[0];
  if (!Check(item.sessionid == "sess-1", "sessionid mapped")) return false;
  if (!Check(item.userid == "user-42", "userid mapped")) return false;
  if (!Check(item.status == "running", "status mapped as lower-case string")) return false;
  if (!Check(item.progressbatches == 4, "progressbatches mapped")) return false;
  if (!Check(item.totalbatches.has_value() && *item.totalbatches == 10,
             "totalbatches mapped")) {
    return false;
  }
  if (!CheckNear(item.progress, 40.0, "progress must be computed from batches")) return false;
  return true;
}

bool test_list_validation_does_not_call_repo() {
  FakeSessionRepo repo;
  ReadReplaySessions::Dependencies deps;
  deps.session_repo = &repo;
  ReadReplaySessions uc(deps);

  std::vector<ReadReplaySessions::ListRequest> invalid_cases;

  ReadReplaySessions::ListRequest invalid_status;
  invalid_status.status = "queued";
  invalid_cases.push_back(invalid_status);

  ReadReplaySessions::ListRequest invalid_limit;
  invalid_limit.limit = 0;
  invalid_cases.push_back(invalid_limit);

  ReadReplaySessions::ListRequest invalid_offset;
  invalid_offset.offset = -1;
  invalid_cases.push_back(invalid_offset);

  ReadReplaySessions::ListRequest invalid_created_range;
  invalid_created_range.created_from = TimeMs(1000);
  invalid_created_range.created_to = TimeMs(999);
  invalid_cases.push_back(invalid_created_range);

  ReadReplaySessions::ListRequest invalid_replay_range;
  invalid_replay_range.replay_from = TimeMs(2000);
  invalid_replay_range.replay_to = TimeMs(1999);
  invalid_cases.push_back(invalid_replay_range);

  for (size_t i = 0; i < invalid_cases.size(); ++i) {
    auto result = uc.ListReplaySessions(invalid_cases[i]);
    if (!Check(!result.ok, "invalid list request must fail #" + std::to_string(i))) return false;
    if (!Check(result.error_code == "validation_error",
               "error_code=validation_error #" + std::to_string(i))) {
      return false;
    }
  }

  if (!Check(repo.list_calls == 0, "repo.List must not be called on validation errors")) {
    return false;
  }
  return true;
}

bool test_get_happy_path_returns_full_card() {
  FakeSessionRepo repo;
  auto session = MakeSession("sess-full", ReplaySessionStatus::kFailed, 8, 3);
  session.user_id = "user-007";
  session.name = "Replay full card";
  session.strategy_json = R"([{"symbol":"ETHUSDT","side":"sell"}])";
  session.date_range_from = TimeMs(1701000000000);
  session.date_range_to = TimeMs(1702000000000);
  session.solver_config_id = "solver-x";
  session.risk_limits_id = "risk-y";
  session.fee_model_json = R"({"makerfeerate":0.0002,"takerfeerate":0.0005})";
  session.session_config_snapshot_json = R"({"solver":{"id":"solver-x"},"risk":{"id":"risk-y"}})";
  session.created_at = TimeMs(1702100000000);
  session.started_at = TimeMs(1702100010000);
  session.completed_at = TimeMs(1702100020000);
  session.error_details = "solver divergence on batch 512";

  repo.get_response = session;

  ReadReplaySessions::Dependencies deps;
  deps.session_repo = &repo;
  ReadReplaySessions uc(deps);

  auto result = uc.GetReplaySession({.session_id = "sess-full"});
  if (!Check(result.ok, "get happy path must succeed")) return false;
  if (!Check(repo.get_calls == 1, "repo.GetById must be called once")) return false;
  if (!Check(repo.last_get_id == "sess-full", "session id passed to repo")) return false;

  const auto& view = result.session;
  if (!Check(view.sessionid == "sess-full", "sessionid mapped")) return false;
  if (!Check(view.userid == "user-007", "userid mapped")) return false;
  if (!Check(view.name == "Replay full card", "name mapped")) return false;
  if (!Check(view.status == "failed", "status mapped")) return false;
  if (!CheckNear(view.progress, 37.5, "progress mapped from batches")) return false;
  if (!Check(view.progressbatches == 3, "progressbatches mapped")) return false;
  if (!Check(view.totalbatches.has_value() && *view.totalbatches == 8,
             "totalbatches mapped")) {
    return false;
  }
  if (!Check(view.createdat == TimeMs(1702100000000), "createdat mapped")) return false;
  if (!Check(view.startedat.has_value() && *view.startedat == TimeMs(1702100010000),
             "startedat mapped")) {
    return false;
  }
  if (!Check(view.completedat.has_value() && *view.completedat == TimeMs(1702100020000),
             "completedat mapped")) {
    return false;
  }
  if (!Check(view.daterangefrom == TimeMs(1701000000000), "daterangefrom mapped")) return false;
  if (!Check(view.daterangeto == TimeMs(1702000000000), "daterangeto mapped")) return false;
  if (!Check(view.solverconfigid == "solver-x", "solverconfigid mapped")) return false;
  if (!Check(view.risklimitsid == "risk-y", "risklimitsid mapped")) return false;
  if (!Check(view.feemodel == R"({"makerfeerate":0.0002,"takerfeerate":0.0005})",
             "feemodel mapped")) {
    return false;
  }
  if (!Check(view.strategy == R"([{"symbol":"ETHUSDT","side":"sell"}])",
             "strategy mapped")) {
    return false;
  }
  if (!Check(view.sessionconfigsnapshot.has_value() &&
                 *view.sessionconfigsnapshot ==
                     R"({"solver":{"id":"solver-x"},"risk":{"id":"risk-y"}})",
             "sessionconfigsnapshot mapped")) {
    return false;
  }
  if (!Check(view.errordetails.has_value() &&
                 *view.errordetails == "solver divergence on batch 512",
             "errordetails mapped")) {
    return false;
  }
  return true;
}

bool test_get_not_found() {
  FakeSessionRepo repo;
  repo.get_response = std::nullopt;

  ReadReplaySessions::Dependencies deps;
  deps.session_repo = &repo;
  ReadReplaySessions uc(deps);

  auto result = uc.GetReplaySession({.session_id = "unknown"});
  if (!Check(!result.ok, "not found must fail")) return false;
  if (!Check(result.error_code == "not_found", "error_code=not_found")) return false;
  if (!Check(repo.get_calls == 1, "repo.GetById must be called")) return false;
  return true;
}

bool test_repository_exception_maps_to_repository_error() {
  FakeSessionRepo repo;
  ReadReplaySessions::Dependencies deps;
  deps.session_repo = &repo;
  ReadReplaySessions uc(deps);

  repo.throw_on_list = true;
  auto list_result = uc.ListReplaySessions({});
  if (!Check(!list_result.ok, "list exception must fail")) return false;
  if (!Check(list_result.error_code == "repository_error",
             "list exception maps to repository_error")) {
    return false;
  }

  repo.throw_on_list = false;
  repo.throw_on_get = true;
  auto get_result = uc.GetReplaySession({.session_id = "sess-1"});
  if (!Check(!get_result.ok, "get exception must fail")) return false;
  if (!Check(get_result.error_code == "repository_error",
             "get exception maps to repository_error")) {
    return false;
  }
  return true;
}

bool test_missing_dependency_maps_to_dependency_error() {
  ReadReplaySessions uc({});

  auto list_result = uc.ListReplaySessions({});
  if (!Check(!list_result.ok, "list without dependency must fail")) return false;
  if (!Check(list_result.error_code == "dependency_error",
             "list missing dependency -> dependency_error")) {
    return false;
  }

  auto get_result = uc.GetReplaySession({.session_id = "sess-1"});
  if (!Check(!get_result.ok, "get without dependency must fail")) return false;
  if (!Check(get_result.error_code == "dependency_error",
             "get missing dependency -> dependency_error")) {
    return false;
  }
  return true;
}

bool test_progress_cases() {
  FakeSessionRepo repo;
  repo.list_response = {
      MakeSession("sess-null-total", ReplaySessionStatus::kRunning, std::nullopt, 3),
      MakeSession("sess-zero-total", ReplaySessionStatus::kCompleted, 0, 3),
      MakeSession("sess-partial-running", ReplaySessionStatus::kRunning, 20, 5),
      MakeSession("sess-completed-null-total", ReplaySessionStatus::kCompleted, std::nullopt, 0),
  };

  ReadReplaySessions::Dependencies deps;
  deps.session_repo = &repo;
  ReadReplaySessions uc(deps);

  auto result = uc.ListReplaySessions({});
  if (!Check(result.ok, "progress cases list must succeed")) return false;
  if (!Check(result.items.size() == 4, "all progress case items returned")) return false;
  if (!Check(result.limit == 50 && result.offset == 0, "default paging is applied")) return false;
  if (!Check(repo.last_filter.limit.has_value() && *repo.last_filter.limit == 50,
             "default limit forwarded to repo")) {
    return false;
  }
  if (!Check(repo.last_filter.offset.has_value() && *repo.last_filter.offset == 0,
             "default offset forwarded to repo")) {
    return false;
  }

  if (!CheckNear(result.items[0].progress, 0.0, "null total -> progress 0")) return false;
  if (!CheckNear(result.items[1].progress, 100.0, "zero total and completed -> progress 100"))
    return false;
  if (!CheckNear(result.items[2].progress, 25.0, "partial running -> batch ratio progress"))
    return false;
  if (!CheckNear(result.items[3].progress, 100.0, "completed without total -> progress 100"))
    return false;
  return true;
}

}  // namespace

int main() {
  bool all_passed = true;
  auto run = [&](const char* name, bool (*fn)()) {
    if (!fn()) {
      std::cerr << "  in test: " << name << std::endl;
      all_passed = false;
    }
  };

  run("list_happy_path_passes_filter_and_maps_dto",
      &test_list_happy_path_passes_filter_and_maps_dto);
  run("list_validation_does_not_call_repo", &test_list_validation_does_not_call_repo);
  run("get_happy_path_returns_full_card", &test_get_happy_path_returns_full_card);
  run("get_not_found", &test_get_not_found);
  run("repository_exception_maps_to_repository_error",
      &test_repository_exception_maps_to_repository_error);
  run("missing_dependency_maps_to_dependency_error",
      &test_missing_dependency_maps_to_dependency_error);
  run("progress_cases", &test_progress_cases);

  if (!all_passed) return EXIT_FAILURE;
  std::cout << "[OK] backtest_read_replay_sessions_test" << std::endl;
  return EXIT_SUCCESS;
}