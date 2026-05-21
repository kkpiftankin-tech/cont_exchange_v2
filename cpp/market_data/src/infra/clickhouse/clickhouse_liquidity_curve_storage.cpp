#include "infra/clickhouse/clickhouse_liquidity_curve_storage.hpp"

#include <sstream>
#include <string>
#include <cstdlib>
#include <algorithm>

#include "cex/common/log.hpp"
#include "cex/common/time.hpp"

namespace cex::market_data::infra::clickhouse {

namespace {

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

std::string DoubleArrayToJsonString(const google::protobuf::RepeatedField<double>& values) {
  std::ostringstream out;
  out << "[";
  for (int i = 0; i < values.size(); ++i) {
    if (i > 0) out << ",";
    out << values[i];
  }
  out << "]";
  return out.str();
}

int64_t TimestampToUnixMs(const google::protobuf::Timestamp& ts) {
  return static_cast<int64_t>(ts.seconds()) * 1000LL +
         static_cast<int64_t>(ts.nanos()) / 1000000LL;
}

std::string BuildSideCurveJson(const fob::venue::v1::SideLiquidityCurve& curve) {
  std::ostringstream out;
  out << "{";
  out << "\"q_grid\":" << DoubleArrayToJsonString(curve.q_grid()) << ",";
  out << "\"p_of_q\":" << DoubleArrayToJsonString(curve.p_of_q()) << ",";
  out << "\"s_of_q\":" << DoubleArrayToJsonString(curve.s_of_q()) << ",";
  out << "\"v_grid\":" << DoubleArrayToJsonString(curve.v_grid()) << ",";
  out << "\"l_of_v\":" << DoubleArrayToJsonString(curve.l_of_v()) << ",";
  out << "\"l_of_v_monotone\":" << DoubleArrayToJsonString(curve.l_of_v_monotone()) << ",";
  out << "\"p_star_grid\":" << DoubleArrayToJsonString(curve.p_star_grid()) << ",";
  out << "\"s_star_of_p\":" << DoubleArrayToJsonString(curve.s_star_of_p()) << ",";
  out << "\"q_star_of_p\":" << DoubleArrayToJsonString(curve.q_star_of_p());
  out << "}";
  return out.str();
}

std::string TagOrEmpty(const fob::venue::v1::VenueLiquidityCurve& curve,
                       const std::string& key) {
  if (!curve.has_meta()) return {};
  const auto& tags = curve.meta().tags();
  const auto it = tags.find(key);
  if (it == tags.end()) return {};
  return it->second;
}

double ParseDoubleOrDefault(const std::string& text, const double fallback) {
  if (text.empty()) return fallback;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == nullptr || *end != '\0') return fallback;
  return value;
}

}  // namespace

ClickHouseLiquidityCurveStorage::ClickHouseLiquidityCurveStorage(const ClickHouseConfig& config)
    : client_(::clickhouse::ClientOptions()
                  .SetHost(config.host)
                  .SetPort(config.tcp_port)
                  .SetUser(config.user)
                  .SetPassword(config.password)
                  .SetDefaultDatabase(config.database)),
      table_name_(config.liquidity_curves_table),
      database_(config.database),
      retention_days_(std::max(1, config.liquidity_curves_retention_days)) {}

void ClickHouseLiquidityCurveStorage::EnsureSchema() {
  const int retention_days = std::max(1, retention_days_);
  std::ostringstream query;
  query << "CREATE TABLE IF NOT EXISTS " << database_ << "." << table_name_ << " ("
        << "timestamp_ms Int64,"
        << "venue_id String,"
        << "symbol String,"
        << "base String,"
        << "quote String,"
        << "snapshot_id String,"
        << "curve_id String,"
        << "mid_price Float64,"
        << "mid_price_units Int64,"
        << "mid_price_scale Int32,"
        << "tau_ms Float64,"
        << "confidence Float64,"
        << "level String,"
        << "epsilon1 Float64,"
        << "epsilon2 Float64,"
        << "epsilon3 Float64,"
        << "schema_version UInt32,"
        << "min_compatible_schema_version UInt32,"
        << "producer_version String,"
        << "model_config_version String,"
        << "requested_level String,"
        << "effective_level String,"
        << "degradation_reason String,"
        << "quality_action String,"
        << "build_latency_ms Float64,"
        << "l3_impact_model String,"
        << "l3_execution_blend_weight Float64,"
        << "amm_path String,"
        << "calibration_amm_pool_fee_rate Float64,"
        << "calibration_amm_gas_cost_quote Float64,"
        << "calibration_amm_execution_overhead_bps Float64,"
        << "calibration_amm_slippage_multiplier Float64,"
        << "calibration_amm_effective_fee_rate Float64,"
        << "bid_curve_json String,"
        << "ask_curve_json String,"
        << "ingested_at DateTime DEFAULT now()"
        << ") ENGINE = MergeTree "
        << "ORDER BY (timestamp_ms, venue_id, symbol) "
        << "TTL toDateTime(timestamp_ms / 1000) + INTERVAL " << retention_days
        << " DAY DELETE";

  try {
    client_.Execute(::clickhouse::Query(query.str()));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " MODIFY TTL toDateTime(timestamp_ms / 1000) + INTERVAL " +
        std::to_string(retention_days) + " DAY DELETE"));
    // Backward-compatible online schema evolution for pre-existing tables.
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS schema_version UInt32"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS min_compatible_schema_version UInt32"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS producer_version String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS model_config_version String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS requested_level String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS effective_level String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS degradation_reason String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS quality_action String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS build_latency_ms Float64"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS l3_impact_model String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS l3_execution_blend_weight Float64"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS amm_path String"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS calibration_amm_pool_fee_rate Float64"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS calibration_amm_gas_cost_quote Float64"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS calibration_amm_execution_overhead_bps Float64"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS calibration_amm_slippage_multiplier Float64"));
    client_.Execute(::clickhouse::Query(
        "ALTER TABLE " + database_ + "." + table_name_ +
        " ADD COLUMN IF NOT EXISTS calibration_amm_effective_fee_rate Float64"));
    cex::common::log_json("INFO", "Liquidity curves table ensured", {{"table", table_name_}});
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "Failed to create liquidity curves table",
                          {{"error", e.what()}});
  }
}

void ClickHouseLiquidityCurveStorage::Save(const fob::venue::v1::VenueLiquidityCurve& curve) {
  std::ostringstream insert_query;
  insert_query << "INSERT INTO " << database_ << "." << table_name_ << " FORMAT JSONEachRow";
  
  std::ostringstream row;
  row << "{";
  
  // Timestamp
  int64_t ts_ms = curve.has_timestamp() ? TimestampToUnixMs(curve.timestamp()) : 0;
  row << "\"timestamp_ms\":" << ts_ms << ",";
  
  // Venue and instrument
  row << "\"venue_id\":\"" << JsonEscape(curve.venue_id()) << "\",";
  row << "\"symbol\":\"" << JsonEscape(curve.instrument().symbol()) << "\",";
  row << "\"base\":\"" << JsonEscape(curve.instrument().base()) << "\",";
  row << "\"quote\":\"" << JsonEscape(curve.instrument().quote()) << "\",";
  row << "\"snapshot_id\":\"" << JsonEscape(curve.snapshot_id()) << "\",";
  row << "\"curve_id\":\"" << JsonEscape(curve.curve_id()) << "\",";
  
  // Mid price
  double mid_price = 0.0;
  int64_t mid_units = 0;
  int32_t mid_scale = 0;
  if (curve.has_mid_price()) {
    mid_price = static_cast<double>(curve.mid_price().units()) / std::pow(10.0, curve.mid_price().scale());
    mid_units = curve.mid_price().units();
    mid_scale = curve.mid_price().scale();
  }
  row << "\"mid_price\":" << mid_price << ",";
  row << "\"mid_price_units\":" << mid_units << ",";
  row << "\"mid_price_scale\":" << mid_scale << ",";
  
  // Metrics
  row << "\"tau_ms\":" << curve.tau_ms() << ",";
  row << "\"confidence\":" << curve.confidence() << ",";
  row << "\"level\":\"" << JsonEscape(curve.level()) << "\",";
  row << "\"epsilon1\":" << curve.epsilon1() << ",";
  row << "\"epsilon2\":" << curve.epsilon2() << ",";
  row << "\"epsilon3\":" << curve.epsilon3() << ",";
  row << "\"schema_version\":" << curve.schema_version() << ",";
  row << "\"min_compatible_schema_version\":" << curve.min_compatible_schema_version() << ",";
  row << "\"producer_version\":\"" << JsonEscape(curve.producer_version()) << "\",";
  row << "\"model_config_version\":\""
      << JsonEscape(TagOrEmpty(curve, "model_config_version")) << "\",";
  row << "\"requested_level\":\""
      << JsonEscape(TagOrEmpty(curve, "requested_level")) << "\",";
  row << "\"effective_level\":\""
      << JsonEscape(TagOrEmpty(curve, "effective_level")) << "\",";
  row << "\"degradation_reason\":\""
      << JsonEscape(TagOrEmpty(curve, "degradation_reason")) << "\",";
  row << "\"quality_action\":\""
      << JsonEscape(TagOrEmpty(curve, "quality_action")) << "\",";
  row << "\"build_latency_ms\":"
      << ParseDoubleOrDefault(TagOrEmpty(curve, "build_latency_ms"), 0.0) << ",";
  row << "\"l3_impact_model\":\""
      << JsonEscape(TagOrEmpty(curve, "l3_impact_model")) << "\",";
  row << "\"l3_execution_blend_weight\":"
      << ParseDoubleOrDefault(TagOrEmpty(curve, "l3_execution_blend_weight"), 0.0) << ",";
  row << "\"amm_path\":\"" << JsonEscape(TagOrEmpty(curve, "amm_path")) << "\",";
  row << "\"calibration_amm_pool_fee_rate\":"
      << ParseDoubleOrDefault(TagOrEmpty(curve, "calibration.amm.pool_fee_rate"), 0.0) << ",";
  row << "\"calibration_amm_gas_cost_quote\":"
      << ParseDoubleOrDefault(TagOrEmpty(curve, "calibration.amm.gas_cost_quote"), 0.0) << ",";
  row << "\"calibration_amm_execution_overhead_bps\":"
      << ParseDoubleOrDefault(
             TagOrEmpty(curve, "calibration.amm.execution_overhead_bps"), 0.0)
      << ",";
  row << "\"calibration_amm_slippage_multiplier\":"
      << ParseDoubleOrDefault(
             TagOrEmpty(curve, "calibration.amm.slippage_multiplier"), 0.0)
      << ",";
  row << "\"calibration_amm_effective_fee_rate\":"
      << ParseDoubleOrDefault(
             TagOrEmpty(curve, "calibration.amm.effective_fee_rate"), 0.0)
      << ",";

  // Curves JSON
  std::string bid_json = curve.has_bid_curve() ? BuildSideCurveJson(curve.bid_curve()) : "{}";
  std::string ask_json = curve.has_ask_curve() ? BuildSideCurveJson(curve.ask_curve()) : "{}";
  row << "\"bid_curve_json\":\"" << JsonEscape(bid_json) << "\",";
  row << "\"ask_curve_json\":\"" << JsonEscape(ask_json) << "\"";
  
  row << "}";
  
  // Build the full insert query with data
  std::ostringstream full_query;
  full_query << insert_query.str() << " " << row.str();
  
  try {
    client_.Execute(::clickhouse::Query(full_query.str()));
    cex::common::log_json("INFO", "Stored venue.liquidity.fob in ClickHouse",
                          {{"service", "market_data"},
                           {"component", "clickhouse_liquidity_curve_storage"},
                           {"participant", "ClickHouse"},
                           {"stage", "store_liquidity_curve"},
                           {"table", database_ + "." + table_name_},
                           {"venue", curve.venue_id()},
                           {"symbol", curve.instrument().symbol()},
                           {"curve_id", curve.curve_id()},
                           {"snapshot_id", curve.snapshot_id()},
                           {"level", curve.level()},
                           {"confidence", std::to_string(curve.confidence())},
                           {"ts_ms", std::to_string(ts_ms)},
                           {"source_file",
                            "cpp/market_data/src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp"}});
  } catch (const std::exception& e) {
    cex::common::log_json("ERROR", "Failed to save liquidity curve to ClickHouse",
                          {{"service", "market_data"},
                           {"component", "clickhouse_liquidity_curve_storage"},
                           {"participant", "ClickHouse"},
                           {"stage", "store_liquidity_curve"},
                           {"table", database_ + "." + table_name_},
                           {"error", e.what()},
                           {"venue", curve.venue_id()},
                           {"symbol", curve.instrument().symbol()},
                           {"curve_id", curve.curve_id()},
                           {"snapshot_id", curve.snapshot_id()},
                           {"source_file",
                            "cpp/market_data/src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp"}});
  }
}

}  // namespace cex::market_data::infra::clickhouse
