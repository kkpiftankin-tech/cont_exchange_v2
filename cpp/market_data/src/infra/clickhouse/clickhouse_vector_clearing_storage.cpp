// ============================================================================
// clickhouse_vector_clearing_storage.cpp — F-05A (T-F05A-305 1a). См. .hpp.
// ============================================================================

#include "infra/clickhouse/clickhouse_vector_clearing_storage.hpp"

#include <chrono>
#include <sstream>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

namespace cex::market_data::infra::clickhouse {

namespace mv1 = fob::marketdata::v1;

namespace {

std::string JsonEscape(const std::string& in) {
  std::ostringstream o;
  for (char c : in) {
    switch (c) {
      case '\"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default: o << c; break;
    }
  }
  return o.str();
}

/// repeated Decimal → JSON-массив строк (сохраняем точность §9).
std::string DecArr(
    const google::protobuf::RepeatedPtrField<fob::common::v1::Decimal>& arr) {
  std::ostringstream o;
  o << "[";
  for (int i = 0; i < arr.size(); ++i) {
    if (i) o << ",";
    o << "\"" << cex::common::Decimal::from_proto(arr.Get(i)).to_string() << "\"";
  }
  o << "]";
  return o.str();
}

const char* StatusStr(mv1::VectorSolverStatus s) {
  switch (s) {
    case mv1::VECTOR_SOLVER_STATUS_CONVERGED: return "converged";
    case mv1::VECTOR_SOLVER_STATUS_DEGRADED: return "degraded";
    case mv1::VECTOR_SOLVER_STATUS_FAILED: return "failed";
    default: return "unspecified";
  }
}

const char* PolicyStr(mv1::SurplusAllocationPolicy p) {
  switch (p) {
    case mv1::SURPLUS_ALLOCATION_POLICY_REJECT_IF_RESIDUAL: return "reject_if_residual";
    case mv1::SURPLUS_ALLOCATION_POLICY_EXCHANGE_PNL: return "exchange_pnl";
    case mv1::SURPLUS_ALLOCATION_POLICY_SURPLUS_ASSET: return "surplus_asset";
    case mv1::SURPLUS_ALLOCATION_POLICY_MM_LAST_RESORT: return "mm_last_resort";
    default: return "unspecified";
  }
}

std::string SurplusJson(const mv1::VectorClearingResult& r) {
  std::ostringstream o;
  o << "[";
  for (int i = 0; i < r.surplus_size(); ++i) {
    if (i) o << ",";
    const auto& s = r.surplus(i);
    o << "{\"asset\":\"" << JsonEscape(s.asset()) << "\",\"amount\":\""
      << cex::common::Decimal::from_proto(s.amount()).to_string()
      << "\",\"allocation_policy\":\"" << PolicyStr(s.allocation_policy()) << "\"}";
  }
  o << "]";
  return o.str();
}

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

ClickHouseVectorClearingStorage::ClickHouseVectorClearingStorage(
    const ClickHouseConfig& config, const std::string& table)
    : client_(::clickhouse::ClientOptions()
                  .SetHost(config.host)
                  .SetPort(config.tcp_port)
                  .SetUser(config.user)
                  .SetPassword(config.password)
                  .SetDefaultDatabase(config.database)),
      table_(table),
      database_(config.database) {}

void ClickHouseVectorClearingStorage::EnsureSchema() {
  std::ostringstream q;
  q << "CREATE TABLE IF NOT EXISTS " << database_ << "." << table_ << " ("
    << "batch_id String,"
    << "execution_group_id String,"
    << "asset_basis_json String,"
    << "x_json String,"
    << "pi_json String,"
    << "residual_json String,"
    << "residual_norm Float64,"
    << "solver_status LowCardinality(String),"
    << "surplus_json String,"
    << "solver_diagnostics_json String,"
    << "leg_count UInt16,"
    << "event_time_ms Int64,"
    << "ingested_at DateTime DEFAULT now()"
    << ") ENGINE = ReplacingMergeTree(event_time_ms) ORDER BY (batch_id)";
  try {
    client_.Execute(::clickhouse::Query(q.str()));
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "vector_clearing_results EnsureSchema failed",
                          {{"error", e.what()}});
  }
}

void ClickHouseVectorClearingStorage::SaveResult(
    const mv1::VectorClearingResult& r) {
  std::ostringstream diag;
  diag << "{\"iterations\":" << r.diagnostics().iterations()
       << ",\"solve_time_ms\":" << r.diagnostics().solve_time_ms()
       << ",\"solver_params_version\":\""
       << JsonEscape(r.diagnostics().solver_params_version()) << "\"}";

  std::ostringstream q;
  q << "INSERT INTO " << database_ << "." << table_ << " FORMAT JSONEachRow\n"
    << "{"
    << "\"batch_id\":\"" << JsonEscape(r.batch_id()) << "\","
    << "\"execution_group_id\":\"" << JsonEscape(r.execution_group_id()) << "\","
    << "\"asset_basis_json\":\"[]\","
    << "\"x_json\":\"" << JsonEscape(DecArr(r.x())) << "\","
    << "\"pi_json\":\"" << JsonEscape(DecArr(r.pi())) << "\","
    << "\"residual_json\":\"" << JsonEscape(DecArr(r.residual())) << "\","
    << "\"residual_norm\":" << r.diagnostics().residual_norm() << ","
    << "\"solver_status\":\"" << StatusStr(r.solver_status()) << "\","
    << "\"surplus_json\":\"" << JsonEscape(SurplusJson(r)) << "\","
    << "\"solver_diagnostics_json\":\"" << JsonEscape(diag.str()) << "\","
    << "\"leg_count\":" << r.x_size() << ","
    << "\"event_time_ms\":" << NowMs()
    << "}\n";

  try {
    client_.Execute(::clickhouse::Query(q.str()));
  } catch (const std::exception& e) {
    cex::common::log_json("WARN", "Failed to persist vector_clearing_results",
                          {{"batch_id", r.batch_id()}, {"error", e.what()}});
  }
}

}  // namespace cex::market_data::infra::clickhouse
