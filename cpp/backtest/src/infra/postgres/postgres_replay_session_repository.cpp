#include "infra/postgres/postgres_replay_session_repository.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace cex::backtest::infra {

namespace {

using Status = app::ReplaySessionStatus;

struct ParamCollector {
  std::vector<std::string> values;

  std::string Add(std::string value) {
    values.push_back(std::move(value));
    return "$" + std::to_string(values.size());
  }
};

std::string to_lower_ascii(std::string input) {
  std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return input;
}

std::string status_to_db(Status status) {
  switch (status) {
    case Status::kPending: return "pending";
    case Status::kRunning: return "running";
    case Status::kCompleted: return "completed";
    case Status::kFailed: return "failed";
    case Status::kCancelled: return "cancelled";
  }
  throw std::invalid_argument("Unsupported ReplaySessionStatus value");
}

Status status_from_db(const std::string& db_status) {
  const auto normalized = to_lower_ascii(db_status);
  if (normalized == "pending") {
    return Status::kPending;
  }
  if (normalized == "running") {
    return Status::kRunning;
  }
  if (normalized == "completed") {
    return Status::kCompleted;
  }
  if (normalized == "failed") {
    return Status::kFailed;
  }
  if (normalized == "cancelled") {
    return Status::kCancelled;
  }
  throw std::invalid_argument("Unsupported replay_sessions.status value: " + db_status);
}

double to_epoch_seconds(std::chrono::system_clock::time_point time_point) {
  return std::chrono::duration<double>(time_point.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_epoch_seconds(double epoch_seconds) {
  const auto duration =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::duration<double>(epoch_seconds));
  return std::chrono::system_clock::time_point{duration};
}

std::string epoch_seconds_string(std::chrono::system_clock::time_point time_point) {
  std::ostringstream oss;
  oss << std::setprecision(17) << to_epoch_seconds(time_point);
  return oss.str();
}

std::string join_clauses(const std::vector<std::string>& clauses,
                         const std::string& delimiter) {
  std::string joined;
  for (std::size_t i = 0; i < clauses.size(); ++i) {
    if (i > 0) {
      joined += delimiter;
    }
    joined += clauses[i];
  }
  return joined;
}

void require_non_empty(const std::string& value, const std::string& field_name) {
  if (value.empty()) {
    throw std::invalid_argument(field_name + " must not be empty");
  }
}

void validate_create_session(const app::ReplaySession& session) {
  require_non_empty(session.session_id, "ReplaySession.session_id");
  require_non_empty(session.user_id, "ReplaySession.user_id");
  require_non_empty(session.name, "ReplaySession.name");
  require_non_empty(session.solver_config_id, "ReplaySession.solver_config_id");
  require_non_empty(session.risk_limits_id, "ReplaySession.risk_limits_id");

  if (session.date_range_from > session.date_range_to) {
    throw std::invalid_argument("ReplaySession date_range_from must be <= date_range_to");
  }
  if (session.progress_batches < 0) {
    throw std::invalid_argument("ReplaySession.progress_batches must be >= 0");
  }
  if (session.total_batches.has_value() && *session.total_batches < session.progress_batches) {
    throw std::invalid_argument("ReplaySession.total_batches must be >= progress_batches");
  }
}

void validate_list_filter(const app::ReplaySessionListFilter& filter) {
  if (filter.user_id.has_value() && filter.user_id->empty()) {
    throw std::invalid_argument("ReplaySessionListFilter.user_id must not be empty");
  }
  if (filter.limit.has_value() && *filter.limit <= 0) {
    throw std::invalid_argument("ReplaySessionListFilter.limit must be positive");
  }
  if (filter.offset.has_value() && *filter.offset < 0) {
    throw std::invalid_argument("ReplaySessionListFilter.offset must be >= 0");
  }
  if (filter.created_from.has_value() && filter.created_to.has_value() &&
      *filter.created_from > *filter.created_to) {
    throw std::invalid_argument(
        "ReplaySessionListFilter.created_from must be <= created_to");
  }
  if (filter.replay_from.has_value() && filter.replay_to.has_value() &&
      *filter.replay_from > *filter.replay_to) {
    throw std::invalid_argument("ReplaySessionListFilter.replay_from must be <= replay_to");
  }
}

bool has_patch_fields(const app::ReplaySessionStatePatch& patch) {
  return patch.status.has_value() || patch.total_batches.has_value() ||
         patch.progress_batches.has_value() || patch.started_at.has_value() ||
         patch.completed_at.has_value() || patch.error_details.has_value();
}

void validate_state_patch(const app::ReplaySessionStatePatch& patch) {
  if (!has_patch_fields(patch)) {
    throw std::invalid_argument("ReplaySessionStatePatch must not be empty");
  }
  if (patch.progress_batches.has_value() && *patch.progress_batches < 0) {
    throw std::invalid_argument("ReplaySessionStatePatch.progress_batches must be >= 0");
  }
  if (patch.total_batches.has_value() && *patch.total_batches < 0) {
    throw std::invalid_argument("ReplaySessionStatePatch.total_batches must be >= 0");
  }
  if (patch.total_batches.has_value() && patch.progress_batches.has_value() &&
      *patch.total_batches < *patch.progress_batches) {
    throw std::invalid_argument(
        "ReplaySessionStatePatch.total_batches must be >= progress_batches");
  }
}

void validate_session_id(const std::string& session_id) {
  if (session_id.empty()) {
    throw std::invalid_argument("session_id must not be empty");
  }
}

std::string select_projection() {
  return R"SQL(
session_id::text AS session_id,
user_id,
name,
strategy::text AS strategy_json,
EXTRACT(EPOCH FROM date_range_from)::double precision AS date_range_from_epoch,
EXTRACT(EPOCH FROM date_range_to)::double precision AS date_range_to_epoch,
solver_config_id,
risk_limits_id,
fee_model::text AS fee_model_json,
session_config_snapshot::text AS session_config_snapshot_json,
status,
total_batches,
progress_batches,
EXTRACT(EPOCH FROM created_at)::double precision AS created_at_epoch,
CASE
  WHEN started_at IS NULL THEN NULL
  ELSE EXTRACT(EPOCH FROM started_at)::double precision
END AS started_at_epoch,
CASE
  WHEN completed_at IS NULL THEN NULL
  ELSE EXTRACT(EPOCH FROM completed_at)::double precision
END AS completed_at_epoch,
error_details,
retry_parent_id::text AS retry_parent_id
)SQL";
}

app::ReplaySession map_replay_session(const pqxx::row& row) {
  app::ReplaySession session;
  session.session_id = row["session_id"].as<std::string>();
  session.user_id = row["user_id"].as<std::string>();
  session.name = row["name"].as<std::string>();
  session.strategy_json = row["strategy_json"].as<std::string>();
  session.date_range_from = from_epoch_seconds(row["date_range_from_epoch"].as<double>());
  session.date_range_to = from_epoch_seconds(row["date_range_to_epoch"].as<double>());
  session.solver_config_id = row["solver_config_id"].as<std::string>();
  session.risk_limits_id = row["risk_limits_id"].as<std::string>();
  session.fee_model_json = row["fee_model_json"].as<std::string>();

  if (row["session_config_snapshot_json"].is_null()) {
    session.session_config_snapshot_json = std::nullopt;
  } else {
    session.session_config_snapshot_json =
        row["session_config_snapshot_json"].as<std::string>();
  }

  session.status = status_from_db(row["status"].as<std::string>());

  if (row["total_batches"].is_null()) {
    session.total_batches = std::nullopt;
  } else {
    session.total_batches = row["total_batches"].as<std::int32_t>();
  }

  session.progress_batches = row["progress_batches"].as<std::int32_t>();
  session.created_at = from_epoch_seconds(row["created_at_epoch"].as<double>());

  if (row["started_at_epoch"].is_null()) {
    session.started_at = std::nullopt;
  } else {
    session.started_at = from_epoch_seconds(row["started_at_epoch"].as<double>());
  }

  if (row["completed_at_epoch"].is_null()) {
    session.completed_at = std::nullopt;
  } else {
    session.completed_at = from_epoch_seconds(row["completed_at_epoch"].as<double>());
  }

  if (row["error_details"].is_null()) {
    session.error_details = std::nullopt;
  } else {
    session.error_details = row["error_details"].as<std::string>();
  }

  if (row["retry_parent_id"].is_null()) {
    session.retry_parent_id = std::nullopt;
  } else {
    session.retry_parent_id = row["retry_parent_id"].as<std::string>();
  }

  return session;
}

app::ReplaySummary map_replay_summary(const pqxx::row& row) {
  app::ReplaySummary summary;
  if (!row["summary_id"].is_null()) {
    summary.summary_id = row["summary_id"].as<std::string>();
  }
  summary.session_id = row["session_id"].as<std::string>();
  summary.total_batches = row["total_batches"].is_null()
                              ? 0
                              : static_cast<uint32_t>(row["total_batches"].as<std::int32_t>());
  summary.processed_batches = row["processed_batches"].is_null()
                                  ? 0
                                  : static_cast<uint32_t>(
                                        row["processed_batches"].as<std::int32_t>());
  summary.failed_batches = row["failed_batches"].is_null()
                               ? 0
                               : static_cast<uint32_t>(
                                     row["failed_batches"].as<std::int32_t>());
  if (!row["total_fill_events"].is_null()) {
    summary.total_fill_events =
        static_cast<uint32_t>(row["total_fill_events"].as<std::int32_t>());
  }
  summary.partial = row["partial"].is_null() ? false : row["partial"].as<bool>();
  summary.total_pnl =
      row["total_pnl"].is_null() ? 0.0 : row["total_pnl"].as<double>();
  summary.avg_pnl = row["avg_pnl"].is_null() ? 0.0 : row["avg_pnl"].as<double>();
  summary.avg_is = row["avg_is"].is_null() ? 0.0 : row["avg_is"].as<double>();
  summary.std_pnl = row["std_pnl"].is_null() ? 0.0 : row["std_pnl"].as<double>();
  summary.sharpe =
      row["sharpe_ratio"].is_null() ? 0.0 : row["sharpe_ratio"].as<double>();
  summary.avg_fill_rate =
      row["fill_rate"].is_null() ? 0.0 : row["fill_rate"].as<double>();
  summary.avg_solve_time_ms = row["avg_solve_time_ms"].is_null()
                                  ? 0.0
                                  : row["avg_solve_time_ms"].as<double>();
  summary.max_drawdown =
      row["max_drawdown"].is_null() ? 0.0 : row["max_drawdown"].as<double>();
  summary.avg_vwap =
      row["avg_vwap"].is_null() ? 0.0 : row["avg_vwap"].as<double>();
  if (!row["avgis_rule"].is_null()) {
    summary.avgis_rule = row["avgis_rule"].as<std::string>();
  }
  if (!row["decision_price_source"].is_null()) {
    summary.decision_price_source = row["decision_price_source"].as<std::string>();
  }
  summary.no_requested_volume =
      row["no_requested_volume"].is_null() ? false : row["no_requested_volume"].as<bool>();
  if (!row["created_at_epoch"].is_null()) {
    summary.created_at = from_epoch_seconds(row["created_at_epoch"].as<double>());
  }
  return summary;
}

std::unique_ptr<pqxx::connection> open_connection(
    const PostgresReplaySessionRepository::ConnectionFactory& factory) {
  auto conn = factory();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  return conn;
}

}  // namespace

PostgresReplaySessionRepository::PostgresReplaySessionRepository(std::string connection_string)
    : connection_factory_([conn_str = std::move(connection_string)]() {
        return std::make_unique<pqxx::connection>(conn_str);
      }) {}

PostgresReplaySessionRepository::PostgresReplaySessionRepository(
    ConnectionFactory connection_factory)
    : connection_factory_(std::move(connection_factory)) {
  if (!connection_factory_) {
    throw std::invalid_argument(
        "PostgresReplaySessionRepository requires a valid connection factory");
  }
}

bool PostgresReplaySessionRepository::EnsureSchema() {
  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS replay_sessions (
  session_id UUID PRIMARY KEY,
  user_id TEXT NOT NULL,
  name VARCHAR(255) NOT NULL,
  strategy JSONB NOT NULL,
  date_range_from TIMESTAMPTZ NOT NULL,
  date_range_to TIMESTAMPTZ NOT NULL,
  solver_config_id TEXT NOT NULL,
  risk_limits_id TEXT NOT NULL,
  fee_model JSONB NOT NULL,
  session_config_snapshot JSONB,
  status TEXT NOT NULL CHECK (status IN ('pending', 'running', 'completed', 'failed', 'cancelled')),
  total_batches INTEGER,
  progress_batches INTEGER NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  started_at TIMESTAMPTZ,
  completed_at TIMESTAMPTZ,
  error_details TEXT,
  retry_parent_id UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL,
  CHECK (date_range_from <= date_range_to),
  CHECK (progress_batches >= 0),
  CHECK (total_batches IS NULL OR total_batches >= progress_batches)
)
)SQL");

  tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS replay_summaries (
  summary_id UUID PRIMARY KEY,
  session_id UUID NOT NULL UNIQUE REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  total_pnl NUMERIC(24, 8),
  avg_is NUMERIC(24, 8),
  avg_pnl NUMERIC(24, 8),
  std_pnl NUMERIC(24, 8),
  sharpe_ratio NUMERIC(24, 8),
  fill_rate NUMERIC(12, 8),
  avg_vwap NUMERIC(24, 8),
  avg_solve_time_ms NUMERIC(24, 8),
  max_drawdown NUMERIC(24, 8),
  total_batches INTEGER,
  processed_batches INTEGER NOT NULL DEFAULT 0,
  failed_batches INTEGER NOT NULL DEFAULT 0,
  total_fill_events INTEGER,
  partial BOOLEAN NOT NULL DEFAULT FALSE,
  avgis_rule TEXT NOT NULL DEFAULT 'volume_weighted',
  decision_price_source TEXT NOT NULL DEFAULT 'marketdata_mid_with_clearprice_fallback',
  no_requested_volume BOOLEAN NOT NULL DEFAULT FALSE,
  metrics JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  CHECK (processed_batches >= 0),
  CHECK (failed_batches >= 0),
  CHECK (total_batches IS NULL OR total_batches >= processed_batches)
)
)SQL");

  tx.exec(R"SQL(
ALTER TABLE replay_summaries
  ADD COLUMN IF NOT EXISTS avgis_rule TEXT NOT NULL DEFAULT 'volume_weighted',
  ADD COLUMN IF NOT EXISTS decision_price_source TEXT NOT NULL DEFAULT 'marketdata_mid_with_clearprice_fallback',
  ADD COLUMN IF NOT EXISTS no_requested_volume BOOLEAN NOT NULL DEFAULT FALSE
)SQL");

  tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS replay_compare_cache (
  compare_key TEXT PRIMARY KEY,
  session_a_id UUID NOT NULL REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  session_b_id UUID NOT NULL REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  comparison_json JSONB NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at TIMESTAMPTZ,
  CHECK (session_a_id <> session_b_id)
)
)SQL");

  tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS replay_audit_runs (
  audit_run_id UUID PRIMARY KEY,
  session_id UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL,
  requested_by TEXT NOT NULL,
  batch_id TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('pending', 'running', 'completed', 'failed', 'cancelled')),
  equivalent BOOLEAN,
  tolerance_json JSONB,
  replay_result_json JSONB,
  production_result_json JSONB,
  diff_json JSONB,
  error_details TEXT,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  completed_at TIMESTAMPTZ
)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_sessions_user_created_idx
ON replay_sessions (user_id, created_at DESC)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_sessions_status_created_idx
ON replay_sessions (status, created_at DESC)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_sessions_user_id_idx
ON replay_sessions (user_id)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_sessions_status_idx
ON replay_sessions (status)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_sessions_created_idx
ON replay_sessions (created_at DESC)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_sessions_date_range_idx
ON replay_sessions (date_range_from, date_range_to)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_sessions_retry_parent_idx
ON replay_sessions (retry_parent_id)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_summaries_created_at_idx
ON replay_summaries (created_at DESC)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_compare_cache_session_pair_idx
ON replay_compare_cache (session_a_id, session_b_id)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_audit_runs_session_id_idx
ON replay_audit_runs (session_id)
)SQL");

  tx.exec(R"SQL(
CREATE INDEX IF NOT EXISTS replay_audit_runs_created_at_idx
ON replay_audit_runs (created_at DESC)
)SQL");

  tx.commit();
  return true;
}

app::ReplaySession PostgresReplaySessionRepository::Create(const app::ReplaySession& session) {
  validate_create_session(session);

  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  ParamCollector params;

  const std::string session_config_sql = session.session_config_snapshot_json.has_value()
                                             ? params.Add(*session.session_config_snapshot_json) +
                                                   "::jsonb"
                                             : "NULL";

  const std::string total_batches_sql = session.total_batches.has_value()
                                            ? params.Add(std::to_string(*session.total_batches)) +
                                                  "::integer"
                                            : "NULL";

  const std::string started_at_sql = session.started_at.has_value()
                                         ? "to_timestamp(" +
                                               params.Add(epoch_seconds_string(*session.started_at)) +
                                               "::double precision)"
                                         : "NULL";

  const std::string completed_at_sql = session.completed_at.has_value()
                                           ? "to_timestamp(" +
                                                 params.Add(epoch_seconds_string(*session.completed_at)) +
                                                 "::double precision)"
                                           : "NULL";

  const std::string error_details_sql =
      session.error_details.has_value() ? params.Add(*session.error_details) : "NULL";

  const std::string retry_parent_id_sql = session.retry_parent_id.has_value()
                                              ? params.Add(*session.retry_parent_id) + "::uuid"
                                              : "NULL";

  const std::string sql =
      "INSERT INTO replay_sessions ("
      "session_id, "
      "user_id, "
      "name, "
      "strategy, "
      "date_range_from, "
      "date_range_to, "
      "solver_config_id, "
      "risk_limits_id, "
      "fee_model, "
      "session_config_snapshot, "
      "status, "
      "total_batches, "
      "progress_batches, "
      "created_at, "
      "started_at, "
      "completed_at, "
      "error_details, "
      "retry_parent_id"
      ") VALUES (" +
      params.Add(session.session_id) + "::uuid, " +
      params.Add(session.user_id) + ", " +
      params.Add(session.name) + ", " +
      params.Add(session.strategy_json) + "::jsonb, " +
      "to_timestamp(" + params.Add(epoch_seconds_string(session.date_range_from)) +
      "::double precision), " +
      "to_timestamp(" + params.Add(epoch_seconds_string(session.date_range_to)) +
      "::double precision), " +
      params.Add(session.solver_config_id) + ", " +
      params.Add(session.risk_limits_id) + ", " +
      params.Add(session.fee_model_json) + "::jsonb, " +
      session_config_sql + ", " +
      params.Add(status_to_db(session.status)) + ", " +
      total_batches_sql + ", " +
      params.Add(std::to_string(session.progress_batches)) + "::integer, " +
      "to_timestamp(" + params.Add(epoch_seconds_string(session.created_at)) +
      "::double precision), " +
      started_at_sql + ", " +
      completed_at_sql + ", " +
      error_details_sql + ", " +
      retry_parent_id_sql + ") "
      "RETURNING " +
      select_projection();

  const auto rows = tx.exec_params(
      sql,
      pqxx::prepare::make_dynamic_params(params.values.begin(), params.values.end()));

  if (rows.size() != 1) {
    throw std::runtime_error("Unexpected INSERT result for replay_sessions");
  }

  auto created = map_replay_session(rows.front());
  tx.commit();
  return created;
}

std::optional<app::ReplaySession> PostgresReplaySessionRepository::GetById(
    const std::string& session_id) {
  validate_session_id(session_id);

  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  const std::string sql = "SELECT " + select_projection() +
                          " FROM replay_sessions WHERE session_id = $1::uuid";

  const auto rows = tx.exec_params(sql, session_id);
  if (rows.empty()) {
    tx.commit();
    return std::nullopt;
  }

  if (rows.size() != 1) {
    throw std::runtime_error("Unexpected number of rows from replay_sessions by session_id");
  }

  auto session = map_replay_session(rows.front());
  tx.commit();
  return session;
}

std::optional<app::ReplaySummary> PostgresReplaySessionRepository::GetSummaryBySessionId(
    const std::string& session_id) {
  validate_session_id(session_id);

  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  const std::string sql =
      "SELECT "
      "summary_id::text AS summary_id, "
      "session_id::text AS session_id, "
      "total_batches, "
      "processed_batches, "
      "failed_batches, "
      "total_fill_events, "
      "partial, "
      "total_pnl, "
      "avg_pnl, "
      "avg_is, "
      "std_pnl, "
      "sharpe_ratio, "
      "fill_rate, "
      "avg_vwap, "
      "avgis_rule, "
      "decision_price_source, "
      "no_requested_volume, "
      "avg_solve_time_ms, "
      "max_drawdown, "
      "EXTRACT(EPOCH FROM created_at)::double precision AS created_at_epoch "
      "FROM replay_summaries WHERE session_id = $1::uuid";

  const auto rows = tx.exec_params(sql, session_id);
  if (rows.empty()) {
    tx.commit();
    return std::nullopt;
  }
  if (rows.size() != 1) {
    throw std::runtime_error("Unexpected number of rows from replay_summaries by session_id");
  }

  auto summary = map_replay_summary(rows.front());
  tx.commit();
  return summary;
}

std::vector<app::ReplaySession> PostgresReplaySessionRepository::List(
    const app::ReplaySessionListFilter& filter) {
  validate_list_filter(filter);

  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  ParamCollector params;
  std::vector<std::string> where_clauses;

  if (filter.user_id.has_value()) {
    where_clauses.push_back("user_id = " + params.Add(*filter.user_id));
  }
  if (filter.status.has_value()) {
    where_clauses.push_back("status = " + params.Add(status_to_db(*filter.status)));
  }
  if (filter.created_from.has_value()) {
    where_clauses.push_back(
        "created_at >= to_timestamp(" +
        params.Add(epoch_seconds_string(*filter.created_from)) +
        "::double precision)");
  }
  if (filter.created_to.has_value()) {
    where_clauses.push_back(
        "created_at <= to_timestamp(" +
        params.Add(epoch_seconds_string(*filter.created_to)) +
        "::double precision)");
  }
  if (filter.replay_from.has_value()) {
    where_clauses.push_back(
        "date_range_from >= to_timestamp(" +
        params.Add(epoch_seconds_string(*filter.replay_from)) +
        "::double precision)");
  }
  if (filter.replay_to.has_value()) {
    where_clauses.push_back(
        "date_range_to <= to_timestamp(" +
        params.Add(epoch_seconds_string(*filter.replay_to)) +
        "::double precision)");
  }

  std::string sql = "SELECT " + select_projection() + " FROM replay_sessions";
  if (!where_clauses.empty()) {
    sql += " WHERE " + join_clauses(where_clauses, " AND ");
  }

  sql += " ORDER BY created_at DESC, session_id DESC";

  if (filter.limit.has_value()) {
    sql += " LIMIT " + params.Add(std::to_string(*filter.limit)) + "::integer";
  }
  if (filter.offset.has_value()) {
    sql += " OFFSET " + params.Add(std::to_string(*filter.offset)) + "::integer";
  }

  pqxx::result rows;
  if (params.values.empty()) {
    rows = tx.exec(sql);
  } else {
    rows = tx.exec_params(
        sql,
        pqxx::prepare::make_dynamic_params(params.values.begin(), params.values.end()));
  }

  std::vector<app::ReplaySession> sessions;
  sessions.reserve(rows.size());
  for (const auto& row : rows) {
    sessions.push_back(map_replay_session(row));
  }

  tx.commit();
  return sessions;
}

bool PostgresReplaySessionRepository::UpdateState(
    const std::string& session_id,
    const app::ReplaySessionStatePatch& patch) {
  validate_session_id(session_id);
  validate_state_patch(patch);

  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  const auto existing_rows = tx.exec_params(R"SQL(
SELECT progress_batches, total_batches
FROM replay_sessions
WHERE session_id = $1::uuid
FOR UPDATE
)SQL",
                                            session_id);

  if (existing_rows.empty()) {
    tx.commit();
    return false;
  }

  const auto& existing = existing_rows.front();
  const auto current_progress = existing["progress_batches"].as<std::int32_t>();
  std::optional<std::int32_t> current_total = std::nullopt;
  if (!existing["total_batches"].is_null()) {
    current_total = existing["total_batches"].as<std::int32_t>();
  }

  const auto effective_progress = patch.progress_batches.value_or(current_progress);
  const auto effective_total = patch.total_batches.has_value() ? patch.total_batches : current_total;

  if (effective_progress < 0) {
    throw std::invalid_argument("ReplaySessionStatePatch.progress_batches must be >= 0");
  }
  if (effective_total.has_value() && *effective_total < effective_progress) {
    throw std::invalid_argument(
        "ReplaySessionStatePatch.total_batches must be >= progress_batches");
  }

  ParamCollector params;
  std::vector<std::string> set_clauses;

  if (patch.status.has_value()) {
    set_clauses.push_back("status = " + params.Add(status_to_db(*patch.status)));
  }
  if (patch.total_batches.has_value()) {
    set_clauses.push_back(
        "total_batches = " + params.Add(std::to_string(*patch.total_batches)) + "::integer");
  }
  if (patch.progress_batches.has_value()) {
    set_clauses.push_back("progress_batches = " +
                          params.Add(std::to_string(*patch.progress_batches)) +
                          "::integer");
  }
  if (patch.started_at.has_value()) {
    set_clauses.push_back(
        "started_at = to_timestamp(" +
        params.Add(epoch_seconds_string(*patch.started_at)) + "::double precision)");
  }
  if (patch.completed_at.has_value()) {
    set_clauses.push_back(
        "completed_at = to_timestamp(" +
        params.Add(epoch_seconds_string(*patch.completed_at)) + "::double precision)");
  }
  if (patch.error_details.has_value()) {
    set_clauses.push_back("error_details = " + params.Add(*patch.error_details));
  }

  const std::string where_session_id = params.Add(session_id);

  const std::string sql =
      "UPDATE replay_sessions SET " + join_clauses(set_clauses, ", ") +
      " WHERE session_id = " + where_session_id + "::uuid";

  const auto update_result = tx.exec_params(
      sql,
      pqxx::prepare::make_dynamic_params(params.values.begin(), params.values.end()));

  if (update_result.affected_rows() == 0) {
    tx.commit();
    return false;
  }

  tx.commit();
  return true;
}

std::vector<app::ReplaySession> PostgresReplaySessionRepository::GetRetryChain(
    const std::string& session_id) {
  validate_session_id(session_id);

  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  const std::string sql = R"SQL(
WITH RECURSIVE retry_chain AS (
  SELECT session_id::text, retry_parent_id::text
  FROM replay_sessions
  WHERE session_id = $1::uuid
  
  UNION ALL
  
  SELECT s.session_id::text, s.retry_parent_id::text
  FROM replay_sessions s
  INNER JOIN retry_chain rc ON s.session_id = rc.retry_parent_id::uuid
)
SELECT )SQL" + select_projection() + R"SQL(
FROM replay_sessions
WHERE session_id IN (SELECT session_id::uuid FROM retry_chain)
ORDER BY created_at ASC
)SQL";

  const auto rows = tx.exec_params(sql, session_id);

  std::vector<app::ReplaySession> chain;
  chain.reserve(rows.size());
  for (const auto& row : rows) {
    chain.push_back(map_replay_session(row));
  }

  tx.commit();
  return chain;
}

void PostgresReplaySessionRepository::SaveSummary(const app::ReplaySummary& summary) {
  validate_session_id(summary.session_id);

  auto conn = open_connection(connection_factory_);
  pqxx::work tx(*conn);

  // Upsert keyed by session_id (UNIQUE). The summary_id is generated on
  // insert; on conflict we keep the original summary_id and refresh the
  // metric columns. This makes the operation idempotent under retries
  // (F15-BACKTEST-7) and consistent with the way RunReplaySession may
  // call SaveSummary multiple times for the same session.
  const std::string sql = R"SQL(
INSERT INTO replay_summaries (
  summary_id,
  session_id,
  total_pnl,
  avg_is,
  avg_pnl,
  std_pnl,
  sharpe_ratio,
  fill_rate,
  avg_vwap,
  avg_solve_time_ms,
  max_drawdown,
  total_batches,
  processed_batches,
  failed_batches,
  total_fill_events,
  partial,
  avgis_rule,
  decision_price_source,
  no_requested_volume
) VALUES (
  gen_random_uuid(),
  $1::uuid,
  $2::numeric,
  $3::numeric,
  $4::numeric,
  $5::numeric,
  $6::numeric,
  $7::numeric,
  $8::numeric,
  $9::numeric,
  $10::numeric,
  $11::integer,
  $12::integer,
  $13::integer,
  $14::integer,
  $15::boolean,
  $16::text,
  $17::text,
  $18::boolean
)
ON CONFLICT (session_id) DO UPDATE SET
  total_pnl = EXCLUDED.total_pnl,
  avg_is = EXCLUDED.avg_is,
  avg_pnl = EXCLUDED.avg_pnl,
  std_pnl = EXCLUDED.std_pnl,
  sharpe_ratio = EXCLUDED.sharpe_ratio,
  fill_rate = EXCLUDED.fill_rate,
  avg_vwap = EXCLUDED.avg_vwap,
  avg_solve_time_ms = EXCLUDED.avg_solve_time_ms,
  max_drawdown = EXCLUDED.max_drawdown,
  total_batches = EXCLUDED.total_batches,
  processed_batches = EXCLUDED.processed_batches,
  failed_batches = EXCLUDED.failed_batches,
  total_fill_events = EXCLUDED.total_fill_events,
  partial = EXCLUDED.partial,
  avgis_rule = EXCLUDED.avgis_rule,
  decision_price_source = EXCLUDED.decision_price_source,
  no_requested_volume = EXCLUDED.no_requested_volume
)SQL";

  tx.exec_params(
      sql,
      summary.session_id,
      summary.total_pnl,
      summary.avg_is,
      summary.avg_pnl,
      summary.std_pnl,
      summary.sharpe,
      summary.avg_fill_rate,
      summary.avg_vwap,
      summary.avg_solve_time_ms,
      summary.max_drawdown,
      static_cast<std::int32_t>(summary.total_batches),
      static_cast<std::int32_t>(summary.processed_batches),
      static_cast<std::int32_t>(summary.failed_batches),
      static_cast<std::int32_t>(summary.total_fill_events),
      summary.partial,
      summary.avgis_rule,
      summary.decision_price_source,
      summary.no_requested_volume);
  tx.commit();
}

}  // namespace cex::backtest::infra
