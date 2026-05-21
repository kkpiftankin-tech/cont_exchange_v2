#include "infra/clickhouse_storage.hpp"

#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

namespace cex::backtest::infra {
namespace {

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string JsonEscape(const std::string& input) {
  std::ostringstream out;
  for (char c : input) {
    const unsigned char uc = static_cast<unsigned char>(c);
    switch (c) {
      case '\"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (uc < 0x20) {
          out << "\\u00";
          constexpr char kHex[] = "0123456789abcdef";
          out << kHex[(uc >> 4) & 0x0f] << kHex[uc & 0x0f];
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

double DecimalToDouble(const fob::common::v1::Decimal& decimal) {
  return static_cast<double>(decimal.units()) / std::pow(10.0, decimal.scale());
}

int64_t TimestampToUnixMs(const google::protobuf::Timestamp& ts) {
  return static_cast<int64_t>(ts.seconds()) * 1000LL +
         static_cast<int64_t>(ts.nanos()) / 1000000LL;
}

std::string SideToString(fob::common::v1::Side side) {
  switch (side) {
    case fob::common::v1::SIDE_BUY: return "buy";
    case fob::common::v1::SIDE_SELL: return "sell";
    default: return "unspecified";
  }
}

std::string DecimalMapToJsonString(
    const google::protobuf::Map<std::string, fob::common::v1::Decimal>& values) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& [name, decimal] : values) {
    if (!first) out << ",";
    first = false;
    out << "\"" << JsonEscape(name) << "\":\""
        << JsonEscape(cex::common::Decimal::from_proto(decimal).to_string()) << "\"";
  }
  out << "}";
  return out.str();
}

std::string LiquidityAuditToJsonString(
    const google::protobuf::RepeatedPtrField<fob::matching::v1::LiquidityProvenance>& values) {
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& value : values) {
    if (!first) out << ",";
    first = false;
    out << "{"
        << "\"liquidity_source\":\"" << JsonEscape(value.liquidity_source()) << "\","
        << "\"venue_id\":\"" << JsonEscape(value.venue_id()) << "\","
        << "\"snapshot_id\":\"" << JsonEscape(value.snapshot_id()) << "\","
        << "\"curve_id\":\"" << JsonEscape(value.curve_id()) << "\""
        << "}";
  }
  out << "]";
  return out.str();
}

int64_t EventTimeMs(const fob::matching::v1::BatchResult& evt) {
  if (evt.has_timestamp()) return TimestampToUnixMs(evt.timestamp());
  if (evt.has_meta() && evt.meta().has_ts_event()) return TimestampToUnixMs(evt.meta().ts_event());
  return 0;
}

std::string BuildBatchResultJsonRow(const fob::matching::v1::BatchResult& evt) {
  const std::string clear_prices_json = DecimalMapToJsonString(evt.clear_prices());
  const std::string executed_rates_json = DecimalMapToJsonString(evt.executed_rates());
  const std::string used_liquidity_json =
      evt.has_diagnostics() ? LiquidityAuditToJsonString(evt.diagnostics().used_liquidity()) : "[]";

  std::ostringstream row;
  row << "{";
  row << "\"batch_id\":\"" << JsonEscape(evt.batch_id()) << "\",";
  row << "\"event_time_ms\":" << EventTimeMs(evt) << ",";
  row << "\"source\":\"" << JsonEscape(evt.has_meta() ? evt.meta().source() : "") << "\",";
  row << "\"correlation_id\":\""
      << JsonEscape(evt.has_meta() ? evt.meta().correlation_id() : "") << "\",";
  row << "\"partition_key\":\""
      << JsonEscape(evt.has_meta() ? evt.meta().partition_key() : "") << "\",";
  row << "\"residual_norm\":"
      << (evt.has_diagnostics() ? evt.diagnostics().residual_norm() : 0.0) << ",";
  row << "\"solve_time_ms\":"
      << (evt.has_diagnostics() ? evt.diagnostics().solve_time_ms() : 0) << ",";
  row << "\"num_active_orders\":"
      << (evt.has_diagnostics() ? evt.diagnostics().num_active_orders() : 0) << ",";
  row << "\"config_version\":"
      << (evt.has_diagnostics() ? evt.diagnostics().config_version() : 0) << ",";
  row << "\"solver_diagnostics_json\":\""
      << JsonEscape(evt.has_diagnostics() ? evt.diagnostics().solver_diagnostics_json() : "")
      << "\",";
  row << "\"clear_prices_json\":\"" << JsonEscape(clear_prices_json) << "\",";
  row << "\"executed_rates_json\":\"" << JsonEscape(executed_rates_json) << "\",";
  row << "\"used_liquidity_json\":\"" << JsonEscape(used_liquidity_json) << "\",";
  row << "\"fills_count\":" << evt.fills_size();
  row << "}";
  return row.str();
}

std::string BuildFillsJsonRows(const fob::matching::v1::BatchResult& evt) {
  std::ostringstream rows;
  const int64_t event_time_ms = EventTimeMs(evt);
  for (const auto& fill : evt.fills()) {
    const double fee_amount =
        (fill.has_fee() && fill.fee().has_cost() && fill.fee().cost().has_amount())
            ? DecimalToDouble(fill.fee().cost().amount())
            : 0.0;
    const std::string fee_currency =
        (fill.has_fee() && fill.fee().has_cost()) ? fill.fee().cost().currency() : "";

    rows << "{";
    rows << "\"batch_id\":\"" << JsonEscape(evt.batch_id()) << "\",";
    rows << "\"event_time_ms\":" << event_time_ms << ",";
    rows << "\"order_id\":\"" << JsonEscape(fill.order_id()) << "\",";
    rows << "\"user_id\":\"" << JsonEscape(fill.user_id()) << "\",";
    rows << "\"symbol\":\"" << JsonEscape(fill.instrument().symbol()) << "\",";
    rows << "\"base\":\"" << JsonEscape(fill.instrument().base()) << "\",";
    rows << "\"quote\":\"" << JsonEscape(fill.instrument().quote()) << "\",";
    rows << "\"side\":\"" << JsonEscape(SideToString(fill.side())) << "\",";
    rows << "\"executed_qty\":" << DecimalToDouble(fill.executed_qty()) << ",";
    rows << "\"price\":" << DecimalToDouble(fill.price()) << ",";
    rows << "\"executed_notional\":" << DecimalToDouble(fill.executed_notional()) << ",";
    rows << "\"fee_amount\":" << fee_amount << ",";
    rows << "\"fee_currency\":\"" << JsonEscape(fee_currency) << "\",";
    rows << "\"liquidity_source\":\"" << JsonEscape(
        fill.liquidity_source().empty()
            ? (fill.has_provenance() && !fill.provenance().liquidity_source().empty()
                   ? fill.provenance().liquidity_source()
                   : "internal")
            : fill.liquidity_source()) << "\",";
    rows << "\"venue_id\":\"" << JsonEscape(
        fill.has_provenance() ? fill.provenance().venue_id() : "") << "\",";
    rows << "\"snapshot_id\":\"" << JsonEscape(
        fill.has_provenance() ? fill.provenance().snapshot_id() : "") << "\",";
    rows << "\"curve_id\":\"" << JsonEscape(
        fill.has_provenance() ? fill.provenance().curve_id() : "") << "\"";
    rows << "}\n";
  }
  return rows.str();
}

std::string InsertTableName(const std::string& query) {
  constexpr const char* kPrefix = "INSERT INTO ";
  if (query.rfind(kPrefix, 0) != 0) return {};
  const std::size_t begin = std::char_traits<char>::length(kPrefix);
  const std::size_t end = query.find(' ', begin);
  return query.substr(begin, end == std::string::npos ? query.size() - begin : end - begin);
}

const char* AgentLogRiskStatus(const app::AgentLogEntry& entry) {
  if (!entry.risk_status.empty()) return entry.risk_status.c_str();
  return entry.solver_error_flag ? "hard" : "ok";
}

app::FailureComponent ParseFailureComponent(const std::string& raw) {
  if (raw == "solver") return app::FailureComponent::kSolver;
  if (raw == "marketdata") return app::FailureComponent::kMarketData;
  if (raw == "risk") return app::FailureComponent::kRisk;
  if (raw == "history") return app::FailureComponent::kHistory;
  if (raw == "ledger") return app::FailureComponent::kLedger;
  if (raw == "config") return app::FailureComponent::kConfig;
  if (raw == "shadow") return app::FailureComponent::kShadow;
  if (raw == "pipeline") return app::FailureComponent::kPipeline;
  return app::FailureComponent::kUnknown;
}

std::string NormaliseJsonObject(const std::string& payload) {
  return payload.empty() ? "{}" : payload;
}

std::string NormaliseJsonArray(const std::string& payload) {
  return payload.empty() ? "[]" : payload;
}

}  // namespace

std::string BuildAgentLogJsonRow(const app::AgentLogEntry& entry) {
  std::ostringstream row;
  row << "{";
  row << "\"log_id\":\"" << JsonEscape(entry.log_id) << "\",";
  row << "\"session_id\":\"" << JsonEscape(entry.session_id) << "\",";
  row << "\"original_batch_id\":\"" << JsonEscape(entry.original_batch_id) << "\",";
  row << "\"batch_seq\":" << entry.batch_seq << ",";
  row << "\"event_time_ms\":" << entry.event_time_ms << ",";
  row << "\"state_json\":\"" << JsonEscape(NormaliseJsonObject(entry.state_json)) << "\",";
  row << "\"action_json\":\"" << JsonEscape(NormaliseJsonObject(entry.action_json)) << "\",";
  row << "\"reward\":" << entry.reward << ",";
  row << "\"pnl\":" << entry.pnl << ",";
  row << "\"is_value\":" << entry.is_value << ",";
  row << "\"fill_rate\":" << entry.fill_rate << ",";
  row << "\"fills_applied\":" << entry.fills_applied << ",";
  row << "\"solve_time_ms\":" << entry.solve_time_ms << ",";
  row << "\"residual_norm\":" << entry.residual_norm << ",";
  row << "\"solver_error_flag\":" << (entry.solver_error_flag ? 1 : 0) << ",";
  row << "\"risk_status\":\"" << JsonEscape(AgentLogRiskStatus(entry)) << "\",";
  row << "\"error_code\":\"" << JsonEscape(entry.error_code) << "\",";
  row << "\"error_details\":\"" << JsonEscape(entry.error_details) << "\",";
  row << "\"failure_component\":\""
      << JsonEscape(app::FailureComponentName(entry.failure_component)) << "\",";
  row << "\"fills_json\":\"" << JsonEscape(NormaliseJsonArray(entry.fills_json)) << "\",";
  row << "\"batch_result_json\":\""
      << JsonEscape(NormaliseJsonObject(entry.batch_result_json)) << "\",";
  row << "\"metrics_json\":\"" << JsonEscape(NormaliseJsonObject(entry.metrics_json)) << "\",";
  row << "\"session_config_snapshot_version\":"
      << entry.session_config_snapshot_version;
  row << "}";
  return row.str();
}

ClickHouseReplayStorage::ClickHouseReplayStorage(ClickHouseConfig cfg,
                                                 app::ReplayRuntimeMetrics* metrics)
    : cfg_(std::move(cfg)), metrics_(metrics) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool ClickHouseReplayStorage::EnsureSchema() {
  const int replay_agentlogs_retention_days =
      std::max(1, cfg_.replay_agentlogs_retention_days);
  const std::string create_db = "CREATE DATABASE IF NOT EXISTS " + cfg_.database;
  const std::string create_batchresults =
      "CREATE TABLE IF NOT EXISTS " + BatchResultsTableName() + " ("
      "batch_id String,"
      "event_time_ms Int64,"
      "source String,"
      "correlation_id String,"
      "partition_key String,"
      "residual_norm Float64,"
      "solve_time_ms UInt32,"
      "num_active_orders UInt32,"
      "config_version UInt32,"
      "solver_diagnostics_json String,"
      "clear_prices_json String,"
      "executed_rates_json String,"
      "used_liquidity_json String,"
      "fills_count UInt32,"
      "ingested_at DateTime DEFAULT now(),"
      "PROJECTION prj_batchresults_by_batch_id "
      "(SELECT batch_id, event_time_ms, source, correlation_id, partition_key, "
      "residual_norm, solve_time_ms, num_active_orders, config_version, "
      "solver_diagnostics_json, clear_prices_json, executed_rates_json, "
      "used_liquidity_json, fills_count, ingested_at "
      "ORDER BY (batch_id, event_time_ms))"
      ") ENGINE = MergeTree "
      "ORDER BY (event_time_ms, batch_id)";

  const std::string create_fills =
      "CREATE TABLE IF NOT EXISTS " + FillsTableName() + " ("
      "batch_id String,"
      "event_time_ms Int64,"
      "order_id String,"
      "user_id String,"
      "symbol String,"
      "base String,"
      "quote String,"
      "side LowCardinality(String),"
      "executed_qty Float64,"
      "price Float64,"
      "executed_notional Float64,"
      "fee_amount Float64,"
      "fee_currency String,"
      "liquidity_source String,"
      "venue_id String,"
      "snapshot_id String,"
      "curve_id String,"
      "ingested_at DateTime DEFAULT now(),"
      "PROJECTION prj_fills_by_batch_id "
      "(SELECT batch_id, event_time_ms, order_id, user_id, symbol, base, quote, "
      "side, executed_qty, price, executed_notional, fee_amount, fee_currency, "
      "liquidity_source, venue_id, snapshot_id, curve_id, ingested_at "
      "ORDER BY (batch_id, event_time_ms, order_id))"
      ") ENGINE = MergeTree "
      "ORDER BY (event_time_ms, batch_id, order_id)";

  const std::string create_fill_metrics =
      "CREATE TABLE IF NOT EXISTS " + FillMetricsTableName() + " ("
      "batch_id String,"
      "event_time_ms Int64,"
      "order_id String,"
      "user_id String,"
      "symbol String,"
      "side LowCardinality(String),"
      "executed_qty Float64,"
      "price Float64,"
      "executed_notional Float64,"
      "fee_amount Float64,"
      "fee_currency String,"
      "liquidity_source String,"
      "venue_id String,"
      "snapshot_id String,"
      "curve_id String,"
      "clear_price Float64,"
      "is_bps Float64,"
      "fill_pnl Float64,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (event_time_ms, batch_id, order_id)";

  const std::string create_batch_metrics =
      "CREATE TABLE IF NOT EXISTS " + BatchMetricsTableName() + " ("
      "batch_id String,"
      "event_time_ms Int64,"
      "num_fills UInt32,"
      "num_buy_fills UInt32,"
      "num_sell_fills UInt32,"
      "total_notional Float64,"
      "buy_notional Float64,"
      "sell_notional Float64,"
      "total_fees Float64,"
      "net_pnl Float64,"
      "vwap_json String,"
      "solve_time_ms Float64,"
      "residual_norm Float64,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (event_time_ms, batch_id)";

  // F15-BACKTEST-6. ReplacingMergeTree on (session_id, original_batch_id)
  // gives idempotent insert: re-running the same step at retry replaces the
  // previous row at merge (or via FINAL on read). ingested_at is the version
  // column so the latest write wins.
  const std::string create_replay_agentlogs =
      "CREATE TABLE IF NOT EXISTS " + ReplayAgentLogsTableName() + " ("
      "session_id String,"
      "log_id String,"
      "original_batch_id String,"
      "batch_seq UInt32,"
      "event_time_ms Int64,"
      "state_json String,"
      "action_json String,"
      "reward Float64,"
      "pnl Float64,"
      "is_value Float64,"
      "fill_rate Float64,"
      "fills_applied UInt32,"
      "solve_time_ms UInt32,"
      "residual_norm Float64,"
      "solver_error_flag UInt8,"
      "risk_status LowCardinality(String),"
      "error_code String,"
      "error_details String,"
      "failure_component LowCardinality(String),"
      "fills_json String,"
      "batch_result_json String,"
      "metrics_json String,"
      "session_config_snapshot_version UInt32 DEFAULT 1,"
      "ingested_at DateTime64(3) DEFAULT now64(3),"
      "INDEX idx_solver_error_flag solver_error_flag TYPE set(2) GRANULARITY 1,"
      "INDEX idx_fill_rate fill_rate TYPE minmax GRANULARITY 1,"
      "INDEX idx_pnl pnl TYPE minmax GRANULARITY 1"
      ") ENGINE = ReplacingMergeTree(ingested_at) "
      "ORDER BY (session_id, original_batch_id) "
      "TTL toDateTime(event_time_ms / 1000) + INTERVAL " +
      std::to_string(replay_agentlogs_retention_days) + " DAY DELETE";
  const std::string apply_replay_agentlogs_ttl =
      "ALTER TABLE " + ReplayAgentLogsTableName() +
      " MODIFY TTL toDateTime(event_time_ms / 1000) + INTERVAL " +
      std::to_string(replay_agentlogs_retention_days) + " DAY DELETE";
  const std::string alter_replay_agentlogs_log_id =
      "ALTER TABLE " + ReplayAgentLogsTableName() +
      " ADD COLUMN IF NOT EXISTS log_id String AFTER session_id";
  const std::string alter_replay_agentlogs_snapshot_version =
      "ALTER TABLE " + ReplayAgentLogsTableName() +
      " ADD COLUMN IF NOT EXISTS session_config_snapshot_version UInt32 DEFAULT 1 "
      "AFTER metrics_json";

  const std::string create_venue_snapshots =
      "CREATE TABLE IF NOT EXISTS " + VenueSnapshotsTableName() + " ("
      "venue_id String,"
      "symbol String,"
      "event_time_ms Int64,"
      "best_bid Float64,"
      "best_ask Float64,"
      "mid_price Float64,"
      "spread Float64,"
      "bid_depth_json String,"
      "ask_depth_json String,"
      "maker_fee Float64,"
      "taker_fee Float64,"
      "tick_size Float64,"
      "lot_size Float64,"
      "status LowCardinality(String),"
      "volume_24h Float64,"
      "source String,"
      "correlation_id String,"
      "ingested_at DateTime DEFAULT now(),"
      "PROJECTION prj_marketdata_by_event_time "
      "(SELECT venue_id, symbol, event_time_ms, best_bid, best_ask, mid_price, "
      "spread, bid_depth_json, ask_depth_json, maker_fee, taker_fee, tick_size, "
      "lot_size, status, volume_24h, source, correlation_id, ingested_at "
      "ORDER BY (event_time_ms, venue_id, symbol))"
      ") ENGINE = MergeTree "
      "ORDER BY (venue_id, symbol, event_time_ms)";

  const std::string create_venue_curves =
      "CREATE TABLE IF NOT EXISTS " + VenueCurvesTableName() + " ("
      "venue_id String,"
      "symbol String,"
      "event_time_ms Int64,"
      "snapshot_id String,"
      "curve_id String,"
      "level LowCardinality(String),"
      "bid_q_grid String,"
      "bid_p_of_q String,"
      "bid_s_of_q String,"
      "ask_q_grid String,"
      "ask_p_of_q String,"
      "ask_s_of_q String,"
      "epsilon1 Float64,"
      "epsilon2 Float64,"
      "epsilon3 Float64,"
      "confidence Float64,"
      "mid_price Float64,"
      "tau_ms Float64,"
      "source String,"
      "correlation_id String,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (venue_id, symbol, event_time_ms)";

  const std::string create_quality_reports =
      "CREATE TABLE IF NOT EXISTS " + QualityReportsTableName() + " ("
      "venue_id String,"
      "symbol String,"
      "from_ms Int64,"
      "to_ms Int64,"
      "total_curves UInt32,"
      "total_snapshots UInt32,"
      "l1_count UInt32,"
      "l2_count UInt32,"
      "l3_count UInt32,"
      "epsilon1_mean Float64,"
      "epsilon1_median Float64,"
      "epsilon1_p95 Float64,"
      "epsilon1_p99 Float64,"
      "epsilon2_mean Float64,"
      "epsilon2_median Float64,"
      "epsilon2_p95 Float64,"
      "epsilon2_p99 Float64,"
      "epsilon3_mean Float64,"
      "epsilon3_median Float64,"
      "epsilon3_p95 Float64,"
      "epsilon3_p99 Float64,"
      "confidence_mean Float64,"
      "confidence_median Float64,"
      "confidence_p95 Float64,"
      "confidence_p99 Float64,"
      "stale_rate Float64,"
      "uptime Float64,"
      "connected_snapshots UInt32,"
      "ingested_at DateTime DEFAULT now()"
      ") ENGINE = MergeTree "
      "ORDER BY (venue_id, symbol, from_ms)";

  return ExecQuery(create_db) && ExecQuery(create_batchresults) &&
         ExecQuery(create_fills) && ExecQuery(create_fill_metrics) &&
         ExecQuery(create_batch_metrics) && ExecQuery(create_replay_agentlogs) &&
         ExecQuery(alter_replay_agentlogs_log_id) &&
         ExecQuery(alter_replay_agentlogs_snapshot_version) &&
         ExecQuery(apply_replay_agentlogs_ttl) && ExecQuery(create_venue_snapshots) &&
         ExecQuery(create_venue_curves) && ExecQuery(create_quality_reports);
}

bool ClickHouseReplayStorage::SaveBatchResult(const fob::matching::v1::BatchResult& evt) {
  const std::string query =
      "INSERT INTO " + BatchResultsTableName() + " FORMAT JSONEachRow";
  const std::string row = BuildBatchResultJsonRow(evt) + "\n";
  return ExecQuery(query, row);
}

bool ClickHouseReplayStorage::SaveFills(const fob::matching::v1::BatchResult& evt) {
  if (evt.fills().empty()) return true;
  const std::string query =
      "INSERT INTO " + FillsTableName() + " FORMAT JSONEachRow";
  return ExecQuery(query, BuildFillsJsonRows(evt));
}

void ClickHouseReplayStorage::WriteAgentLog(const app::AgentLogEntry& entry) {
  if (entry.session_id.empty() || entry.original_batch_id.empty()) {
    cex::common::log_json("ERROR",
                          "WriteAgentLog rejected: session_id/original_batch_id empty");
    throw std::invalid_argument(
        "AgentLog write failure: session_id/original_batch_id empty");
  }
  const std::string query =
      "INSERT INTO " + ReplayAgentLogsTableName() + " FORMAT JSONEachRow";
  const std::string body = BuildAgentLogJsonRow(entry) + "\n";
  if (!ExecQuery(query, body)) {
    throw std::runtime_error("AgentLog write failure: ClickHouse insert into " +
                             ReplayAgentLogsTableName() + " failed");
  }
}

bool ClickHouseReplayStorage::ExecQuery(const std::string& query, const std::string& body) {
  const bool is_insert = query.rfind("INSERT INTO ", 0) == 0;
  const std::string insert_table = is_insert ? InsertTableName(query) : std::string{};
  const auto started = std::chrono::steady_clock::now();
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    cex::common::log_json("ERROR", "curl_easy_init failed");
    if (is_insert && metrics_ != nullptr) {
      metrics_->ObserveClickHouseInsert(insert_table, 0.0, false);
    }
    return false;
  }

  char* encoded = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
  if (encoded == nullptr) {
    cex::common::log_json("ERROR", "Failed to URL-encode ClickHouse query");
    curl_easy_cleanup(curl);
    if (is_insert && metrics_ != nullptr) {
      metrics_->ObserveClickHouseInsert(insert_table, 0.0, false);
    }
    return false;
  }

  std::string url = cfg_.url + "/?query=" + encoded;
  curl_free(encoded);

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg_.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  if (!cfg_.user.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  const double latency_ms = static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count());

  if (rc != CURLE_OK) {
    cex::common::log_json("ERROR", "ClickHouse request failed",
                          {{"err", curl_easy_strerror(rc)}});
    if (is_insert && metrics_ != nullptr) {
      metrics_->ObserveClickHouseInsert(insert_table, latency_ms, false);
    }
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    cex::common::log_json(
        "ERROR", "ClickHouse returned non-success status",
        {{"http_code", std::to_string(http_code)},
         {"response", response.empty() ? "<empty>" : response}});
    if (is_insert && metrics_ != nullptr) {
      metrics_->ObserveClickHouseInsert(insert_table, latency_ms, false);
    }
    return false;
  }
  if (is_insert && metrics_ != nullptr) {
    metrics_->ObserveClickHouseInsert(insert_table, latency_ms, true);
  }
  return true;
}

bool ClickHouseReplayStorage::SaveFillMetrics(
    const std::string& batch_id,
    int64_t event_time_ms,
    const std::vector<app::FillMetrics>& metrics) {
  if (metrics.empty()) return true;
  std::ostringstream rows;
  for (const auto& m : metrics) {
    rows << "{";
    rows << "\"batch_id\":\"" << JsonEscape(batch_id) << "\",";
    rows << "\"event_time_ms\":" << event_time_ms << ",";
    rows << "\"order_id\":\"" << JsonEscape(m.order_id) << "\",";
    rows << "\"user_id\":\"" << JsonEscape(m.user_id) << "\",";
    rows << "\"symbol\":\"" << JsonEscape(m.symbol) << "\",";
    rows << "\"side\":\"" << JsonEscape(m.side) << "\",";
    rows << "\"executed_qty\":" << m.executed_qty << ",";
    rows << "\"price\":" << m.price << ",";
    rows << "\"executed_notional\":" << m.executed_notional << ",";
    rows << "\"fee_amount\":" << m.fee_amount << ",";
    rows << "\"fee_currency\":\"" << JsonEscape(m.fee_currency) << "\",";
    rows << "\"liquidity_source\":\"" << JsonEscape(m.liquidity_source) << "\",";
    rows << "\"venue_id\":\"" << JsonEscape(m.venue_id) << "\",";
    rows << "\"snapshot_id\":\"" << JsonEscape(m.snapshot_id) << "\",";
    rows << "\"curve_id\":\"" << JsonEscape(m.curve_id) << "\",";
    rows << "\"clear_price\":" << m.clear_price << ",";
    rows << "\"is_bps\":" << m.is_bps << ",";
    rows << "\"fill_pnl\":" << m.fill_pnl;
    rows << "}\n";
  }
  const std::string query =
      "INSERT INTO " + FillMetricsTableName() + " FORMAT JSONEachRow";
  return ExecQuery(query, rows.str());
}

bool ClickHouseReplayStorage::SaveBatchMetrics(const app::BatchMetrics& bm) {
  std::ostringstream row;
  row << "{";
  row << "\"batch_id\":\"" << JsonEscape(bm.batch_id) << "\",";
  row << "\"event_time_ms\":" << bm.event_time_ms << ",";
  row << "\"num_fills\":" << bm.num_fills << ",";
  row << "\"num_buy_fills\":" << bm.num_buy_fills << ",";
  row << "\"num_sell_fills\":" << bm.num_sell_fills << ",";
  row << "\"total_notional\":" << bm.total_notional << ",";
  row << "\"buy_notional\":" << bm.buy_notional << ",";
  row << "\"sell_notional\":" << bm.sell_notional << ",";
  row << "\"total_fees\":" << bm.total_fees << ",";
  row << "\"net_pnl\":" << bm.net_pnl << ",";
  row << "\"vwap_json\":\"" << JsonEscape(bm.vwap_json) << "\",";
  row << "\"solve_time_ms\":" << bm.solve_time_ms << ",";
  row << "\"residual_norm\":" << bm.residual_norm;
  row << "}\n";
  const std::string query =
      "INSERT INTO " + BatchMetricsTableName() + " FORMAT JSONEachRow";
  return ExecQuery(query, row.str());
}

std::string ClickHouseReplayStorage::BatchResultsTableName() const {
  return cfg_.database + "." + cfg_.batchresults_table;
}

std::string ClickHouseReplayStorage::FillsTableName() const {
  return cfg_.database + "." + cfg_.fills_table;
}

std::string ClickHouseReplayStorage::FillMetricsTableName() const {
  return cfg_.database + "." + cfg_.fill_metrics_table;
}

std::string ClickHouseReplayStorage::BatchMetricsTableName() const {
  return cfg_.database + "." + cfg_.batch_metrics_table;
}

std::string ClickHouseReplayStorage::ReplayAgentLogsTableName() const {
  return cfg_.database + "." + cfg_.replay_agentlogs_table;
}

std::string ClickHouseReplayStorage::VenueSnapshotsTableName() const {
  return cfg_.database + "." + cfg_.venue_snapshots_table;
}

std::string ClickHouseReplayStorage::VenueCurvesTableName() const {
  return cfg_.database + "." + cfg_.venue_curves_table;
}

std::string ClickHouseReplayStorage::RiskEventsTableName() const {
  return cfg_.database + "." + cfg_.risk_events_table;
}

std::string ClickHouseReplayStorage::QualityReportsTableName() const {
  return cfg_.database + "." + cfg_.quality_reports_table;
}

bool ClickHouseReplayStorage::SaveQualityReport(
    const app::VenueQualityReport& r) {
  std::ostringstream row;
  row << "{";
  row << "\"venue_id\":\"" << JsonEscape(r.venue_id) << "\",";
  row << "\"symbol\":\"" << JsonEscape(r.symbol) << "\",";
  row << "\"from_ms\":" << r.from_ms << ",";
  row << "\"to_ms\":" << r.to_ms << ",";
  row << "\"total_curves\":" << r.total_curves << ",";
  row << "\"total_snapshots\":" << r.total_snapshots << ",";
  row << "\"l1_count\":" << r.l1_count << ",";
  row << "\"l2_count\":" << r.l2_count << ",";
  row << "\"l3_count\":" << r.l3_count << ",";
  row << "\"epsilon1_mean\":" << r.epsilon1.mean << ",";
  row << "\"epsilon1_median\":" << r.epsilon1.median << ",";
  row << "\"epsilon1_p95\":" << r.epsilon1.p95 << ",";
  row << "\"epsilon1_p99\":" << r.epsilon1.p99 << ",";
  row << "\"epsilon2_mean\":" << r.epsilon2.mean << ",";
  row << "\"epsilon2_median\":" << r.epsilon2.median << ",";
  row << "\"epsilon2_p95\":" << r.epsilon2.p95 << ",";
  row << "\"epsilon2_p99\":" << r.epsilon2.p99 << ",";
  row << "\"epsilon3_mean\":" << r.epsilon3.mean << ",";
  row << "\"epsilon3_median\":" << r.epsilon3.median << ",";
  row << "\"epsilon3_p95\":" << r.epsilon3.p95 << ",";
  row << "\"epsilon3_p99\":" << r.epsilon3.p99 << ",";
  row << "\"confidence_mean\":" << r.confidence.mean << ",";
  row << "\"confidence_median\":" << r.confidence.median << ",";
  row << "\"confidence_p95\":" << r.confidence.p95 << ",";
  row << "\"confidence_p99\":" << r.confidence.p99 << ",";
  row << "\"stale_rate\":" << r.stale_rate << ",";
  row << "\"uptime\":" << r.uptime << ",";
  row << "\"connected_snapshots\":" << r.connected_snapshots;
  row << "}\n";

  const std::string query =
      "INSERT INTO " + QualityReportsTableName() + " FORMAT JSONEachRow";
  return ExecQuery(query, row.str());
}

namespace {

std::string DoubleArrayToJson(const google::protobuf::RepeatedField<double>& arr) {
  std::ostringstream out;
  out << "[";
  for (int i = 0; i < arr.size(); ++i) {
    if (i > 0) out << ",";
    out << arr.Get(i);
  }
  out << "]";
  return out.str();
}

std::string DecimalArrayToJson(
    const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& prices,
    const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& quantities) {
  std::ostringstream out;
  out << "[";
  const int n = std::min(prices.size(), quantities.size());
  for (int i = 0; i < n; ++i) {
    if (i > 0) out << ",";
    out << "[" << DecimalToDouble(prices.Get(i)) << ","
        << DecimalToDouble(quantities.Get(i)) << "]";
  }
  out << "]";
  return out.str();
}

}  // namespace

bool ClickHouseReplayStorage::SaveVenueSnapshot(
    const fob::venue::v1::VenueSnapshot& snapshot) {
  const int64_t event_time_ms = snapshot.has_timestamp()
      ? TimestampToUnixMs(snapshot.timestamp())
      : (snapshot.has_meta() && snapshot.meta().has_ts_event()
             ? TimestampToUnixMs(snapshot.meta().ts_event()) : 0);

  const std::string bid_depth = DecimalArrayToJson(
      snapshot.bid_prices(), snapshot.bid_quantities());
  const std::string ask_depth = DecimalArrayToJson(
      snapshot.ask_prices(), snapshot.ask_quantities());

  std::ostringstream row;
  row << "{";
  row << "\"venue_id\":\"" << JsonEscape(snapshot.venue_id()) << "\",";
  row << "\"symbol\":\"" << JsonEscape(snapshot.instrument().symbol()) << "\",";
  row << "\"event_time_ms\":" << event_time_ms << ",";
  row << "\"best_bid\":" << DecimalToDouble(snapshot.best_bid()) << ",";
  row << "\"best_ask\":" << DecimalToDouble(snapshot.best_ask()) << ",";
  row << "\"mid_price\":" << DecimalToDouble(snapshot.mid_price()) << ",";
  row << "\"spread\":" << DecimalToDouble(snapshot.spread()) << ",";
  row << "\"bid_depth_json\":\"" << JsonEscape(bid_depth) << "\",";
  row << "\"ask_depth_json\":\"" << JsonEscape(ask_depth) << "\",";
  row << "\"maker_fee\":" << DecimalToDouble(snapshot.maker_fee()) << ",";
  row << "\"taker_fee\":" << DecimalToDouble(snapshot.taker_fee()) << ",";
  row << "\"tick_size\":" << DecimalToDouble(snapshot.tick_size()) << ",";
  row << "\"lot_size\":" << DecimalToDouble(snapshot.lot_size()) << ",";
  row << "\"status\":\"" << JsonEscape(snapshot.status()) << "\",";
  row << "\"volume_24h\":" << DecimalToDouble(snapshot.volume_24h()) << ",";
  row << "\"source\":\"" << JsonEscape(snapshot.has_meta() ? snapshot.meta().source() : "") << "\",";
  row << "\"correlation_id\":\"" << JsonEscape(
      snapshot.has_meta() ? snapshot.meta().correlation_id() : "") << "\"";
  row << "}\n";

  const std::string query =
      "INSERT INTO " + VenueSnapshotsTableName() + " FORMAT JSONEachRow";
  return ExecQuery(query, row.str());
}

bool ClickHouseReplayStorage::SaveVenueLiquidityCurve(
    const fob::venue::v1::VenueLiquidityCurve& curve) {
  const int64_t event_time_ms = curve.has_timestamp()
      ? TimestampToUnixMs(curve.timestamp())
      : (curve.has_meta() && curve.meta().has_ts_event()
             ? TimestampToUnixMs(curve.meta().ts_event()) : 0);

  std::ostringstream row;
  row << "{";
  row << "\"venue_id\":\"" << JsonEscape(curve.venue_id()) << "\",";
  row << "\"symbol\":\"" << JsonEscape(curve.instrument().symbol()) << "\",";
  row << "\"event_time_ms\":" << event_time_ms << ",";
  row << "\"snapshot_id\":\"" << JsonEscape(curve.snapshot_id()) << "\",";
  row << "\"curve_id\":\"" << JsonEscape(curve.curve_id()) << "\",";
  row << "\"level\":\"" << JsonEscape(curve.level()) << "\",";
  row << "\"bid_q_grid\":\"" << JsonEscape(
      curve.has_bid_curve() ? DoubleArrayToJson(curve.bid_curve().q_grid()) : "[]") << "\",";
  row << "\"bid_p_of_q\":\"" << JsonEscape(
      curve.has_bid_curve() ? DoubleArrayToJson(curve.bid_curve().p_of_q()) : "[]") << "\",";
  row << "\"bid_s_of_q\":\"" << JsonEscape(
      curve.has_bid_curve() ? DoubleArrayToJson(curve.bid_curve().s_of_q()) : "[]") << "\",";
  row << "\"ask_q_grid\":\"" << JsonEscape(
      curve.has_ask_curve() ? DoubleArrayToJson(curve.ask_curve().q_grid()) : "[]") << "\",";
  row << "\"ask_p_of_q\":\"" << JsonEscape(
      curve.has_ask_curve() ? DoubleArrayToJson(curve.ask_curve().p_of_q()) : "[]") << "\",";
  row << "\"ask_s_of_q\":\"" << JsonEscape(
      curve.has_ask_curve() ? DoubleArrayToJson(curve.ask_curve().s_of_q()) : "[]") << "\",";
  row << "\"epsilon1\":" << curve.epsilon1() << ",";
  row << "\"epsilon2\":" << curve.epsilon2() << ",";
  row << "\"epsilon3\":" << curve.epsilon3() << ",";
  row << "\"confidence\":" << curve.confidence() << ",";
  row << "\"mid_price\":" << DecimalToDouble(curve.mid_price()) << ",";
  row << "\"tau_ms\":" << curve.tau_ms() << ",";
  row << "\"source\":\"" << JsonEscape(curve.has_meta() ? curve.meta().source() : "") << "\",";
  row << "\"correlation_id\":\"" << JsonEscape(
      curve.has_meta() ? curve.meta().correlation_id() : "") << "\"";
  row << "}\n";

  const std::string query =
      "INSERT INTO " + VenueCurvesTableName() + " FORMAT JSONEachRow";
  return ExecQuery(query, row.str());
}

std::string ClickHouseReplayStorage::SelectQuery(const std::string& query) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    cex::common::log_json("ERROR", "curl_easy_init failed");
    return {};
  }

  char* encoded = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size()));
  if (encoded == nullptr) {
    cex::common::log_json("ERROR", "Failed to URL-encode ClickHouse query");
    curl_easy_cleanup(curl);
    return {};
  }

  std::string url = cfg_.url + "/?query=" + encoded;
  curl_free(encoded);

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg_.timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  if (!cfg_.user.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, cfg_.user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, cfg_.password.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || http_code < 200 || http_code >= 300) {
    cex::common::log_json("ERROR", "ClickHouse SELECT failed",
                          {{"err", rc != CURLE_OK ? curl_easy_strerror(rc) : ""},
                           {"http_code", std::to_string(http_code)}});
    return {};
  }
  return response;
}

namespace {

// Split a TSV line into fields.
std::vector<std::string> SplitTsv(const std::string& line) {
  std::vector<std::string> fields;
  std::istringstream ss(line);
  std::string field;
  while (std::getline(ss, field, '\t')) {
    fields.push_back(field);
  }
  return fields;
}

double SafeStod(const std::string& s) {
  try { return std::stod(s); } catch (...) { return 0.0; }
}

int64_t SafeStoll(const std::string& s) {
  try { return std::stoll(s); } catch (...) { return 0; }
}

}  // namespace

std::vector<app::SnapshotRow> ClickHouseReplayStorage::LoadSnapshots(
    const std::string& venue_id,
    const std::string& symbol,
    int64_t from_ms,
    int64_t to_ms) {
  std::ostringstream q;
  q << "SELECT venue_id, symbol, event_time_ms, best_bid, best_ask, "
       "mid_price, spread, bid_depth_json, ask_depth_json, tick_size, lot_size, status "
       "FROM " << VenueSnapshotsTableName()
    << " WHERE venue_id = '" << JsonEscape(venue_id) << "'"
    << " AND symbol = '" << JsonEscape(symbol) << "'"
    << " AND event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " ORDER BY event_time_ms FORMAT TabSeparated";

  const std::string body = SelectQuery(q.str());
  std::vector<app::SnapshotRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 12) continue;
    app::SnapshotRow row;
    row.venue_id = fields[0];
    row.symbol = fields[1];
    row.event_time_ms = SafeStoll(fields[2]);
    row.best_bid = SafeStod(fields[3]);
    row.best_ask = SafeStod(fields[4]);
    row.mid_price = SafeStod(fields[5]);
    row.spread = SafeStod(fields[6]);
    row.bid_depth_json = fields[7];
    row.ask_depth_json = fields[8];
    row.tick_size = SafeStod(fields[9]);
    row.lot_size = SafeStod(fields[10]);
    row.status = fields[11];
    rows.push_back(std::move(row));
  }
  return rows;
}

namespace {

// ClickHouse TabSeparated escapes tabs/newlines/backslashes inside string
// fields. Decode them back so embedded JSON values round-trip cleanly.
std::string DecodeTsvField(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      const char next = s[i + 1];
      switch (next) {
        case 'n': out.push_back('\n'); ++i; continue;
        case 't': out.push_back('\t'); ++i; continue;
        case 'r': out.push_back('\r'); ++i; continue;
        case '\\': out.push_back('\\'); ++i; continue;
        case '\'': out.push_back('\''); ++i; continue;
        case '0': out.push_back('\0'); ++i; continue;
        default: break;
      }
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::vector<app::HistoricalBatchResultRow> ClickHouseReplayStorage::LoadBatchResults(
    int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) {
  const std::string body = SelectQuery(
      historical_queries::BuildBatchResultsRangeQuery(
          BatchResultsTableName(), from_ms, to_ms, offset, limit));
  std::vector<app::HistoricalBatchResultRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 14) continue;
    app::HistoricalBatchResultRow row;
    row.batch_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.source = DecodeTsvField(fields[2]);
    row.correlation_id = DecodeTsvField(fields[3]);
    row.partition_key = DecodeTsvField(fields[4]);
    row.residual_norm = SafeStod(fields[5]);
    row.solve_time_ms = static_cast<uint32_t>(SafeStoll(fields[6]));
    row.num_active_orders = static_cast<uint32_t>(SafeStoll(fields[7]));
    row.config_version = static_cast<uint32_t>(SafeStoll(fields[8]));
    row.solver_diagnostics_json = DecodeTsvField(fields[9]);
    row.clear_prices_json = DecodeTsvField(fields[10]);
    row.executed_rates_json = DecodeTsvField(fields[11]);
    row.used_liquidity_json = DecodeTsvField(fields[12]);
    row.fills_count = static_cast<uint32_t>(SafeStoll(fields[13]));
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalBatchResultRow> ClickHouseReplayStorage::LoadBatchResultsById(
    const std::string& batch_id) {
  const std::string body = SelectQuery(
      historical_queries::BuildBatchResultsByIdQuery(BatchResultsTableName(), batch_id));
  std::vector<app::HistoricalBatchResultRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 14) continue;
    app::HistoricalBatchResultRow row;
    row.batch_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.source = DecodeTsvField(fields[2]);
    row.correlation_id = DecodeTsvField(fields[3]);
    row.partition_key = DecodeTsvField(fields[4]);
    row.residual_norm = SafeStod(fields[5]);
    row.solve_time_ms = static_cast<uint32_t>(SafeStoll(fields[6]));
    row.num_active_orders = static_cast<uint32_t>(SafeStoll(fields[7]));
    row.config_version = static_cast<uint32_t>(SafeStoll(fields[8]));
    row.solver_diagnostics_json = DecodeTsvField(fields[9]);
    row.clear_prices_json = DecodeTsvField(fields[10]);
    row.executed_rates_json = DecodeTsvField(fields[11]);
    row.used_liquidity_json = DecodeTsvField(fields[12]);
    row.fills_count = static_cast<uint32_t>(SafeStoll(fields[13]));
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalBatchResultRow> ClickHouseReplayStorage::LoadBatchResultsByIds(
    const std::vector<std::string>& batch_ids) {
  const std::string body = SelectQuery(
      historical_queries::BuildBatchResultsByIdsQuery(BatchResultsTableName(), batch_ids));
  std::vector<app::HistoricalBatchResultRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 14) continue;
    app::HistoricalBatchResultRow row;
    row.batch_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.source = DecodeTsvField(fields[2]);
    row.correlation_id = DecodeTsvField(fields[3]);
    row.partition_key = DecodeTsvField(fields[4]);
    row.residual_norm = SafeStod(fields[5]);
    row.solve_time_ms = static_cast<uint32_t>(SafeStoll(fields[6]));
    row.num_active_orders = static_cast<uint32_t>(SafeStoll(fields[7]));
    row.config_version = static_cast<uint32_t>(SafeStoll(fields[8]));
    row.solver_diagnostics_json = DecodeTsvField(fields[9]);
    row.clear_prices_json = DecodeTsvField(fields[10]);
    row.executed_rates_json = DecodeTsvField(fields[11]);
    row.used_liquidity_json = DecodeTsvField(fields[12]);
    row.fills_count = static_cast<uint32_t>(SafeStoll(fields[13]));
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalBatchResultRow> ClickHouseReplayStorage::ResumeBatchResultsFrom(
    int64_t from_ms,
    int64_t to_ms,
    const historical_queries::BatchResultCursor& cursor,
    int64_t limit) {
  const std::string body = SelectQuery(
      historical_queries::BuildBatchResultsResumeQuery(
          BatchResultsTableName(), from_ms, to_ms, cursor, limit));
  std::vector<app::HistoricalBatchResultRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 14) continue;
    app::HistoricalBatchResultRow row;
    row.batch_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.source = DecodeTsvField(fields[2]);
    row.correlation_id = DecodeTsvField(fields[3]);
    row.partition_key = DecodeTsvField(fields[4]);
    row.residual_norm = SafeStod(fields[5]);
    row.solve_time_ms = static_cast<uint32_t>(SafeStoll(fields[6]));
    row.num_active_orders = static_cast<uint32_t>(SafeStoll(fields[7]));
    row.config_version = static_cast<uint32_t>(SafeStoll(fields[8]));
    row.solver_diagnostics_json = DecodeTsvField(fields[9]);
    row.clear_prices_json = DecodeTsvField(fields[10]);
    row.executed_rates_json = DecodeTsvField(fields[11]);
    row.used_liquidity_json = DecodeTsvField(fields[12]);
    row.fills_count = static_cast<uint32_t>(SafeStoll(fields[13]));
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalFillRow> ClickHouseReplayStorage::LoadFills(
    int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) {
  const std::string body = SelectQuery(
      historical_queries::BuildFillsRangeQuery(
          FillsTableName(), from_ms, to_ms, offset, limit));
  std::vector<app::HistoricalFillRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 17) continue;
    app::HistoricalFillRow row;
    row.batch_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.order_id = DecodeTsvField(fields[2]);
    row.user_id = DecodeTsvField(fields[3]);
    row.symbol = DecodeTsvField(fields[4]);
    row.base = DecodeTsvField(fields[5]);
    row.quote = DecodeTsvField(fields[6]);
    row.side = DecodeTsvField(fields[7]);
    row.executed_qty = SafeStod(fields[8]);
    row.price = SafeStod(fields[9]);
    row.executed_notional = SafeStod(fields[10]);
    row.fee_amount = SafeStod(fields[11]);
    row.fee_currency = DecodeTsvField(fields[12]);
    row.liquidity_source = DecodeTsvField(fields[13]);
    row.venue_id = DecodeTsvField(fields[14]);
    row.snapshot_id = DecodeTsvField(fields[15]);
    row.curve_id = DecodeTsvField(fields[16]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalFillRow> ClickHouseReplayStorage::LoadFillsByBatchIds(
    const std::vector<std::string>& batch_ids) {
  const std::string body = SelectQuery(
      historical_queries::BuildFillsByBatchIdsQuery(FillsTableName(), batch_ids));
  std::vector<app::HistoricalFillRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 17) continue;
    app::HistoricalFillRow row;
    row.batch_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.order_id = DecodeTsvField(fields[2]);
    row.user_id = DecodeTsvField(fields[3]);
    row.symbol = DecodeTsvField(fields[4]);
    row.base = DecodeTsvField(fields[5]);
    row.quote = DecodeTsvField(fields[6]);
    row.side = DecodeTsvField(fields[7]);
    row.executed_qty = SafeStod(fields[8]);
    row.price = SafeStod(fields[9]);
    row.executed_notional = SafeStod(fields[10]);
    row.fee_amount = SafeStod(fields[11]);
    row.fee_currency = DecodeTsvField(fields[12]);
    row.liquidity_source = DecodeTsvField(fields[13]);
    row.venue_id = DecodeTsvField(fields[14]);
    row.snapshot_id = DecodeTsvField(fields[15]);
    row.curve_id = DecodeTsvField(fields[16]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalFillRow> ClickHouseReplayStorage::ResumeFillsFrom(
    int64_t from_ms,
    int64_t to_ms,
    const historical_queries::FillCursor& cursor,
    int64_t limit) {
  const std::string body = SelectQuery(
      historical_queries::BuildFillsResumeQuery(
          FillsTableName(), from_ms, to_ms, cursor, limit));
  std::vector<app::HistoricalFillRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 17) continue;
    app::HistoricalFillRow row;
    row.batch_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.order_id = DecodeTsvField(fields[2]);
    row.user_id = DecodeTsvField(fields[3]);
    row.symbol = DecodeTsvField(fields[4]);
    row.base = DecodeTsvField(fields[5]);
    row.quote = DecodeTsvField(fields[6]);
    row.side = DecodeTsvField(fields[7]);
    row.executed_qty = SafeStod(fields[8]);
    row.price = SafeStod(fields[9]);
    row.executed_notional = SafeStod(fields[10]);
    row.fee_amount = SafeStod(fields[11]);
    row.fee_currency = DecodeTsvField(fields[12]);
    row.liquidity_source = DecodeTsvField(fields[13]);
    row.venue_id = DecodeTsvField(fields[14]);
    row.snapshot_id = DecodeTsvField(fields[15]);
    row.curve_id = DecodeTsvField(fields[16]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalMarketdataSnapshotRow>
ClickHouseReplayStorage::LoadMarketdataSnapshots(
    int64_t from_ms, int64_t to_ms, int64_t offset, int64_t limit) {
  const std::string body = SelectQuery(
      historical_queries::BuildMarketdataSnapshotsRangeQuery(
          VenueSnapshotsTableName(), from_ms, to_ms, offset, limit));
  std::vector<app::HistoricalMarketdataSnapshotRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 12) continue;
    app::HistoricalMarketdataSnapshotRow row;
    row.venue_id = DecodeTsvField(fields[0]);
    row.symbol = DecodeTsvField(fields[1]);
    row.event_time_ms = SafeStoll(fields[2]);
    row.best_bid = SafeStod(fields[3]);
    row.best_ask = SafeStod(fields[4]);
    row.mid_price = SafeStod(fields[5]);
    row.spread = SafeStod(fields[6]);
    row.bid_depth_json = DecodeTsvField(fields[7]);
    row.ask_depth_json = DecodeTsvField(fields[8]);
    row.tick_size = SafeStod(fields[9]);
    row.lot_size = SafeStod(fields[10]);
    row.status = DecodeTsvField(fields[11]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalMarketdataSnapshotRow>
ClickHouseReplayStorage::LoadMarketdataSnapshotsByEventTimes(
    const std::vector<int64_t>& event_time_ms) {
  const std::string body = SelectQuery(
      historical_queries::BuildMarketdataSnapshotsByEventTimesQuery(
          VenueSnapshotsTableName(), event_time_ms));
  std::vector<app::HistoricalMarketdataSnapshotRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 12) continue;
    app::HistoricalMarketdataSnapshotRow row;
    row.venue_id = DecodeTsvField(fields[0]);
    row.symbol = DecodeTsvField(fields[1]);
    row.event_time_ms = SafeStoll(fields[2]);
    row.best_bid = SafeStod(fields[3]);
    row.best_ask = SafeStod(fields[4]);
    row.mid_price = SafeStod(fields[5]);
    row.spread = SafeStod(fields[6]);
    row.bid_depth_json = DecodeTsvField(fields[7]);
    row.ask_depth_json = DecodeTsvField(fields[8]);
    row.tick_size = SafeStod(fields[9]);
    row.lot_size = SafeStod(fields[10]);
    row.status = DecodeTsvField(fields[11]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalRiskEventRow> ClickHouseReplayStorage::LoadRiskEventsByBatchId(
    const std::string& batch_id) {
  const std::string body = SelectQuery(
      historical_queries::BuildRiskEventsByBatchIdQuery(
          RiskEventsTableName(), batch_id));
  std::vector<app::HistoricalRiskEventRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 7) continue;
    app::HistoricalRiskEventRow row;
    row.event_id = DecodeTsvField(fields[0]);
    row.event_time_ms = SafeStoll(fields[1]);
    row.entity_id = DecodeTsvField(fields[2]);
    row.event_type = DecodeTsvField(fields[3]);
    row.order_id = DecodeTsvField(fields[4]);
    row.details_json = DecodeTsvField(fields[5]);
    row.batch_id = DecodeTsvField(fields[6]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::ReplayAgentLogRef> ClickHouseReplayStorage::LoadAgentLogRefsBySessionId(
    const std::string& session_id) {
  const std::string body = SelectQuery(
      historical_queries::BuildReplayAgentLogRefsBySessionQuery(
          ReplayAgentLogsTableName(), session_id));
  std::vector<app::ReplayAgentLogRef> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 2) continue;
    app::ReplayAgentLogRef row;
    row.batch_seq = static_cast<uint32_t>(SafeStoll(fields[0]));
    row.original_batch_id = DecodeTsvField(fields[1]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::AgentLogEntry> ClickHouseReplayStorage::ReadLogsUpTo(
    const std::string& session_id,
    uint32_t up_to_batch_seq_exclusive) {
  const std::string body = SelectQuery(
      historical_queries::BuildReplayAgentLogsBySessionQuery(
          ReplayAgentLogsTableName(), session_id, up_to_batch_seq_exclusive));
  std::vector<app::AgentLogEntry> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 22) continue;
    app::AgentLogEntry row;
    row.session_id = session_id;
    row.log_id = DecodeTsvField(fields[0]);
    row.batch_seq = static_cast<uint32_t>(SafeStoll(fields[1]));
    row.original_batch_id = DecodeTsvField(fields[2]);
    row.event_time_ms = SafeStoll(fields[3]);
    row.state_json = DecodeTsvField(fields[4]);
    row.action_json = DecodeTsvField(fields[5]);
    row.reward = SafeStod(fields[6]);
    row.pnl = SafeStod(fields[7]);
    row.is_value = SafeStod(fields[8]);
    row.fill_rate = SafeStod(fields[9]);
    row.fills_applied = static_cast<uint32_t>(SafeStoll(fields[10]));
    row.solve_time_ms = static_cast<uint32_t>(SafeStoll(fields[11]));
    row.residual_norm = SafeStod(fields[12]);
    row.solver_error_flag = SafeStoll(fields[13]) != 0;
    row.risk_status = DecodeTsvField(fields[14]);
    row.error_code = DecodeTsvField(fields[15]);
    row.error_details = DecodeTsvField(fields[16]);
    row.failure_component = ParseFailureComponent(DecodeTsvField(fields[17]));
    row.fills_json = DecodeTsvField(fields[18]);
    row.batch_result_json = DecodeTsvField(fields[19]);
    row.metrics_json = DecodeTsvField(fields[20]);
    row.session_config_snapshot_version =
        static_cast<uint32_t>(SafeStoll(fields[21]));
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::HistoricalMarketdataSnapshotRow>
ClickHouseReplayStorage::ResumeMarketdataSnapshotsFrom(
    int64_t from_ms,
    int64_t to_ms,
    const historical_queries::SnapshotCursor& cursor,
    int64_t limit) {
  const std::string body = SelectQuery(
      historical_queries::BuildMarketdataSnapshotsResumeQuery(
          VenueSnapshotsTableName(), from_ms, to_ms, cursor, limit));
  std::vector<app::HistoricalMarketdataSnapshotRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 12) continue;
    app::HistoricalMarketdataSnapshotRow row;
    row.venue_id = DecodeTsvField(fields[0]);
    row.symbol = DecodeTsvField(fields[1]);
    row.event_time_ms = SafeStoll(fields[2]);
    row.best_bid = SafeStod(fields[3]);
    row.best_ask = SafeStod(fields[4]);
    row.mid_price = SafeStod(fields[5]);
    row.spread = SafeStod(fields[6]);
    row.bid_depth_json = DecodeTsvField(fields[7]);
    row.ask_depth_json = DecodeTsvField(fields[8]);
    row.tick_size = SafeStod(fields[9]);
    row.lot_size = SafeStod(fields[10]);
    row.status = DecodeTsvField(fields[11]);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<app::CurveRow> ClickHouseReplayStorage::LoadCurves(
    const std::string& venue_id,
    const std::string& symbol,
    int64_t from_ms,
    int64_t to_ms) {
  std::ostringstream q;
  q << "SELECT venue_id, symbol, event_time_ms, snapshot_id, curve_id, level, "
       "bid_q_grid, bid_p_of_q, bid_s_of_q, ask_q_grid, ask_p_of_q, ask_s_of_q, "
       "epsilon1, epsilon2, epsilon3, confidence, mid_price, tau_ms "
       "FROM " << VenueCurvesTableName()
    << " WHERE venue_id = '" << JsonEscape(venue_id) << "'"
    << " AND symbol = '" << JsonEscape(symbol) << "'"
    << " AND event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " ORDER BY event_time_ms FORMAT TabSeparated";

  const std::string body = SelectQuery(q.str());
  std::vector<app::CurveRow> rows;
  std::istringstream ss(body);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    auto fields = SplitTsv(line);
    if (fields.size() < 18) continue;
    app::CurveRow row;
    row.venue_id = fields[0];
    row.symbol = fields[1];
    row.event_time_ms = SafeStoll(fields[2]);
    row.snapshot_id = fields[3];
    row.curve_id = fields[4];
    row.level = fields[5];
    row.bid_q_grid = fields[6];
    row.bid_p_of_q = fields[7];
    row.bid_s_of_q = fields[8];
    row.ask_q_grid = fields[9];
    row.ask_p_of_q = fields[10];
    row.ask_s_of_q = fields[11];
    row.epsilon1 = SafeStod(fields[12]);
    row.epsilon2 = SafeStod(fields[13]);
    row.epsilon3 = SafeStod(fields[14]);
    row.confidence = SafeStod(fields[15]);
    row.mid_price = SafeStod(fields[16]);
    row.tau_ms = SafeStod(fields[17]);
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace cex::backtest::infra
