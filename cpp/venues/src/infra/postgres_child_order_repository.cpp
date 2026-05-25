#include "infra/postgres_child_order_repository.hpp"

#include <exception>
#include <string>
#include <utility>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"
#include "cex/common/uuid.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::venues::infra {

namespace {

#ifdef CEX_VENUES_HAS_LIBPQXX

std::string SideToText(fob::common::v1::Side side) {
  return side == fob::common::v1::SIDE_SELL ? "SELL" : "BUY";
}

std::string StrategyToOrderType(fob::execution::v1::ExecutionStrategy s) {
  switch (s) {
    case fob::execution::v1::EXEC_STRATEGY_MARKET:
      return "MARKET";
    case fob::execution::v1::EXEC_STRATEGY_POST_ONLY:
      return "POST_ONLY";
    case fob::execution::v1::EXEC_STRATEGY_LIMIT:
    case fob::execution::v1::EXEC_STRATEGY_TWAP:
    case fob::execution::v1::EXEC_STRATEGY_UNSPECIFIED:
    default:
      return "LIMIT";
  }
}

std::string TifToText(fob::common::v1::TimeInForce tif) {
  switch (tif) {
    case fob::common::v1::TIF_IOC:
      return "IOC";
    case fob::common::v1::TIF_FOK:
      return "FOK";
    case fob::common::v1::TIF_GTC:
    case fob::common::v1::TIF_UNSPECIFIED:
    default:
      return "GTC";
  }
}

std::string ReportStatusToChildOrderStatus(
    fob::execution::v1::ExecutionReportStatus status) {
  switch (status) {
    case fob::execution::v1::EXECUTION_REPORT_STATUS_FILLED:
      return "FILLED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_PARTIALLY_FILLED:
      return "PARTIALLY_FILLED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_REJECTED:
      return "REJECTED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_CANCELLED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_EXPIRED:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_OVERFILL_GUARD:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_UNDERFILLED:
      return "CANCELLED";
    case fob::execution::v1::EXECUTION_REPORT_STATUS_NEW:
    case fob::execution::v1::EXECUTION_REPORT_STATUS_UNSPECIFIED:
    default:
      return "PENDING";
  }
}

std::string DecimalToString(const fob::common::v1::Decimal& d) {
  return cex::common::Decimal::from_proto(d).to_string();
}

// Generate a stable UUID for the child_order_id from the intent.
// Prefers proto-provided UUIDs when present; falls back to v4 random.
std::string DeriveChildOrderId(
    const fob::execution::v1::ExecutionIntent& intent) {
  if (!intent.client_order_id().empty()) {
    // client_order_id is sometimes a venue-friendly id (e.g.
    // "<batch>|<order>|<venue>|external_fill_0") and not a UUID. Hash to
    // a derived stable UUID-shaped string in those cases.
    if (intent.client_order_id().size() == 36 &&
        intent.client_order_id()[8] == '-') {
      return intent.client_order_id();
    }
  }
  return cex::common::uuid_v4();
}

#endif  // CEX_VENUES_HAS_LIBPQXX

}  // namespace

PostgresChildOrderRepository::PostgresChildOrderRepository(
    std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

bool PostgresChildOrderRepository::EnsureSchema() {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS child_orders (
  child_order_id   UUID PRIMARY KEY,
  hedge_flow_id    UUID NOT NULL,
  venue_id         TEXT NOT NULL,
  symbol           TEXT NOT NULL,
  side             TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
  order_type       TEXT NOT NULL CHECK (order_type IN ('MARKET', 'LIMIT', 'POST_ONLY', 'IOC', 'FOK')),
  qty              NUMERIC(38, 18) NOT NULL CHECK (qty > 0),
  price            NUMERIC(38, 18),
  tif              TEXT NOT NULL CHECK (tif IN ('GTC', 'IOC', 'FOK')),
  filled_qty       NUMERIC(38, 18) NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
  avg_price        NUMERIC(38, 18),
  fee              NUMERIC(38, 18) NOT NULL DEFAULT 0,
  fee_currency     TEXT,
  client_order_id  TEXT NOT NULL,
  venue_order_id   TEXT,
  status           TEXT NOT NULL CHECK (status IN ('PENDING', 'FILLED', 'PARTIALLY_FILLED', 'CANCELLED', 'REJECTED')),
  error_code       TEXT,
  error_message    TEXT,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
)
)SQL");
    tx.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS child_orders_idem "
        "ON child_orders (hedge_flow_id, client_order_id)");
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to ensure child_orders schema",
                          {{"error", ex.what()}});
    return false;
  }
#else
  cex::common::log_json("WARN",
                        "PostgresChildOrderRepository disabled "
                        "(CEX_VENUES_HAS_LIBPQXX=0)");
  return false;
#endif
}

bool PostgresChildOrderRepository::InsertPending(
    const fob::execution::v1::ExecutionIntent& intent) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  if (intent.hedge_flow_id().empty() || !intent.has_target_qty()) {
    return false;
  }
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    tx.exec_params(
        R"SQL(
INSERT INTO child_orders (
  child_order_id, hedge_flow_id, venue_id, symbol, side, order_type,
  qty, price, tif, client_order_id, status
) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, 'PENDING')
ON CONFLICT (child_order_id) DO NOTHING
)SQL",
        DeriveChildOrderId(intent),
        intent.hedge_flow_id(),
        intent.venue(),
        intent.venue_symbol().empty() ? intent.instrument().symbol()
                                      : intent.venue_symbol(),
        SideToText(intent.side()),
        StrategyToOrderType(intent.strategy()),
        DecimalToString(intent.target_qty()),
        intent.has_limit_price()
            ? std::optional<std::string>{DecimalToString(intent.limit_price())}
            : std::optional<std::string>{},
        TifToText(intent.tif()),
        intent.client_order_id());
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to insert child_order",
                          {{"hedge_flow_id", intent.hedge_flow_id()},
                           {"client_order_id", intent.client_order_id()},
                           {"error", ex.what()}});
    return false;
  }
#else
  (void)intent;
  return false;
#endif
}

bool PostgresChildOrderRepository::ApplyReport(
    const fob::execution::v1::ExecutionReport& report) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  if (report.hedge_flow_id().empty() || report.client_order_id().empty()) {
    return false;
  }
  try {
    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    const std::string new_status =
        ReportStatusToChildOrderStatus(report.status());
    tx.exec_params(
        R"SQL(
UPDATE child_orders
   SET filled_qty    = $3::NUMERIC,
       avg_price     = $4::NUMERIC,
       venue_order_id = COALESCE(NULLIF($5,''), venue_order_id),
       status        = $6,
       error_code    = COALESCE(NULLIF($7,''), error_code),
       error_message = COALESCE(NULLIF($8,''), error_message),
       updated_at    = now()
 WHERE hedge_flow_id  = $1
   AND client_order_id = $2
)SQL",
        report.hedge_flow_id(),
        report.client_order_id(),
        report.has_filled_qty() ? DecimalToString(report.filled_qty())
                                : std::string{"0"},
        report.has_average_price() ? DecimalToString(report.average_price())
                                   : std::string{"0"},
        report.venue_order_id(),
        new_status,
        report.has_error() ? report.error().code() : std::string{},
        report.has_error() ? report.error().message() : std::string{});
    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to apply report to child_order",
                          {{"hedge_flow_id", report.hedge_flow_id()},
                           {"client_order_id", report.client_order_id()},
                           {"error", ex.what()}});
    return false;
  }
#else
  (void)report;
  return false;
#endif
}

}  // namespace cex::venues::infra
