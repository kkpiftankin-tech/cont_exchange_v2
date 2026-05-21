#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/create_replay_session_uc.hpp"
#include "app/historical_batch_loader_port.hpp"
#include "app/replay_config_repository_port.hpp"
#include "app/replay_config_snapshot.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"

namespace {

using json = nlohmann::json;

using cex::backtest::app::CreateReplaySession;
using cex::backtest::app::HistoricalBatchResultRow;
using cex::backtest::app::HistoricalFillRow;
using cex::backtest::app::HistoricalMarketdataSnapshotRow;
using cex::backtest::app::IHistoricalBatchLoader;
using cex::backtest::app::IReplayConfigRepository;
using cex::backtest::app::ReplayConfigSnapshotBuilder;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::app::StoredConfigDocument;

bool Check(bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

struct FakeSessionRepo final : public ReplaySessionRepositoryPort {
  int create_calls{0};
  std::optional<ReplaySession> last_created;
  bool throw_on_create{false};

  ReplaySession Create(const ReplaySession& session) override {
    ++create_calls;
    last_created = session;
    if (throw_on_create) {
      throw std::runtime_error("create failed");
    }
    return session;
  }

  std::optional<ReplaySession> GetById(const std::string&) override { return std::nullopt; }
  std::vector<ReplaySession> List(const ReplaySessionListFilter&) override { return {}; }
  bool UpdateState(const std::string&, const ReplaySessionStatePatch&) override { return true; }

  std::vector<ReplaySession> GetRetryChain(const std::string&) override {
    return {};
  }
};

struct FakeHistoryLoader final : public IHistoricalBatchLoader {
  std::vector<HistoricalBatchResultRow> rows;
  std::vector<HistoricalFillRow> fills;
  std::vector<HistoricalMarketdataSnapshotRow> snapshots;
  int batch_calls{0};
  int fill_calls{0};
  int snapshot_calls{0};
  bool throw_on_batch_load{false};
  int64_t last_from_ms{0};
  int64_t last_to_ms{0};
  int64_t last_offset{0};
  int64_t last_limit{0};

  std::vector<HistoricalBatchResultRow> LoadBatchResults(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    ++batch_calls;
    if (throw_on_batch_load) {
      throw std::runtime_error("ClickHouse unavailable");
    }
    last_from_ms = from_ms;
    last_to_ms = to_ms;
    last_offset = offset;
    last_limit = limit;

    std::vector<HistoricalBatchResultRow> filtered;
    for (const auto& row : rows) {
      if (row.event_time_ms >= from_ms && row.event_time_ms <= to_ms) {
        filtered.push_back(row);
      }
    }

    std::vector<HistoricalBatchResultRow> out;
    for (int64_t i = offset;
         i < static_cast<int64_t>(filtered.size()) && i < offset + limit; ++i) {
      out.push_back(filtered[i]);
    }
    return out;
  }

  std::vector<HistoricalFillRow> LoadFills(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    ++fill_calls;
    std::vector<HistoricalFillRow> filtered;
    for (const auto& row : fills) {
      if (row.event_time_ms >= from_ms && row.event_time_ms <= to_ms) {
        filtered.push_back(row);
      }
    }
    std::vector<HistoricalFillRow> out;
    for (int64_t i = offset;
         i < static_cast<int64_t>(filtered.size()) && i < offset + limit; ++i) {
      out.push_back(filtered[i]);
    }
    return out;
  }

  std::vector<HistoricalMarketdataSnapshotRow> LoadMarketdataSnapshots(
      int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) override {
    ++snapshot_calls;
    std::vector<HistoricalMarketdataSnapshotRow> source = snapshots;
    if (source.empty()) {
      for (const auto& row : rows) {
        HistoricalMarketdataSnapshotRow snapshot;
        snapshot.event_time_ms = row.event_time_ms;
        snapshot.symbol = "BTCUSDT";
        snapshot.mid_price = 60000.0;
        source.push_back(snapshot);
      }
    }
    std::vector<HistoricalMarketdataSnapshotRow> filtered;
    for (const auto& row : source) {
      if (row.event_time_ms >= from_ms && row.event_time_ms <= to_ms) {
        filtered.push_back(row);
      }
    }
    std::vector<HistoricalMarketdataSnapshotRow> out;
    for (int64_t i = offset;
         i < static_cast<int64_t>(filtered.size()) && i < offset + limit; ++i) {
      out.push_back(filtered[i]);
    }
    return out;
  }
};

struct FakeConfigRepo final : public IReplayConfigRepository {
  std::unordered_map<std::string, StoredConfigDocument> solvers;
  std::unordered_map<std::string, StoredConfigDocument> risks;
  std::unordered_map<std::string, StoredConfigDocument> fees;
  std::unordered_map<std::string, StoredConfigDocument> rewards;

  std::optional<StoredConfigDocument> GetSolverConfig(const std::string& id) override {
    auto it = solvers.find(id);
    if (it == solvers.end()) return std::nullopt;
    return it->second;
  }
  std::optional<StoredConfigDocument> GetRiskLimits(const std::string& id) override {
    auto it = risks.find(id);
    if (it == risks.end()) return std::nullopt;
    return it->second;
  }
  std::optional<StoredConfigDocument> GetFeeModel(const std::string& id) override {
    auto it = fees.find(id);
    if (it == fees.end()) return std::nullopt;
    return it->second;
  }
  std::optional<StoredConfigDocument> GetRewardConfig(const std::string& id) override {
    auto it = rewards.find(id);
    if (it == rewards.end()) return std::nullopt;
    return it->second;
  }
};

FakeConfigRepo MakeConfigRepo() {
  FakeConfigRepo repo;
  repo.solvers["solver-1"] = {"solver-1", 1, R"({"maxiterations":128})"};
  repo.risks["risk-1"] = {"risk-1", 2, R"({"maxposition":100})"};
  repo.fees["fee-default"] = {"fee-default", 3, R"({"makerfeerate":0.0002})"};
  repo.rewards["reward-1"] = {"reward-1", 4, R"({"mode":"incrementalPnL"})"};
  return repo;
}

CreateReplaySession::Request MakeValidRequest() {
  CreateReplaySession::Request req;
  req.user_id = "user-42";
  req.name = "Replay smoke";
  req.date_range_from =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{1700000000000}};
  req.date_range_to =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{1700003600000}};
  req.strategy = {
      {
          .symbol = "BTCUSDT",
          .side = "buy",
          .pL = 58000.0,
          .pH = 62000.0,
          .qrate = 0.5,
          .qmax = 100.0,
          .executionwindow = 3600,
      },
  };
  req.solver_config_id = "solver-1";
  req.risk_limits_id = "risk-1";
  req.fee_model_id = "fee-default";
  req.reward_config_id = "reward-1";
  req.random_seed = 7;
  req.tolerance = 1e-6;
  return req;
}

CreateReplaySession::Clock FixedClock() {
  return []() {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1711111111222}};
  };
}

CreateReplaySession::IdGenerator FixedId() {
  return []() { return "00000000-0000-0000-0000-000000000777"; };
}

CreateReplaySession BuildUseCase(FakeSessionRepo* session_repo,
                                 ReplayConfigSnapshotBuilder* snapshot_builder,
                                 FakeHistoryLoader* history_loader) {
  CreateReplaySession::Dependencies deps;
  deps.session_repo = session_repo;
  deps.config_builder = snapshot_builder;
  deps.historical_loader = history_loader;
  return CreateReplaySession(deps, FixedClock(), FixedId());
}

bool test_happy_path_creates_pending_session() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto result = uc.Run(MakeValidRequest());
  if (!Check(result.ok, "happy path must succeed")) return false;
  if (!Check(session_repo.create_calls == 1, "repo.Create called exactly once")) return false;
  if (!Check(history_loader.batch_calls == 1,
             "batchresults preflight must run at create time")) {
    return false;
  }
  if (!Check(history_loader.fill_calls == 1,
             "fills preflight must run at create time")) {
    return false;
  }
  if (!Check(history_loader.snapshot_calls == 1,
             "marketdata snapshot preflight must run at create time")) {
    return false;
  }

  if (!Check(result.created.session_id == "00000000-0000-0000-0000-000000000777",
             "deterministic id is used")) return false;
  if (!Check(result.created.status == ReplaySessionStatus::kPending,
             "status = pending")) return false;
  if (!Check(result.created.progress_batches == 0, "progress starts at 0")) return false;
  if (!Check(result.created.total_batches.has_value() &&
                 *result.created.total_batches == 1,
             "total_batches must be counted at create time")) return false;
  if (!Check(!result.created.started_at.has_value(), "started_at must be empty")) return false;
  if (!Check(!result.created.completed_at.has_value(), "completed_at must be empty")) return false;
  if (!Check(!result.created.error_details.has_value(), "error_details must be empty")) return false;
  if (!Check(result.created.session_config_snapshot_json.has_value(),
             "snapshot JSON must be stored")) return false;
  if (!Check(Contains(*result.created.session_config_snapshot_json, "\"solver_config\""),
             "snapshot payload must contain solver_config")) return false;

  const auto fee_json = json::parse(result.created.fee_model_json, nullptr, false);
  if (!Check(!fee_json.is_discarded() && fee_json.is_object(),
             "fee_model_json must be valid JSON object")) return false;
  if (!Check(fee_json.value("id", "") == "fee-default",
             "fee model id must be wrapped as JSON")) return false;
  return true;
}

bool test_invalid_date_range_does_not_create() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto req = MakeValidRequest();
  req.date_range_from = req.date_range_to + std::chrono::milliseconds(1);

  auto result = uc.Run(req);
  if (!Check(!result.ok, "invalid date range must fail")) return false;
  if (!Check(result.error_code == "validation_error", "error_code=validation_error")) return false;
  if (!Check(session_repo.create_calls == 0, "repo.Create must not be called")) return false;
  if (!Check(history_loader.batch_calls == 0, "history must not be checked on invalid input"))
    return false;
  return true;
}

bool test_missing_history_creates_pending_with_zero_total_for_run_failure() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;  // empty rows -> no history
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto result = uc.Run(MakeValidRequest());
  if (!Check(result.ok, "missing history should not fail create")) return false;
  if (!Check(result.created.status == ReplaySessionStatus::kPending,
             "missing history still creates pending session")) return false;
  if (!Check(session_repo.create_calls == 1, "repo.Create must be called")) return false;
  if (!Check(history_loader.batch_calls == 1,
             "batchresults preflight must run for missing history")) return false;
  if (!Check(history_loader.fill_calls == 1,
             "fills preflight must run for missing history")) return false;
  if (!Check(history_loader.snapshot_calls == 1,
             "marketdata snapshot preflight must run for missing history")) return false;
  if (!Check(result.created.total_batches.has_value() &&
                 *result.created.total_batches == 0,
             "missing history must store total_batches=0 for API visibility")) return false;
  return true;
}

bool test_history_preflight_failure_still_creates_pending_for_run_failure() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.throw_on_batch_load = true;
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto result = uc.Run(MakeValidRequest());
  if (!Check(result.ok, "preflight failure should not fail create")) return false;
  if (!Check(result.created.status == ReplaySessionStatus::kPending,
             "preflight failure still creates pending session")) return false;
  if (!Check(session_repo.create_calls == 1,
             "repo.Create must be called after preflight failure")) return false;
  if (!Check(!result.created.total_batches.has_value(),
             "unknown preflight total stays nullopt")) return false;
  return true;
}

bool test_invalid_strategy_cases() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  std::vector<CreateReplaySession::Request> cases;

  auto empty_strategy = MakeValidRequest();
  empty_strategy.strategy.clear();
  cases.push_back(std::move(empty_strategy));

  auto bad_side = MakeValidRequest();
  bad_side.strategy[0].side = "hold";
  cases.push_back(std::move(bad_side));

  auto bad_prices = MakeValidRequest();
  bad_prices.strategy[0].pL = 70000.0;
  bad_prices.strategy[0].pH = 65000.0;
  cases.push_back(std::move(bad_prices));

  auto bad_qrate = MakeValidRequest();
  bad_qrate.strategy[0].qrate = 0.0;
  cases.push_back(std::move(bad_qrate));

  auto bad_qmax = MakeValidRequest();
  bad_qmax.strategy[0].qmax = 0.0;
  cases.push_back(std::move(bad_qmax));

  auto bad_execution_window = MakeValidRequest();
  bad_execution_window.strategy[0].executionwindow = 0;
  cases.push_back(std::move(bad_execution_window));

  for (size_t i = 0; i < cases.size(); ++i) {
    auto result = uc.Run(cases[i]);
    if (!Check(!result.ok, "invalid strategy case must fail #" + std::to_string(i))) return false;
    if (!Check(result.error_code == "validation_error",
               "error_code must be validation_error #" + std::to_string(i))) return false;
  }

  if (!Check(session_repo.create_calls == 0,
             "repo.Create must not be called for invalid strategy")) return false;
  return true;
}

bool test_config_snapshot_failure_does_not_create() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  config_repo.risks.erase("risk-1");
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto result = uc.Run(MakeValidRequest());
  if (!Check(!result.ok, "missing config must fail")) return false;
  if (!Check(result.error_code == "config_error", "error_code=config_error")) return false;
  if (!Check(session_repo.create_calls == 0, "repo.Create must not be called")) return false;
  return true;
}

bool test_fee_model_id_wrapped_into_json_object() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  config_repo.fees["maker-taker-spot"] = {
      "maker-taker-spot",
      8,
      R"({"makerfeerate":0.0001,"takerfeerate":0.0003})"};
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto req = MakeValidRequest();
  req.fee_model_id = "maker-taker-spot";
  auto result = uc.Run(req);
  if (!Check(result.ok, "fee_model_id scenario must succeed")) return false;

  const auto fee_json = json::parse(result.created.fee_model_json, nullptr, false);
  if (!Check(!fee_json.is_discarded() && fee_json.is_object(), "fee_model_json is object"))
    return false;
  if (!Check(fee_json.value("id", "") == "maker-taker-spot",
             "id field must be preserved")) return false;
  return true;
}

bool test_inline_fee_model_is_preserved_as_json() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto req = MakeValidRequest();
  req.fee_model_id = std::nullopt;
  req.fee_model_inline_json = R"({"makerfeerate":0.0002,"takerfeerate":0.0005})";

  auto result = uc.Run(req);
  if (!Check(result.ok, "inline fee model must succeed")) return false;

  const auto fee_json = json::parse(result.created.fee_model_json, nullptr, false);
  if (!Check(!fee_json.is_discarded() && fee_json.is_object(),
             "inline fee model must be valid object")) return false;
  if (!Check(std::abs(fee_json.value("makerfeerate", 0.0) - 0.0002) < 1e-12,
             "makerfeerate preserved")) return false;
  if (!Check(std::abs(fee_json.value("takerfeerate", 0.0) - 0.0005) < 1e-12,
             "takerfeerate preserved")) return false;
  if (!Check(result.created.session_config_snapshot_json.has_value() &&
                 Contains(*result.created.session_config_snapshot_json,
                          "\"fee_model\":{\"id\":\"\",\"version\":0,\"inline_override\":true"),
             "snapshot must include inline fee model section")) {
    return false;
  }
  return true;
}

bool test_inline_solver_and_risk_overrides_are_preserved_as_json() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto req = MakeValidRequest();
  req.solver_config_id.clear();
  req.solver_config_inline_json =
      R"({"batchintervalms":1000,"maxiterations":128,"tolerance":0.000001,"epsilonliquidity":0.0,"feemodel":{"makerfeerate":0.0002,"takerfeerate":0.0005}})";
  req.risk_limits_id.clear();
  req.risk_limits_inline_json =
      R"({"maxnotional":1000000,"maxposition":100,"maxleverage":3,"maxorderrate":25,"whitelist":["BTCUSDT"]})";

  auto result = uc.Run(req);
  if (!Check(result.ok, "full inline solver/risk overrides must succeed")) return false;
  if (!Check(result.created.solver_config_id == "inline",
             "inline solver config id must be stored as inline")) return false;
  if (!Check(result.created.risk_limits_id == "inline",
             "inline risk limits id must be stored as inline")) return false;
  if (!Check(result.created.session_config_snapshot_json.has_value(),
             "inline override snapshot must be stored")) return false;
  if (!Check(Contains(*result.created.session_config_snapshot_json,
                      "\"solver_config\":{\"id\":\"inline\",\"version\":0,\"inline_override\":true"),
             "snapshot must mark solver inline_override=true")) {
    return false;
  }
  return Check(Contains(*result.created.session_config_snapshot_json,
                        "\"risk_limits\":{\"id\":\"inline\",\"version\":0,\"inline_override\":true"),
               "snapshot must mark risk inline_override=true");
}

bool test_json_strategy_executionwindow_is_optional() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto req = MakeValidRequest();
  req.strategy.clear();
  req.strategy_json =
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":0.5,"qmax":100}])";

  auto result = uc.Run(req);
  if (!Check(result.ok, "JSON strategy without executionwindow must succeed")) return false;
  if (!Check(!Contains(result.created.strategy_json, "executionwindow"),
             "executionwindow should stay absent when not provided")) return false;

  req.strategy_json =
      R"([{"symbol":"BTCUSDT","side":"buy","pL":58000,"pH":62000,"qrate":0.5,"qmax":100,"executionwindow":0}])";
  result = uc.Run(req);
  if (!Check(!result.ok, "executionwindow=0 must fail when provided")) return false;
  return Check(result.error_code == "validation_error", "error_code=validation_error");
}

bool test_invalid_inline_overrides_do_not_create() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  history_loader.rows.push_back({"batch-1", 1700000100000, "", "", "", 0.0, 0, 0, 0, "", "", "", "", 0});
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  std::vector<CreateReplaySession::Request> cases;

  auto bad_fee = MakeValidRequest();
  bad_fee.fee_model_id = std::nullopt;
  bad_fee.fee_model_inline_json = R"({"makerfeerate":-0.1,"takerfeerate":0.0005})";
  cases.push_back(std::move(bad_fee));

  auto bad_solver = MakeValidRequest();
  bad_solver.solver_config_id.clear();
  bad_solver.solver_config_inline_json = R"({"batchintervalms":0})";
  cases.push_back(std::move(bad_solver));

  auto missing_solver_fields = MakeValidRequest();
  missing_solver_fields.solver_config_id.clear();
  missing_solver_fields.solver_config_inline_json = R"({})";
  cases.push_back(std::move(missing_solver_fields));

  auto bad_risk = MakeValidRequest();
  bad_risk.risk_limits_id.clear();
  bad_risk.risk_limits_inline_json = R"({"maxnotional":1000,"whitelist":["BTCUSDT",""]})";
  cases.push_back(std::move(bad_risk));

  auto bad_risk_whitelist = MakeValidRequest();
  bad_risk_whitelist.risk_limits_id.clear();
  bad_risk_whitelist.risk_limits_inline_json =
      R"({"maxnotional":1000,"maxposition":100,"maxleverage":3,"maxorderrate":25,"whitelist":["BTCUSDT",""]})";
  cases.push_back(std::move(bad_risk_whitelist));

  for (size_t i = 0; i < cases.size(); ++i) {
    auto result = uc.Run(cases[i]);
    if (!Check(!result.ok, "invalid inline override must fail #" + std::to_string(i))) {
      return false;
    }
    if (!Check(result.error_code == "validation_error",
               "inline override error_code must be validation_error #" +
                   std::to_string(i))) {
      return false;
    }
  }
  return Check(session_repo.create_calls == 0,
               "repo.Create must not be called for invalid inline overrides");
}

bool test_invalid_reward_mode_does_not_create() {
  FakeSessionRepo session_repo;
  FakeHistoryLoader history_loader;
  auto config_repo = MakeConfigRepo();
  ReplayConfigSnapshotBuilder snapshot_builder(&config_repo);
  auto uc = BuildUseCase(&session_repo, &snapshot_builder, &history_loader);

  auto bad_mode = MakeValidRequest();
  bad_mode.reward_config_id = std::nullopt;
  bad_mode.reward_mode = "hybrid";
  auto result = uc.Run(bad_mode);
  if (!Check(!result.ok, "unsupported reward_mode must fail")) return false;
  if (!Check(result.error_code == "validation_error", "reward_mode validation error"))
    return false;

  auto bad_inline = MakeValidRequest();
  bad_inline.reward_config_id = std::nullopt;
  bad_inline.reward_config_inline_json = R"({"mode":"hybrid"})";
  result = uc.Run(bad_inline);
  if (!Check(!result.ok, "unsupported inline reward config must fail")) return false;
  if (!Check(result.error_code == "validation_error", "inline reward validation error"))
    return false;

  return Check(session_repo.create_calls == 0,
               "repo.Create must not be called for invalid reward mode");
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

  run("happy_path_creates_pending_session", &test_happy_path_creates_pending_session);
  run("invalid_date_range_does_not_create", &test_invalid_date_range_does_not_create);
  run("missing_history_creates_pending_with_zero_total_for_run_failure",
      &test_missing_history_creates_pending_with_zero_total_for_run_failure);
  run("history_preflight_failure_still_creates_pending_for_run_failure",
      &test_history_preflight_failure_still_creates_pending_for_run_failure);
  run("invalid_strategy_cases", &test_invalid_strategy_cases);
  run("config_snapshot_failure_does_not_create", &test_config_snapshot_failure_does_not_create);
  run("fee_model_id_wrapped_into_json_object", &test_fee_model_id_wrapped_into_json_object);
  run("inline_fee_model_is_preserved_as_json", &test_inline_fee_model_is_preserved_as_json);
  run("inline_solver_and_risk_overrides_are_preserved_as_json",
      &test_inline_solver_and_risk_overrides_are_preserved_as_json);
  run("json_strategy_executionwindow_is_optional",
      &test_json_strategy_executionwindow_is_optional);
  run("invalid_inline_overrides_do_not_create", &test_invalid_inline_overrides_do_not_create);
  run("invalid_reward_mode_does_not_create", &test_invalid_reward_mode_does_not_create);

  if (!all_passed) return EXIT_FAILURE;
  std::cout << "[OK] backtest_create_replay_session_test" << std::endl;
  return EXIT_SUCCESS;
}
