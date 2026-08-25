#include "infra/postgres_synthetic_order_repository.hpp"

#include <exception>
#include <utility>

#include "cex/common/decimal.hpp"
#include "cex/common/log.hpp"

#ifdef CEX_VENUES_HAS_LIBPQXX
#include <pqxx/pqxx>
#endif

namespace cex::venues::infra {

namespace {

#ifdef CEX_VENUES_HAS_LIBPQXX

std::string DecimalToPgNumeric(const fob::common::v1::Decimal& value) {
  return cex::common::Decimal::from_proto(value).to_string();
}

double TimestampToEpochSeconds(const google::protobuf::Timestamp& ts) {
  return static_cast<double>(ts.seconds()) +
         static_cast<double>(ts.nanos()) / 1.0e9;
}

std::string SideToDb(const fob::common::v1::Side side) {
  if (side == fob::common::v1::SIDE_BUY) return "buy";
  if (side == fob::common::v1::SIDE_SELL) return "sell";
  return "";
}

void EnsureSyntheticOrdersTable(pqxx::work& tx) {
  tx.exec(R"SQL(
CREATE TABLE IF NOT EXISTS synthetic_orders (
  syntheticid UUID PRIMARY KEY,
  venueid VARCHAR(32) NOT NULL,
  symbol VARCHAR(32) NOT NULL,
  side TEXT NOT NULL CHECK (side IN ('buy', 'sell')),
  pl NUMERIC(24,8) NOT NULL,
  ph NUMERIC(24,8) NOT NULL,
  qrate NUMERIC(24,8) NOT NULL,
  qmax NUMERIC(24,8) NOT NULL,
  curveid UUID NOT NULL,
  snapshotid UUID,
  createdat TIMESTAMPTZ NOT NULL,
  expiresat TIMESTAMPTZ NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('active', 'expired', 'used'))
)
)SQL");
}

#endif

}  // namespace

PostgresSyntheticOrderRepository::PostgresSyntheticOrderRepository(
    std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

bool PostgresSyntheticOrderRepository::SaveSyntheticOrder(
    const fob::orders::v1::SyntheticFlowOrder& order) {
#ifdef CEX_VENUES_HAS_LIBPQXX
  try {
    const std::string side = SideToDb(order.side());
    if (side.empty()) {
      cex::common::log_json("ERROR", "Invalid SyntheticFlowOrder side",
                            {{"synthetic_id", order.synthetic_id()},
                             {"venue", order.venue_id()}});
      return false;
    }

    pqxx::connection connection(connection_string_);
    pqxx::work tx(connection);
    EnsureSyntheticOrdersTable(tx);

    tx.exec_params(R"SQL(
INSERT INTO synthetic_orders (
  syntheticid,
  venueid,
  symbol,
  side,
  pl,
  ph,
  qrate,
  qmax,
  curveid,
  snapshotid,
  createdat,
  expiresat,
  status
) VALUES (
  $1::uuid,
  $2,
  $3,
  $4,
  $5::numeric,
  $6::numeric,
  $7::numeric,
  $8::numeric,
  $9::uuid,
  NULLIF($10, '')::uuid,
  to_timestamp($11::double precision),
  to_timestamp($12::double precision),
  $13
)
ON CONFLICT (syntheticid) DO UPDATE SET
  venueid = EXCLUDED.venueid,
  symbol = EXCLUDED.symbol,
  side = EXCLUDED.side,
  pl = EXCLUDED.pl,
  ph = EXCLUDED.ph,
  qrate = EXCLUDED.qrate,
  qmax = EXCLUDED.qmax,
  curveid = EXCLUDED.curveid,
  snapshotid = EXCLUDED.snapshotid,
  createdat = EXCLUDED.createdat,
  expiresat = EXCLUDED.expiresat,
  status = EXCLUDED.status
)SQL",
                   order.synthetic_id(),
                   order.venue_id(),
                   order.instrument().symbol(),
                   side,
                   DecimalToPgNumeric(order.p_l()),
                   DecimalToPgNumeric(order.p_h()),
                   DecimalToPgNumeric(order.q_rate()),
                   DecimalToPgNumeric(order.q_max()),
                   order.curve_id(),
                   order.snapshot_id(),
                   TimestampToEpochSeconds(order.created_at()),
                   TimestampToEpochSeconds(order.expires_at()),
                   order.status().empty() ? "active" : order.status());

    tx.commit();
    return true;
  } catch (const std::exception& ex) {
    cex::common::log_json("ERROR", "Failed to save synthetic_orders row",
                          {{"synthetic_id", order.synthetic_id()},
                           {"venue", order.venue_id()},
                           {"symbol", order.instrument().symbol()},
                           {"error", ex.what()}});
    return false;
  }
#else
  (void)order;
  static bool warned = false;
  if (!warned) {
    warned = true;
    cex::common::log_json(
        "WARN",
        "PostgresSyntheticOrderRepository is disabled: build without libpqxx "
        "(CEX_VENUES_HAS_LIBPQXX=0)");
  }
  return false;
#endif
}

}  // namespace cex::venues::infra
