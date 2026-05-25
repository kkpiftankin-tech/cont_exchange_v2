#include "infra/postgres_hedgeflow_repository.hpp"

#include <exception>
#include <string>
#include <utility>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::venues::infra {

namespace {

#ifdef CEX_VENUES_HAS_LIBPQXX

// Map proto Side to "BUY"/"SELL" text matching schema CHECK constraint.
std::string SideToText(fob::common::v1::Side side) {
  switch (side) {
    case fob::common::v1::SIDE_BUY:
      return "BUY";
    case fob::common::v1::SIDE_SELL:
      return "SELL";
    default:
      return "BUY";  // defensive default; schema CHECK will fail loudly
  }
}

std::string UrgencyToText(fob::execution::v1::ExecutionUrgency urgency) {
  switch (urgency) {
    case fob::execution::v1::URGENCY_LOW:
      return "LOW";
    case fob::execution::v1::URGENCY_HIGH:
      return "HIGH";
    case fob::execution::v1::URGENCY_MEDIUM:
    default:
      return "MEDIUM";
  }
}

// Map ExecutionReportStatus to hedgeflows.status.
// Aggregation rule (PR-F12-3a, MVP): if any single ExecutionReport carries
// a terminal status we propagate it to the HedgeFlow level. This is
// simplistic — multi-venue routing in PR-F12-5 will need a proper
// aggregator that waits for all child_orders to settle before flipping
// hedgeflow.status.
std::string ReportStatusToHedgeFlowStatus(
    fob::execution::v1::ExecutionReportStatus status) {
  switch (status) {
    case fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED:
      return "COMPLETED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED:
      return "OPEN";  // still in progress
    case fob::execution::v1::EXECUTION_REPORT_STATUS_UNDERFILLED:
      return "UNDERFILLED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED:
      return "REJECTED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED:
      return "CANCELLED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_OVERFILL_GUARD:
      return "COMPLETED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_NEW:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED:
    default:
      return "OPEN";
  }
}

bool IsTerminalStatus(const std::string& s) {
  return s == "COMPLETED" || s == "UNDERFILLED" || s == "REJECTED" ||
         s == "CANCELLED" || s == "RISK_REJECTED";
}

std::string DecimalToString(const fob::common::v1::Decimal& d) {
  return cex::common::Decimal::from_proto(d).to_string();
}

#endif  // CEX_VENUES_HAS_LIBPQXX

}  // namespace

PostgresHedgeflowRepository::PostgresHedgeflowRepository(
    std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

bool PostgresHedgeflowRepository::EnsureSchema() {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    // Mirror of infra/postgres/init.sql DDL — keeps the C++ side
    // self-healing if init.sql wasn't applied.
    tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS hedgeflows (
  hedge_flow_id    UUID PRIMARY KEY,
  intent_id        UUID NOT NULL,
  batch_id         TEXT,
  provider_id      TEXT NOT NULL,
  symbol           TEXT NOT NULL,
  side             TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
  target_qty       NUMERIC(38, 18) NOT NULL CHECK (target_qty > 0),
  filled_qty       NUMERIC(38, 18) NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
  target_notional  NUMERIC(38, 18),
  reference_mid    NUMERIC(38, 18),
  avg_fill_price   NUMERIC(38, 18),
  tot_fee          NUMERIC(38, 18) NOT NULL DEFAULT 0,
  hedge_pnl        NUMERIC(38, 18),
  urgency          TEXT NOT NULL CHECK (urgency IN ('LOW', 'MEDIUM', 'HIGH')),
  timeout_ms       INTEGER NOT NULL CHECK (timeout_ms > 0),
  status           TEXT NOT NULL CHECK (status IN ('OPEN', 'COMPLETED', 'UNDERFILLED', 'REJECTED', 'RISK_REJECTED', 'CANCELLED')),
  error_code       TEXT,
  error_message    TEXT,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  completed_at     TIMESTAMPTZ
)
)SQL");
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to ensure hedgeflows schema",
                          {{"error", ex.what()}});
    return false;
  }
#else
  cex::common::log_json("WARN",
                        "PostgresHedgeflowRepository disabled "
                        "(CEX_VENUES_HAS_LIBPQXX=0)");
  return false;
#endif
}

bool PostgresHedgeflowRepository::InsertOpen(
    const fob::execution::v1::ExecutionIntent& intent) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  if (intent.hedge_flow_id().empty() || intent.intent_id().empty() ||
      !intent.has_target_qty()) {
    cex::common::log_json("WARN",
                          "Skipping hedgeflow insert: incomplete intent",
                          {{"intent_id", intent.intent_id()},
                           {"hedge_flow_id", intent.hedge_flow_id()}});
    return false;
  }
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec_params(
        R"SQL(
INSERT INTO hedgeflows (
  hedge_flow_id, intent_id, batch_id, provider_id,
  symbol, side, target_qty, target_notional, reference_mid,
  urgency, timeout_ms, status
) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, 'OPEN')
ON CONFLICT (hedge_flow_id) DO NOTHING
)SQL",
        intent.hedge_flow_id(),
        intent.intent_id(),
        intent.batch_id().empty() ? std::optional<std::string>{}
                                  : std::optional<std::string>{intent.batch_id()},
        intent.provider_id(),
        intent.instrument().symbol(),
        SideToText(intent.side()),
        DecimalToString(intent.target_qty()),
        intent.has_target_notional()
            ? std::optional<std::string>{DecimalToString(intent.target_notional())}
            : std::optional<std::string>{},
        intent.has_reference_mid()
            ? std::optional<std::string>{DecimalToString(intent.reference_mid())}
            : std::optional<std::string>{},
        UrgencyToText(intent.urgency()),
        intent.timeout_ms() > 0 ? intent.timeout_ms() : 30000);
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to insert hedgeflow",
                          {{"hedge_flow_id", intent.hedge_flow_id()},
                           {"error", ex.what()}});
    return false;
  }
#else
  (void)intent;
  return false;
#endif
}

bool PostgresHedgeflowRepository::ApplyReport(
    const fob::execution::v1::ExecutionReport& report) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  if (report.hedge_flow_id().empty()) {
    return false;
  }
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    const std::string new_status = ReportStatusToHedgeFlowStatus(report.status());
    const bool terminal = IsTerminalStatus(new_status);
    const std::string filled_qty_str =
        report.has_filled_qty() ? DecimalToString(report.filled_qty()) : "0";
    const std::string avg_price_str =
        report.has_average_price() ? DecimalToString(report.average_price())
                                   : std::string{"0"};

    // Recompute weighted avg_fill_price on the fly. Note: this approximates
    // when filled_qty=0 (uses incoming avg). Sufficient for MVP; a proper
    // weighted accumulator across multiple reports lands in PR-F12-5.
    tx.exec_params(
        R"SQL(
UPDATE hedgeflows
   SET filled_qty     = filled_qty + $2::NUMERIC,
       avg_fill_price = CASE
         WHEN (filled_qty + $2::NUMERIC) > 0 THEN
           ((COALESCE(avg_fill_price, 0) * filled_qty) + ($3::NUMERIC * $2::NUMERIC))
           / NULLIF(filled_qty + $2::NUMERIC, 0)
         ELSE avg_fill_price
       END,
       status         = CASE
         WHEN status IN ('COMPLETED','UNDERFILLED','REJECTED','CANCELLED','RISK_REJECTED')
           THEN status                                    -- already terminal, do not override
         ELSE $4
       END,
       error_code     = COALESCE(NULLIF($5,''), error_code),
       error_message  = COALESCE(NULLIF($6,''), error_message),
       updated_at     = now(),
       completed_at   = CASE WHEN $7 THEN now() ELSE completed_at END
 WHERE hedge_flow_id  = $1
)SQL",
        report.hedge_flow_id(),
        filled_qty_str,
        avg_price_str,
        new_status,
        report.has_error() ? report.error().code() : std::string{},
        report.has_error() ? report.error().message() : std::string{},
        terminal);
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to apply report to hedgeflow",
                          {{"hedge_flow_id", report.hedge_flow_id()},
                           {"error", ex.what()}});
    return false;
  }
#else
  (void)report;
  return false;
#endif
}

}  // namespace cex::venues::infra
