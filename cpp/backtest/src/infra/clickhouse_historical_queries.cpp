#include "infra/clickhouse_historical_queries.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace cex::backtest::infra::historical_queries {
namespace {

std::string EscapeSqlString(const std::string& value) {
  std::ostringstream out;
  for (char ch : value) {
    if (ch == '\\' || ch == '\'') out << '\\';
    out << ch;
  }
  return out.str();
}

std::vector<std::string> SortedUniqueEscaped(const std::vector<std::string>& values) {
  std::vector<std::string> out;
  out.reserve(values.size());
  for (const auto& value : values) {
    if (!value.empty()) out.push_back(EscapeSqlString(value));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string SqlStringList(const std::vector<std::string>& values) {
  const auto escaped = SortedUniqueEscaped(values);
  std::ostringstream out;
  for (size_t i = 0; i < escaped.size(); ++i) {
    if (i > 0) out << ", ";
    out << "'" << escaped[i] << "'";
  }
  return out.str();
}

std::vector<int64_t> SortedUniqueInts(const std::vector<int64_t>& values) {
  std::vector<int64_t> out = values;
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string IntList(const std::vector<int64_t>& values) {
  const auto sorted = SortedUniqueInts(values);
  std::ostringstream out;
  for (size_t i = 0; i < sorted.size(); ++i) {
    if (i > 0) out << ", ";
    out << sorted[i];
  }
  return out.str();
}

constexpr const char* kBatchResultsSelect =
    "SELECT batch_id, event_time_ms, source, correlation_id, partition_key, "
    "residual_norm, solve_time_ms, num_active_orders, config_version, "
    "solver_diagnostics_json, clear_prices_json, executed_rates_json, "
    "used_liquidity_json, fills_count ";

constexpr const char* kFillsSelect =
    "SELECT batch_id, event_time_ms, order_id, user_id, symbol, base, quote, "
    "side, executed_qty, price, executed_notional, fee_amount, fee_currency, "
    "liquidity_source, venue_id, snapshot_id, curve_id ";

constexpr const char* kSnapshotsSelect =
    "SELECT venue_id, symbol, event_time_ms, best_bid, best_ask, "
    "mid_price, spread, bid_depth_json, ask_depth_json, tick_size, "
    "lot_size, status ";

constexpr const char* kRiskEventsSelect =
    "SELECT event_id, timestamp, entity_id, event_type, order_id, "
    "details, batch_id ";

constexpr const char* kReplayAgentLogRefsSelect =
    "SELECT batch_seq, original_batch_id ";

constexpr const char* kReplayAgentLogsSelect =
    "SELECT log_id, batch_seq, original_batch_id, event_time_ms, state_json, "
    "action_json, reward, pnl, is_value, fill_rate, fills_applied, "
    "solve_time_ms, residual_norm, solver_error_flag, risk_status, "
    "error_code, error_details, failure_component, fills_json, "
    "batch_result_json, metrics_json, session_config_snapshot_version ";

}  // namespace

std::string BuildBatchResultsRangeQuery(const std::string& table_name,
                                        int64_t from_ms,
                                        int64_t to_ms,
                                        int64_t offset,
                                        int64_t limit) {
  std::ostringstream q;
  q << kBatchResultsSelect
    << "FROM " << table_name
    << " WHERE event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " ORDER BY event_time_ms ASC, batch_id ASC"
    << " LIMIT " << limit << " OFFSET " << offset
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildBatchResultsByIdQuery(const std::string& table_name,
                                       const std::string& batch_id) {
  std::ostringstream q;
  q << kBatchResultsSelect
    << "FROM " << table_name
    << " WHERE batch_id = '" << EscapeSqlString(batch_id) << "'"
    << " ORDER BY event_time_ms ASC, batch_id ASC"
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildBatchResultsByIdsQuery(const std::string& table_name,
                                        const std::vector<std::string>& batch_ids) {
  std::ostringstream q;
  q << kBatchResultsSelect
    << "FROM " << table_name
    << " WHERE batch_id IN (" << SqlStringList(batch_ids) << ")"
    << " ORDER BY event_time_ms ASC, batch_id ASC"
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildBatchResultsResumeQuery(const std::string& table_name,
                                         int64_t from_ms,
                                         int64_t to_ms,
                                         const BatchResultCursor& cursor,
                                         int64_t limit) {
  std::ostringstream q;
  q << kBatchResultsSelect
    << "FROM " << table_name
    << " WHERE event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " AND (event_time_ms, batch_id) > ("
    << cursor.event_time_ms << ", '" << EscapeSqlString(cursor.batch_id) << "')"
    << " ORDER BY event_time_ms ASC, batch_id ASC"
    << " LIMIT " << limit
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildFillsRangeQuery(const std::string& table_name,
                                 int64_t from_ms,
                                 int64_t to_ms,
                                 int64_t offset,
                                 int64_t limit) {
  std::ostringstream q;
  q << kFillsSelect
    << "FROM " << table_name
    << " WHERE event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " ORDER BY event_time_ms ASC, batch_id ASC, order_id ASC"
    << " LIMIT " << limit << " OFFSET " << offset
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildFillsByBatchIdsQuery(const std::string& table_name,
                                      const std::vector<std::string>& batch_ids) {
  std::ostringstream q;
  q << kFillsSelect
    << "FROM " << table_name
    << " WHERE batch_id IN (" << SqlStringList(batch_ids) << ")"
    << " ORDER BY event_time_ms ASC, batch_id ASC, order_id ASC"
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildFillsResumeQuery(const std::string& table_name,
                                  int64_t from_ms,
                                  int64_t to_ms,
                                  const FillCursor& cursor,
                                  int64_t limit) {
  std::ostringstream q;
  q << kFillsSelect
    << "FROM " << table_name
    << " WHERE event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " AND (event_time_ms, batch_id, order_id) > ("
    << cursor.event_time_ms << ", '" << EscapeSqlString(cursor.batch_id)
    << "', '" << EscapeSqlString(cursor.order_id) << "')"
    << " ORDER BY event_time_ms ASC, batch_id ASC, order_id ASC"
    << " LIMIT " << limit
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildMarketdataSnapshotsRangeQuery(const std::string& table_name,
                                               int64_t from_ms,
                                               int64_t to_ms,
                                               int64_t offset,
                                               int64_t limit) {
  std::ostringstream q;
  q << kSnapshotsSelect
    << "FROM " << table_name
    << " WHERE event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " ORDER BY event_time_ms ASC, venue_id ASC, symbol ASC"
    << " LIMIT " << limit << " OFFSET " << offset
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildMarketdataSnapshotsByEventTimesQuery(
    const std::string& table_name,
    const std::vector<int64_t>& event_time_ms) {
  std::ostringstream q;
  q << kSnapshotsSelect
    << "FROM " << table_name
    << " WHERE event_time_ms IN (" << IntList(event_time_ms) << ")"
    << " ORDER BY event_time_ms ASC, venue_id ASC, symbol ASC"
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildMarketdataSnapshotsResumeQuery(const std::string& table_name,
                                                int64_t from_ms,
                                                int64_t to_ms,
                                                const SnapshotCursor& cursor,
                                                int64_t limit) {
  std::ostringstream q;
  q << kSnapshotsSelect
    << "FROM " << table_name
    << " WHERE event_time_ms >= " << from_ms
    << " AND event_time_ms <= " << to_ms
    << " AND (event_time_ms, venue_id, symbol) > ("
    << cursor.event_time_ms << ", '" << EscapeSqlString(cursor.venue_id)
    << "', '" << EscapeSqlString(cursor.symbol) << "')"
    << " ORDER BY event_time_ms ASC, venue_id ASC, symbol ASC"
    << " LIMIT " << limit
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildReplayAgentLogRefsBySessionQuery(const std::string& table_name,
                                                  const std::string& session_id) {
  std::ostringstream q;
  q << kReplayAgentLogRefsSelect
    << "FROM " << table_name << " FINAL"
    << " WHERE session_id = '" << EscapeSqlString(session_id) << "'"
    << " ORDER BY batch_seq ASC, original_batch_id ASC"
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildReplayAgentLogsBySessionQuery(const std::string& table_name,
                                               const std::string& session_id,
                                               uint32_t up_to_batch_seq_exclusive) {
  std::ostringstream q;
  q << kReplayAgentLogsSelect
    << "FROM " << table_name << " FINAL"
    << " WHERE session_id = '" << EscapeSqlString(session_id) << "'";
  if (up_to_batch_seq_exclusive != 0) {
    q << " AND batch_seq < " << up_to_batch_seq_exclusive;
  }
  q << " ORDER BY batch_seq ASC, original_batch_id ASC"
    << " FORMAT TabSeparated";
  return q.str();
}

std::string BuildRiskEventsByBatchIdQuery(const std::string& table_name,
                                          const std::string& batch_id) {
  std::ostringstream q;
  q << kRiskEventsSelect
    << "FROM " << table_name
    << " WHERE batch_id = '" << EscapeSqlString(batch_id) << "'"
    << " ORDER BY timestamp ASC, event_id ASC"
    << " FORMAT TabSeparated";
  return q.str();
}

}  // namespace cex::backtest::infra::historical_queries
