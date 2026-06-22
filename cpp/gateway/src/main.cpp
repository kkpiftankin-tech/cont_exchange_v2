// ============================================================================
// gateway/main.cpp — entry point HTTP gateway сервиса (edge для всех F-XX).
//
// Что делает:
//   - REST HTTP server на Crow framework.
//   - Routes:
//       * POST /v1/flow-orders   — proxy в order_flow gRPC (F-02).
//       * POST /v1/combo-orders  — F-09 combo orders.
//       * GET  /v1/market-data/* — proxy в market_data gRPC (F-05).
//       * F-15 replay endpoints  — sessions, compare, results.
//       * /healthz                — liveness probe.
//   - F-15 replay infrastructure:
//       * Replay sessions PG repo + RBAC.
//       * SSE/long-poll streaming для replay.results через ReplayResultsHub.
//       * Kafka publisher для replay.commands.
//   - Auth middleware (см. transport/auth_middleware.hpp) — JWT/header-based.
//
// Это edge service — единственный публичный entrypoint снаружи.
// CLAUDE.md §22: prod требует mTLS/TLS termination перед gateway.
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <crow.h>

#include "cex/common/env.hpp"
#include "cex/common/kafka.hpp"
#include "cex/common/log.hpp"

#include "app/compare_replay_sessions_uc.hpp"
#include "infra/clickhouse_storage.hpp"
#include "infra/order_flow_client.hpp"
#include "infra/market_data_client.hpp"
#include "infra/postgres/postgres_rbac_repository.hpp"
#include "infra/postgres/postgres_replay_session_repository.hpp"
#include "infra/replay_commands_kafka_publisher.hpp"
#include "infra/replay_results_hub.hpp"
#include "infra/replay_results_kafka_consumer.hpp"
#include "transport/auth_middleware.hpp"
#include "transport/http_gateway.hpp"

namespace {

std::int64_t ToEpochMillis(std::chrono::system_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             tp.time_since_epoch())
      .count();
}

std::string StatusToString(cex::backtest::app::ReplaySessionStatus status) {
  using cex::backtest::app::ReplaySessionStatus;
  switch (status) {
    case ReplaySessionStatus::kPending: return "pending";
    case ReplaySessionStatus::kRunning: return "running";
    case ReplaySessionStatus::kCompleted: return "completed";
    case ReplaySessionStatus::kFailed: return "failed";
    case ReplaySessionStatus::kCancelled: return "cancelled";
  }
  return "pending";
}

std::optional<cex::backtest::app::ReplaySessionStatus> StatusFromString(
    const std::string& raw) {
  using cex::backtest::app::ReplaySessionStatus;
  if (raw == "pending") return ReplaySessionStatus::kPending;
  if (raw == "running") return ReplaySessionStatus::kRunning;
  if (raw == "completed") return ReplaySessionStatus::kCompleted;
  if (raw == "failed") return ReplaySessionStatus::kFailed;
  if (raw == "cancelled") return ReplaySessionStatus::kCancelled;
  return std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> ParseEpochMillisParam(
    const char* raw) {
  if (raw == nullptr || std::string(raw).empty()) return std::nullopt;
  try {
    const auto millis = std::stoll(raw);
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds(millis)};
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::int32_t> ParseInt32Param(const char* raw) {
  if (raw == nullptr || std::string(raw).empty()) return std::nullopt;
  try {
    return static_cast<std::int32_t>(std::stoi(raw));
  } catch (...) {
    return std::nullopt;
  }
}

crow::json::wvalue SessionToJson(
    const cex::backtest::app::ReplaySession& session) {
  crow::json::wvalue out;
  out["sessionid"] = session.session_id;
  out["userid"] = session.user_id;
  out["name"] = session.name;
  out["strategy"] = session.strategy_json;
  out["daterangefrom"] = ToEpochMillis(session.date_range_from);
  out["daterangeto"] = ToEpochMillis(session.date_range_to);
  out["solverconfigid"] = session.solver_config_id;
  out["risklimitsid"] = session.risk_limits_id;
  out["feemodel"] = session.fee_model_json;
  if (session.session_config_snapshot_json.has_value()) {
    out["sessionconfigsnapshot"] = *session.session_config_snapshot_json;
  }
  out["status"] = StatusToString(session.status);
  if (session.total_batches.has_value()) {
    out["totalbatches"] = *session.total_batches;
  } else {
    out["totalbatches"] = 0;
  }
  out["progressbatches"] = session.progress_batches;
  out["createdat"] = ToEpochMillis(session.created_at);
  if (session.started_at.has_value()) {
    out["startedat"] = ToEpochMillis(*session.started_at);
  }
  if (session.completed_at.has_value()) {
    out["completedat"] = ToEpochMillis(*session.completed_at);
  }
  if (session.error_details.has_value()) {
    out["errordetails"] = *session.error_details;
  }
  if (session.retry_parent_id.has_value()) {
    out["retryparentid"] = *session.retry_parent_id;
  }
  return out;
}

crow::json::wvalue SummaryToJson(
    const cex::backtest::app::ReplaySummary& summary) {
  crow::json::wvalue out;
  if (!summary.summary_id.empty()) {
    out["summaryid"] = summary.summary_id;
  }
  out["sessionid"] = summary.session_id;
  out["avgis"] = summary.avg_is;
  out["totalpnl"] = summary.total_pnl;
  out["avgpnl"] = summary.avg_pnl;
  out["stdpnl"] = summary.std_pnl;
  out["sharpe"] = summary.sharpe;
  out["fillrate"] = summary.avg_fill_rate;
  out["avgvwap"] = summary.avg_vwap;
  out["avgsolvetime"] = summary.avg_solve_time_ms;
  out["maxdrawdown"] = summary.max_drawdown;
  out["totalbatches"] = summary.total_batches;
  out["totalfillevents"] = summary.total_fill_events;
  out["processedbatches"] = summary.processed_batches;
  out["failedbatches"] = summary.failed_batches;
  out["partial"] = summary.partial;
  out["avgis_rule"] = summary.avgis_rule;
  out["decision_price_source"] = summary.decision_price_source;
  out["no_requested_volume"] = summary.no_requested_volume;
  if (summary.created_at.time_since_epoch().count() != 0) {
    out["createdat"] = ToEpochMillis(summary.created_at);
  }
  return out;
}

crow::json::wvalue AgentLogToJson(
    const cex::backtest::app::AgentLogEntry& log) {
  crow::json::wvalue out;
  out["logid"] = log.log_id;
  out["sessionid"] = log.session_id;
  out["batchseq"] = log.batch_seq;
  out["batch_seq"] = log.batch_seq;
  out["originalbatchid"] = log.original_batch_id;
  out["original_batch_id"] = log.original_batch_id;
  out["state"] = log.state_json;
  out["action"] = log.action_json;
  out["reward"] = log.reward;
  out["pnl"] = log.pnl;
  out["isvalue"] = log.is_value;
  out["is_value"] = log.is_value;
  out["fillrate"] = log.fill_rate;
  out["fill_rate"] = log.fill_rate;
  out["solvetime_ms"] = log.solve_time_ms;
  out["solve_time_ms"] = log.solve_time_ms;
  out["solvetimems"] = log.solve_time_ms;
  out["residualnorm"] = log.residual_norm;
  out["residual_norm"] = log.residual_norm;
  out["fills"] = log.fills_json;
  out["batchresult"] = log.batch_result_json;
  out["batch_result"] = log.batch_result_json;
  out["metrics"] = log.metrics_json;
  out["timestamp"] = log.event_time_ms;
  out["riskstatus"] = log.risk_status;
  out["risk_status"] = log.risk_status;
  out["solvererrorflag"] = log.solver_error_flag;
  out["solver_error_flag"] = log.solver_error_flag;
  out["errorcode"] = log.error_code;
  out["error_code"] = log.error_code;
  out["errordetails"] = log.error_details;
  out["error_details"] = log.error_details;
  out["sessionconfigsnapshotversion"] = log.session_config_snapshot_version;
  out["session_config_snapshot_version"] = log.session_config_snapshot_version;
  return out;
}

std::unordered_map<std::string, std::string> ParseFilterString(
    const std::string& filter) {
  std::unordered_map<std::string, std::string> out;
  std::stringstream ss(filter);
  std::string part;
  while (std::getline(ss, part, ';')) {
    const auto pos = part.find('=');
    if (pos == std::string::npos) continue;
    const std::string key = part.substr(0, pos);
    const std::string value = part.substr(pos + 1);
    if (!key.empty() && !value.empty()) out[key] = value;
  }
  return out;
}

std::optional<std::int64_t> ParseInt64Strict(const std::string& value) {
  if (value.empty()) return std::nullopt;
  try {
    size_t parsed = 0;
    const auto out = std::stoll(value, &parsed, 10);
    if (parsed != value.size()) return std::nullopt;
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> ParseDoubleStrict(const std::string& value) {
  if (value.empty()) return std::nullopt;
  try {
    size_t parsed = 0;
    const auto out = std::stod(value, &parsed);
    if (parsed != value.size() || !std::isfinite(out)) return std::nullopt;
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

bool MatchesDoubleFilter(double actual, const std::string& expected_text) {
  const auto expected = ParseDoubleStrict(expected_text);
  if (!expected.has_value()) return false;
  const double scale = std::max(1.0, std::max(std::fabs(actual), std::fabs(*expected)));
  return std::fabs(actual - *expected) <= (1e-9 * scale);
}

bool MatchesAgentLogFilter(const cex::backtest::app::AgentLogEntry& log,
                           const std::unordered_map<std::string, std::string>& filter) {
  auto find_filter = [&filter](std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
      auto it = filter.find(key);
      if (it != filter.end()) return it;
    }
    return filter.end();
  };

  const auto batch_seq = filter.find("batchseq");
  if (batch_seq != filter.end()) {
    const auto expected = ParseInt64Strict(batch_seq->second);
    if (!expected.has_value() || log.batch_seq != *expected) return false;
  }
  const auto pnl = filter.find("pnl");
  if (pnl != filter.end() && !MatchesDoubleFilter(log.pnl, pnl->second)) {
    return false;
  }
  const auto is_value = find_filter({"isvalue", "is_value"});
  if (is_value != filter.end() &&
      !MatchesDoubleFilter(log.is_value, is_value->second)) {
    return false;
  }
  const auto fill_rate = find_filter({"fillrate", "fill_rate"});
  if (fill_rate != filter.end() &&
      !MatchesDoubleFilter(log.fill_rate, fill_rate->second)) {
    return false;
  }
  const auto solver_error =
      find_filter({"solver_error", "solvererror", "solver_error_flag"});
  if (solver_error != filter.end()) {
    const bool expected = solver_error->second == "true" ||
                          solver_error->second == "1";
    if (log.solver_error_flag != expected) return false;
  }
  return true;
}

std::string BetterSide(const double a,
                       const double b,
                       const std::string& preference) {
  if (preference == "neutral") return "equal";
  if (a == b) return "equal";
  if (preference == "lower") return a < b ? "A" : "B";
  return a > b ? "A" : "B";
}

crow::json::wvalue MetricToJson(
    const std::string& key,
    const cex::backtest::app::CompareReplaySessions::MetricDelta& delta,
    const std::string& preference) {
  crow::json::wvalue out;
  out["key"] = key;
  out["valueA"] = delta.session_a;
  out["valueB"] = delta.session_b;
  out["delta"] = delta.delta;
  out["preference"] = preference;
  out["better"] = BetterSide(delta.session_a, delta.session_b, preference);
  return out;
}

}  // namespace

class PostgresSummaryRepo : public cex::gateway::transport::SummaryRepository {
public:
  explicit PostgresSummaryRepo(std::string dsn) : repo_(std::move(dsn)) {}

  crow::json::wvalue GetSummary(const std::string& id) override {
    crow::json::wvalue out;
    try {
      auto summary = repo_.GetSummaryBySessionId(id);
      if (!summary.has_value()) {
        out["sessionid"] = id;
        out["nodata"] = true;
        return out;
      }
      return SummaryToJson(*summary);
    } catch (const std::exception& ex) {
      out["sessionid"] = id;
      out["errorcode"] = "summary_storage_error";
      out["errordetails"] = ex.what();
      return out;
    }
  }

 private:
  cex::backtest::infra::PostgresReplaySessionRepository repo_;
};

class PostgresSessionsRepo : public cex::gateway::transport::SessionsRepository {
 public:
  explicit PostgresSessionsRepo(std::string dsn) : repo_(std::move(dsn)) {}

  crow::json::wvalue ListSessions(
      const crow::request& req,
      const std::optional<std::string>& owner_user_id = std::nullopt) override {
    crow::json::wvalue out;
    out["items"] = std::vector<crow::json::wvalue>{};
    out["total"] = 0;

    cex::backtest::app::ReplaySessionListFilter filter;
    const char* user_id = req.url_params.get("userid");
    if (user_id == nullptr) user_id = req.url_params.get("user_id");
    if (owner_user_id.has_value()) {
      filter.user_id = *owner_user_id;
    } else if (user_id != nullptr) {
      filter.user_id = user_id;
    }
    if (const char* status = req.url_params.get("status")) {
      filter.status = StatusFromString(status);
    }
    filter.created_from = ParseEpochMillisParam(req.url_params.get("createdatfrom"));
    filter.created_to = ParseEpochMillisParam(req.url_params.get("createdatto"));
    if (!filter.created_from.has_value() && !filter.created_to.has_value()) {
      const auto created_at = ParseEpochMillisParam(req.url_params.get("createdat"));
      filter.created_from = created_at;
      filter.created_to = created_at;
    }
    filter.replay_from = ParseEpochMillisParam(req.url_params.get("daterangefrom"));
    filter.replay_to = ParseEpochMillisParam(req.url_params.get("daterangeto"));
    filter.limit = ParseInt32Param(req.url_params.get("limit"));
    filter.offset = ParseInt32Param(req.url_params.get("offset"));

    try {
      const auto sessions = repo_.List(filter);
      int i = 0;
      for (const auto& session : sessions) {
        out["items"][i++] = SessionToJson(session);
      }
      out["total"] = static_cast<int>(sessions.size());
      return out;
    } catch (const std::exception& ex) {
      out["errorcode"] = "session_storage_error";
      out["errordetails"] = ex.what();
      return out;
    }
  }

  std::optional<crow::json::wvalue> GetSession(
      const std::string& id) override {
    try {
      auto session = repo_.GetById(id);
      if (!session.has_value()) return std::nullopt;
      return SessionToJson(*session);
    } catch (...) {
      return std::nullopt;
    }
  }

 private:
  cex::backtest::infra::PostgresReplaySessionRepository repo_;
};

class ClickHouseLogsRepo : public cex::gateway::transport::LogsRepository {
public:
  explicit ClickHouseLogsRepo(cex::backtest::infra::ClickHouseConfig cfg)
      : storage_(std::move(cfg)) {}

  crow::json::wvalue GetAgentLogs(const std::string& id,
                                  int limit,
                                  int offset,
                                  const std::string& filter) override {
    crow::json::wvalue out;
    out["items"] = std::vector<crow::json::wvalue>{};
    out["total"] = 0;
    try {
      auto rows = storage_.ReadLogsUpTo(id, 0);
      const auto parsed_filter = ParseFilterString(filter);
      rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto& row) {
                   return !MatchesAgentLogFilter(row, parsed_filter);
                 }),
                 rows.end());

      const int safe_offset = std::max(0, offset);
      const int safe_limit = std::max(1, limit);
      const int end = std::min<int>(static_cast<int>(rows.size()),
                                    safe_offset + safe_limit);
      int out_index = 0;
      for (int i = safe_offset; i < end; ++i) {
        out["items"][out_index++] = AgentLogToJson(rows[static_cast<size_t>(i)]);
      }
      out["total"] = static_cast<int>(rows.size());
      return out;
    } catch (const std::exception& ex) {
      out["errorcode"] = "agentlog_storage_error";
      out["errordetails"] = ex.what();
      return out;
    }
  }

 private:
  cex::backtest::infra::ClickHouseReplayStorage storage_;
};

class ReplayCompareRepo : public cex::gateway::transport::CompareRepository {
 public:
  ReplayCompareRepo(std::string dsn, cex::backtest::infra::ClickHouseConfig cfg)
      : session_repo_(std::move(dsn)),
        storage_(std::move(cfg)) {}

  crow::json::wvalue Compare(const std::string& session_a,
                             const std::string& session_b) override {
    cex::backtest::app::CompareReplaySessions::Dependencies deps;
    deps.session_repo = &session_repo_;
    deps.summary_reader = &session_repo_;
    deps.agent_log_reader = &storage_;

    cex::backtest::app::CompareReplaySessions uc(deps);
    cex::backtest::app::CompareReplaySessions::Request request;
    request.session_a_id = session_a;
    request.session_b_id = session_b;

    const auto result = uc.Run(request);
    crow::json::wvalue out;
    out["sessionA"] = session_a;
    out["sessionB"] = session_b;
    out["compatible"] = result.compatible;
    if (!result.error_message.empty()) {
      out["reason"] = result.error_message;
    }
    if (!result.error_code.empty()) {
      out["errorcode"] = result.error_code;
    }
    out["compatibility"]["sameDateRange"] = result.compatibility.same_date_range;
    out["compatibility"]["sameInstrumentSet"] = result.compatibility.same_instrument_set;
    out["compatibility"]["sameBatchOrder"] = result.compatibility.same_batch_order;
    out["compatibility"]["sameHistoricalInputs"] =
        result.compatibility.same_historical_inputs;

    out["metrics"][0] = MetricToJson("avgis", result.avg_is, "lower");
    out["metrics"][1] = MetricToJson("totalpnl", result.total_pnl, "higher");
    out["metrics"][2] = MetricToJson("avgpnl", result.avg_pnl, "higher");
    out["metrics"][3] = MetricToJson("stdpnl", result.std_pnl, "lower");
    out["metrics"][4] = MetricToJson("sharpe", result.sharpe, "higher");
    out["metrics"][5] = MetricToJson("fillrate", result.fill_rate, "higher");
    out["metrics"][6] = MetricToJson("maxdrawdown", result.max_drawdown, "lower");
    out["metrics"][7] = MetricToJson("avgvwap", result.avg_vwap, "neutral");
    out["metrics"][8] =
        MetricToJson("avgsolvetime", result.avg_solve_time_ms, "lower");
    return out;
  }

 private:
  cex::backtest::infra::PostgresReplaySessionRepository session_repo_;
  cex::backtest::infra::ClickHouseReplayStorage storage_;
};

int main() {
  const std::string brokers =
      cex::common::Env::get_string("KAFKA_BROKERS", "redpanda:9092");
  const std::string order_flow_addr =
      cex::common::Env::get_string("ORDER_FLOW_GRPC_ADDR", "order_flow:50051");

  const std::string market_data_addr =
      cex::common::Env::get_string("MARKET_DATA_GRPC_ADDR", "market_data:50052");

  const int port = cex::common::Env::get_int("GATEWAY_HTTP_PORT", 8080);

  const std::string pg_conn_str =
      cex::common::Env::get_string(
          "BACKTEST_POSTGRES_DSN",
          cex::common::Env::get_string("RBAC_URL", ""));
  auto db_conn = std::make_shared<pqxx::connection>(pg_conn_str);
  
  auto rbac_repo = std::make_shared<cex::backtest::infra::postgres::PostgresRbacRepository>(db_conn);
  auto rbac_engine = std::make_shared<cex::backtest::app::RbacEngine>(rbac_repo);
  
  auto auth_middleware =
      std::make_shared<cex::gateway::transport::AuthMiddleware>(rbac_engine);

  cex::gateway::infra::OrderFlowClient order_flow_client(order_flow_addr);
  cex::gateway::infra::MarketDataClient market_data_client(market_data_addr);
  
  auto summary_repo = std::make_shared<PostgresSummaryRepo>(pg_conn_str);
  auto sessions_repo = std::make_shared<PostgresSessionsRepo>(pg_conn_str);

  cex::backtest::infra::ClickHouseConfig ch_cfg;
  ch_cfg.url = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_URL", "http://clickhouse:8123");
  ch_cfg.database = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_DB", "default");
  ch_cfg.batchresults_table = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_BATCHRESULTS_TABLE", "batchresults");
  ch_cfg.fills_table = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_FILLS_TABLE", "fills");
  ch_cfg.replay_agentlogs_table = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_REPLAY_AGENTLOGS_TABLE", "replay_agentlogs");
  ch_cfg.user = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_USER", "default");
  ch_cfg.password = cex::common::Env::get_string(
      "BACKTEST_CLICKHOUSE_PASSWORD", "");
  ch_cfg.timeout_ms = cex::common::Env::get_int(
      "BACKTEST_CLICKHOUSE_TIMEOUT_MS", 3000);
  auto logs_repo = std::make_shared<ClickHouseLogsRepo>(ch_cfg);
  auto compare_repo = std::make_shared<ReplayCompareRepo>(pg_conn_str, ch_cfg);

  cex::gateway::infra::ReplayResultsHub replay_results_hub;
  cex::gateway::infra::ReplayResultsKafkaConsumer replay_results_consumer(
      &replay_results_hub, brokers,
      cex::common::Env::get_string("REPLAY_RESULTS_TOPIC", "replay.results"));
  replay_results_consumer.start();
  cex::common::KafkaProducer replay_commands_producer(
      {.brokers = brokers, .client_id = "gateway-replay-commands"});
  cex::gateway::infra::ReplayCommandsKafkaPublisher replay_commands(
      std::move(replay_commands_producer),
      cex::common::Env::get_string("REPLAY_COMMANDS_TOPIC", "replay.commands"));
  cex::gateway::transport::HttpGateway gw(std::move(order_flow_client),
                                          std::move(market_data_client),
                                          summary_repo,
                                          logs_repo,
                                          sessions_repo,
                                          compare_repo,
                                          rbac_engine, 
                                          &replay_commands,
                                          &replay_results_hub,
                                          auth_middleware);

  gw.run(static_cast<uint16_t>(port));
  replay_results_consumer.stop();
  return 0;
}
