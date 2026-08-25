#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/compare_replay_sessions_uc.hpp"
#include "app/replay_compare_ports.hpp"
#include "app/replay_session.hpp"
#include "app/replay_session_repository_port.hpp"

namespace {

using cex::backtest::app::CompareReplaySessions;
using cex::backtest::app::IReplayAgentLogReader;
using cex::backtest::app::IReplaySummaryReader;
using cex::backtest::app::ReplayAgentLogRef;
using cex::backtest::app::ReplaySession;
using cex::backtest::app::ReplaySessionListFilter;
using cex::backtest::app::ReplaySessionRepositoryPort;
using cex::backtest::app::ReplaySessionStatePatch;
using cex::backtest::app::ReplaySessionStatus;
using cex::backtest::app::ReplaySummary;

bool Check(const bool condition, const std::string& message) {
  if (condition) return true;
  std::cerr << "[FAIL] " << message << std::endl;
  return false;
}

struct FakeSessionRepo final : public ReplaySessionRepositoryPort {
  std::unordered_map<std::string, ReplaySession> sessions;

  ReplaySession Create(const ReplaySession& session) override {
    sessions[session.session_id] = session;
    return session;
  }
  std::optional<ReplaySession> GetById(const std::string& session_id) override {
    auto it = sessions.find(session_id);
    if (it == sessions.end()) return std::nullopt;
    return it->second;
  }
  std::vector<ReplaySession> List(const ReplaySessionListFilter&) override { return {}; }
  bool UpdateState(const std::string&, const ReplaySessionStatePatch&) override { return true; }
  std::vector<ReplaySession> GetRetryChain(const std::string&) override { return {}; }
};

struct FakeSummaryReader final : public IReplaySummaryReader {
  std::unordered_map<std::string, ReplaySummary> summaries;

  std::optional<ReplaySummary> GetSummaryBySessionId(
      const std::string& session_id) override {
    auto it = summaries.find(session_id);
    if (it == summaries.end()) return std::nullopt;
    return it->second;
  }
};

struct FakeAgentLogReader final : public IReplayAgentLogReader {
  std::unordered_map<std::string, std::vector<ReplayAgentLogRef>> refs;

  std::vector<ReplayAgentLogRef> LoadAgentLogRefsBySessionId(
      const std::string& session_id) override {
    auto it = refs.find(session_id);
    if (it == refs.end()) return {};
    return it->second;
  }
};

ReplaySession MakeSession(const std::string& session_id,
                          const std::string& strategy_json,
                          const int64_t from_ms,
                          const int64_t to_ms) {
  ReplaySession session;
  session.session_id = session_id;
  session.user_id = "user-1";
  session.name = session_id;
  session.strategy_json = strategy_json;
  session.date_range_from =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{from_ms}};
  session.date_range_to =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{to_ms}};
  session.solver_config_id = "solver-1";
  session.risk_limits_id = "risk-1";
  session.fee_model_json = R"({"maker":0.0})";
  session.status = ReplaySessionStatus::kCompleted;
  session.total_batches = 2;
  session.progress_batches = 2;
  session.created_at =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{from_ms}};
  return session;
}

ReplaySummary MakeSummary(const std::string& session_id,
                          const double total_pnl,
                          const double avg_is,
                          const double sharpe,
                          const double fill_rate,
                          const double max_drawdown,
                          const double avg_solve_time_ms,
                          const double avg_pnl = 0.0,
                          const double std_pnl = 0.0,
                          const double avg_vwap = 0.0) {
  ReplaySummary summary;
  summary.session_id = session_id;
  summary.total_batches = 2;
  summary.processed_batches = 2;
  summary.failed_batches = 0;
  summary.total_pnl = total_pnl;
  summary.avg_pnl = avg_pnl;
  summary.std_pnl = std_pnl;
  summary.avg_is = avg_is;
  summary.sharpe = sharpe;
  summary.avg_fill_rate = fill_rate;
  summary.max_drawdown = max_drawdown;
  summary.avg_solve_time_ms = avg_solve_time_ms;
  summary.avg_vwap = avg_vwap;
  return summary;
}

CompareReplaySessions BuildUseCase(FakeSessionRepo* repo,
                                   FakeSummaryReader* summary_reader,
                                   FakeAgentLogReader* agentlog_reader) {
  CompareReplaySessions::Dependencies deps;
  deps.session_repo = repo;
  deps.summary_reader = summary_reader;
  deps.agent_log_reader = agentlog_reader;
  return CompareReplaySessions(deps);
}

bool test_happy_path_computes_metric_deltas() {
  FakeSessionRepo repo;
  FakeSummaryReader summary_reader;
  FakeAgentLogReader agentlog_reader;
  repo.sessions["sess-a"] =
      MakeSession("sess-a",
                  R"([{"symbol":"BTCUSDT","side":"buy"},{"symbol":"ETHUSDT","side":"sell"}])",
                  1000,
                  2000);
  repo.sessions["sess-b"] =
      MakeSession("sess-b",
                  R"([{"symbol":"ETHUSDT","side":"buy"},{"symbol":"BTCUSDT","side":"sell"}])",
                  1000,
                  2000);
  summary_reader.summaries["sess-a"] = MakeSummary("sess-a", 10.0, -1.2, 0.8, 0.91, 3.0, 12.0, 5.0, 1.4, 60100.0);
  summary_reader.summaries["sess-b"] = MakeSummary("sess-b", 14.0, -0.7, 1.1, 0.95, 2.0, 10.0, 7.0, 1.1, 60125.0);
  agentlog_reader.refs["sess-a"] = {{0, "b1"}, {1, "b2"}};
  agentlog_reader.refs["sess-b"] = {{0, "b1"}, {1, "b2"}};

  auto uc = BuildUseCase(&repo, &summary_reader, &agentlog_reader);
  auto result = uc.Run({"sess-a", "sess-b"});

  if (!Check(result.ok, "compare must succeed")) return false;
  if (!Check(result.compatible, "sessions must be compatible")) return false;
  if (!Check(std::abs(result.total_pnl.delta - 4.0) < 1e-9, "total pnl delta")) return false;
  if (!Check(std::abs(result.avg_pnl.delta - 2.0) < 1e-9, "avg pnl delta")) return false;
  if (!Check(std::abs(result.std_pnl.delta + 0.3) < 1e-9, "std pnl delta")) return false;
  if (!Check(std::abs(result.avg_is.delta - 0.5) < 1e-9, "avg is delta")) return false;
  if (!Check(std::abs(result.sharpe.delta - 0.3) < 1e-9, "sharpe delta")) return false;
  if (!Check(std::abs(result.fill_rate.delta - 0.04) < 1e-9, "fill rate delta")) return false;
  if (!Check(std::abs(result.max_drawdown.delta + 1.0) < 1e-9, "drawdown delta")) return false;
  if (!Check(std::abs(result.avg_vwap.delta - 25.0) < 1e-9, "avg vwap delta")) return false;
  return Check(std::abs(result.avg_solve_time_ms.delta + 2.0) < 1e-9,
               "avg solve time delta");
}

bool test_incompatible_date_range_is_reported() {
  FakeSessionRepo repo;
  FakeSummaryReader summary_reader;
  FakeAgentLogReader agentlog_reader;
  repo.sessions["sess-a"] = MakeSession("sess-a", R"([{"symbol":"BTCUSDT"}])", 1000, 2000);
  repo.sessions["sess-b"] = MakeSession("sess-b", R"([{"symbol":"BTCUSDT"}])", 1000, 3000);
  summary_reader.summaries["sess-a"] = MakeSummary("sess-a", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
  summary_reader.summaries["sess-b"] = MakeSummary("sess-b", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
  agentlog_reader.refs["sess-a"] = {{0, "b1"}};
  agentlog_reader.refs["sess-b"] = {{0, "b1"}};

  auto uc = BuildUseCase(&repo, &summary_reader, &agentlog_reader);
  auto result = uc.Run({"sess-a", "sess-b"});

  if (!Check(result.ok, "compare still returns result")) return false;
  if (!Check(!result.compatible, "date-range mismatch must be incompatible")) return false;
  return Check(result.error_code == "incompatible_compare", "error code");
}

bool test_incompatible_batch_order_is_reported() {
  FakeSessionRepo repo;
  FakeSummaryReader summary_reader;
  FakeAgentLogReader agentlog_reader;
  repo.sessions["sess-a"] = MakeSession("sess-a", R"([{"symbol":"BTCUSDT"}])", 1000, 2000);
  repo.sessions["sess-b"] = MakeSession("sess-b", R"([{"symbol":"BTCUSDT"}])", 1000, 2000);
  summary_reader.summaries["sess-a"] = MakeSummary("sess-a", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
  summary_reader.summaries["sess-b"] = MakeSummary("sess-b", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
  agentlog_reader.refs["sess-a"] = {{0, "b1"}, {1, "b2"}};
  agentlog_reader.refs["sess-b"] = {{0, "b2"}, {1, "b1"}};

  auto uc = BuildUseCase(&repo, &summary_reader, &agentlog_reader);
  auto result = uc.Run({"sess-a", "sess-b"});

  if (!Check(result.ok, "compare still returns result")) return false;
  if (!Check(!result.compatible, "batch-order mismatch must be incompatible")) return false;
  return Check(!result.compatibility.same_batch_order, "same_batch_order=false");
}

bool test_instrument_set_supports_strategy_instrument_field() {
  FakeSessionRepo repo;
  FakeSummaryReader summary_reader;
  FakeAgentLogReader agentlog_reader;
  repo.sessions["sess-a"] =
      MakeSession("sess-a", R"([{"instrument":"BTCUSDT","note":"symbol is not parsed from text"}])", 1000, 2000);
  repo.sessions["sess-b"] =
      MakeSession("sess-b", R"([{"symbol":"BTCUSDT"}])", 1000, 2000);
  summary_reader.summaries["sess-a"] = MakeSummary("sess-a", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
  summary_reader.summaries["sess-b"] = MakeSummary("sess-b", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
  agentlog_reader.refs["sess-a"] = {{0, "b1"}};
  agentlog_reader.refs["sess-b"] = {{0, "b1"}};

  auto uc = BuildUseCase(&repo, &summary_reader, &agentlog_reader);
  auto result = uc.Run({"sess-a", "sess-b"});

  if (!Check(result.ok, "compare with instrument field succeeds")) return false;
  if (!Check(result.compatible, "instrument and symbol fields should match")) return false;
  return Check(result.compatibility.same_instrument_set,
               "same_instrument_set=true");
}

bool test_missing_summary_returns_no_data() {
  FakeSessionRepo repo;
  FakeSummaryReader summary_reader;
  FakeAgentLogReader agentlog_reader;
  repo.sessions["sess-a"] = MakeSession("sess-a", R"([{"symbol":"BTCUSDT"}])", 1000, 2000);
  repo.sessions["sess-b"] = MakeSession("sess-b", R"([{"symbol":"BTCUSDT"}])", 1000, 2000);
  summary_reader.summaries["sess-a"] = MakeSummary("sess-a", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
  agentlog_reader.refs["sess-a"] = {{0, "b1"}};
  agentlog_reader.refs["sess-b"] = {{0, "b1"}};

  auto uc = BuildUseCase(&repo, &summary_reader, &agentlog_reader);
  auto result = uc.Run({"sess-a", "sess-b"});

  if (!Check(!result.ok, "compare must fail without summary")) return false;
  return Check(result.error_code == "no_data", "no_data code");
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_happy_path_computes_metric_deltas() && ok;
  ok = test_incompatible_date_range_is_reported() && ok;
  ok = test_incompatible_batch_order_is_reported() && ok;
  ok = test_instrument_set_supports_strategy_instrument_field() && ok;
  ok = test_missing_summary_returns_no_data() && ok;
  if (ok) {
    std::cout << "[OK] backtest_compare_replay_sessions_test passed" << std::endl;
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
