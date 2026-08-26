// ============================================================================
// clickhouse_vector_segment_storage.cpp — F-05A (T-F05A-206). См. .hpp.
// ============================================================================

#include "infra/clickhouse/clickhouse_vector_segment_storage.hpp"

#include <sstream>

#include "cex/common/log.hpp"

namespace cex::market_data::infra::clickhouse {

namespace {

std::string JsonEscape(const std::string& input) {
  std::ostringstream out;
  for (char c : input) {
    switch (c) {
      case '\"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << c; break;
    }
  }
  return out.str();
}

std::string WJson(const std::vector<double>& w) {
  std::ostringstream a;
  a << "[";
  for (std::size_t i = 0; i < w.size(); ++i) {
    if (i) a << ",";
    a << w[i];
  }
  a << "]";
  return a.str();
}

}  // namespace

ClickHouseVectorSegmentStorage::ClickHouseVectorSegmentStorage(
    const ClickHouseConfig& config, const std::string& table)
    : client_(::clickhouse::ClientOptions()
                  .SetHost(config.host)
                  .SetPort(config.tcp_port)
                  .SetUser(config.user)
                  .SetPassword(config.password)
                  .SetDefaultDatabase(config.database)),
      table_(table),
      database_(config.database) {}

void ClickHouseVectorSegmentStorage::EnsureSchema() {
  std::ostringstream q;
  q << "CREATE TABLE IF NOT EXISTS " << database_ << "." << table_ << " ("
    << "segment_id String,"
    << "batch_id String,"
    << "venue_id String,"
    << "source_order_id String,"
    << "pair LowCardinality(String),"
    << "side LowCardinality(String),"
    << "w_json String,"
    << "p_high Decimal128(18),"
    << "d_hl Decimal128(18),"
    << "q_rate Decimal128(18),"
    << "q_max Decimal128(18),"
    << "effective_price Decimal128(18),"
    << "event_time_ms Int64,"
    << "ingested_at DateTime DEFAULT now()"
    << ") ENGINE = ReplacingMergeTree(event_time_ms) "
    << "ORDER BY (batch_id, segment_id)";
  try {
    client_.Execute(::clickhouse::Query(q.str()));
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "vector_flow_segments_history EnsureSchema failed",
                          {{"error", e.what()}});
  }
}

void ClickHouseVectorSegmentStorage::SaveSegments(
    const std::string& batch_id, const domain::VectorizeResult& result,
    long long event_ts_ms) {
  if (result.segments.empty()) return;

  std::ostringstream q;
  q << "INSERT INTO " << database_ << "." << table_ << " FORMAT JSONEachRow\n";
  for (const auto& s : result.segments) {
    q << "{"
      << "\"segment_id\":\"" << JsonEscape(s.segment_id) << "\","
      << "\"batch_id\":\"" << JsonEscape(batch_id) << "\","
      << "\"venue_id\":\"" << JsonEscape(s.venue_id) << "\","
      << "\"source_order_id\":\"" << JsonEscape(s.source_order_id) << "\","
      << "\"pair\":\"" << JsonEscape(s.pair) << "\","
      << "\"side\":\"" << domain::ToString(s.side) << "\","
      << "\"w_json\":\"" << JsonEscape(WJson(s.w)) << "\","
      << "\"p_high\":" << s.p_high.to_string() << ","
      << "\"d_hl\":" << s.d_hl.to_string() << ","
      << "\"q_rate\":" << s.q_rate.to_string() << ","
      << "\"q_max\":" << s.q_max.to_string() << ","
      << "\"effective_price\":" << s.effective_price.to_string() << ","
      << "\"event_time_ms\":" << event_ts_ms
      << "}\n";
  }

  try {
    client_.Execute(::clickhouse::Query(q.str()));
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "Failed to persist vector_flow_segments_history",
                          {{"batch_id", batch_id}, {"error", e.what()}});
  }
}

}  // namespace cex::market_data::infra::clickhouse
