#include "infra/postgres/postgres_flow_order_repository.hpp"

#include <stdexcept>
#include <utility>

#include "cex/common/decimal.hpp"
#include "fob/common/v1/common.pb.h"

namespace cex::order_flow::infra {

namespace {

// Map proto TimeInForce -> flow_orders.time_in_force ('GTC'|'GTD'|'IOC').
// Matching's batch SELECT excludes IOC explicitly; everything else is
// included. Unspecified/FOK fall back to GTC (the most permissive value)
// because batch clearing is the only execution path right now.
std::string time_in_force_to_db(fob::common::v1::TimeInForce tif) {
  switch (tif) {
    case fob::common::v1::TIF_IOC:
      return "IOC";
    case fob::common::v1::TIF_GTC:
    case fob::common::v1::TIF_FOK:
    case fob::common::v1::TIF_UNSPECIFIED:
    default:
      return "GTC";
  }
}

// flow_order_legs.weight convention from matching domain:
//   BUY  ->  +1
//   SELL ->  -1
// See cpp/matching/src/domain/flow_order.hpp:116.
std::string side_to_leg_weight(fob::common::v1::Side side) {
  if (side == fob::common::v1::SIDE_SELL) return "-1";
  return "1";
}

std::string decimal_to_pg(const fob::common::v1::Decimal& proto_decimal) {
  return cex::common::Decimal::from_proto(proto_decimal).to_string();
}

}  // namespace

PostgresFlowOrderRepository::PostgresFlowOrderRepository(std::string connection_string)
    : connection_factory_([conn_str = std::move(connection_string)]() {
        return std::make_unique<pqxx::connection>(conn_str);
      }) {}

PostgresFlowOrderRepository::PostgresFlowOrderRepository(ConnectionFactory connection_factory)
    : connection_factory_(std::move(connection_factory)) {
  if (!connection_factory_) {
    throw std::invalid_argument(
        "PostgresFlowOrderRepository requires a valid connection factory");
  }
}

void PostgresFlowOrderRepository::InsertFlowOrder(
    const fob::orders::v1::FlowOrder& order) {
  if (order.order_id().empty()) {
    throw std::invalid_argument("InsertFlowOrder: order_id is empty");
  }
  if (order.user_id().empty()) {
    throw std::invalid_argument("InsertFlowOrder: user_id is empty");
  }
  if (order.instrument().symbol().empty()) {
    throw std::invalid_argument("InsertFlowOrder: instrument.symbol is empty");
  }

  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }

  pqxx::work tx(*conn);

  // Status = 'active': matching's PostgresFlowOrderRepository::LoadActive...
  // filters status IN ('active','partially_filled'). Writing 'new' here would
  // make accepted orders invisible to F-04 batch clearing (the bug fixed by
  // this commit).
  tx.exec_params(R"SQL(
INSERT INTO flow_orders (
  order_id, user_id, p_low, p_high, q_rate, q_max,
  filled_cum, time_in_force, status,
  window_start, window_end
) VALUES (
  $1::uuid, $2, $3::numeric, $4::numeric, $5::numeric, $6::numeric,
  0, $7, 'active',
  NOW(), NULL
) ON CONFLICT (order_id) DO NOTHING
)SQL",
                 order.order_id(),
                 order.user_id(),
                 decimal_to_pg(order.price_low()),
                 decimal_to_pg(order.price_high()),
                 decimal_to_pg(order.max_speed()),
                 decimal_to_pg(order.total_qty()),
                 time_in_force_to_db(order.tif()));

  tx.exec_params(R"SQL(
INSERT INTO flow_order_legs (order_id, instrument_symbol, weight)
VALUES ($1::uuid, $2, $3::numeric)
ON CONFLICT (order_id, instrument_symbol) DO NOTHING
)SQL",
                 order.order_id(),
                 order.instrument().symbol(),
                 side_to_leg_weight(order.side()));

  tx.commit();
}

}  // namespace cex::order_flow::infra
