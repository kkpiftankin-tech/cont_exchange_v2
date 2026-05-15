#include "infra/postgres/postgres_flow_order_repository.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "domain/fill_math.hpp"
#include "infra/postgres/decimal_conversion.hpp"

namespace cex::matching::infra {

namespace {

std::int64_t to_epoch_seconds(std::chrono::system_clock::time_point time_point) {
  return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_epoch_seconds(std::int64_t epoch_seconds) {
  return std::chrono::system_clock::time_point{std::chrono::seconds{epoch_seconds}};
}

std::string to_lower_ascii(std::string input) {
  std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return input;
}

domain::FlowOrderTimeInForce parse_time_in_force(const std::string& db_tif) {
  const auto tif = to_lower_ascii(db_tif);
  if (tif == "gtc") {
    return domain::FlowOrderTimeInForce::kGtc;
  }
  if (tif == "gtd") {
    return domain::FlowOrderTimeInForce::kGtd;
  }
  if (tif == "ioc") {
    return domain::FlowOrderTimeInForce::kIoc;
  }
  throw std::invalid_argument("Unsupported flow_orders.time_in_force value: " + db_tif);
}

domain::FlowOrderStatus parse_status(const std::string& db_status) {
  const auto status = to_lower_ascii(db_status);
  if (status == "new") {
    return domain::FlowOrderStatus::kNew;
  }
  if (status == "active") {
    return domain::FlowOrderStatus::kActive;
  }
  if (status == "partially_filled") {
    return domain::FlowOrderStatus::kPartiallyFilled;
  }
  if (status == "filled") {
    return domain::FlowOrderStatus::kFilled;
  }
  if (status == "cancelled") {
    return domain::FlowOrderStatus::kCancelled;
  }
  if (status == "expired") {
    return domain::FlowOrderStatus::kExpired;
  }
  if (status == "liquidated") {
    return domain::FlowOrderStatus::kLiquidated;
  }
  throw std::invalid_argument("Unsupported flow_orders.status value: " + db_status);
}

domain::FlowOrder map_flow_order(const pqxx::row& row) {
  domain::FlowOrder order;
  order.order_id = row["order_id"].as<std::string>();
  order.user_id = row["user_id"].as<std::string>();
  order.p_low = postgres::ParsePgNumeric(row["p_low"].as<std::string>());
  order.p_high = postgres::ParsePgNumeric(row["p_high"].as<std::string>());
  order.q_rate = postgres::ParsePgNumeric(row["q_rate"].as<std::string>());
  order.q_max = postgres::ParsePgNumeric(row["q_max"].as<std::string>());
  order.filled_cum = postgres::ParsePgNumeric(row["filled_cum"].as<std::string>());
  order.time_in_force = parse_time_in_force(row["time_in_force"].as<std::string>());
  order.status = parse_status(row["status"].as<std::string>());

  order.window_start = from_epoch_seconds(row["window_start_epoch"].as<std::int64_t>());
  if (row["window_end_epoch"].is_null()) {
    order.window_end = std::nullopt;
  } else {
    order.window_end = from_epoch_seconds(row["window_end_epoch"].as<std::int64_t>());
  }

  order.created_at = from_epoch_seconds(row["created_at_epoch"].as<std::int64_t>());
  order.updated_at = from_epoch_seconds(row["updated_at_epoch"].as<std::int64_t>());
  return order;
}

std::string build_load_active_orders_query(bool has_instrument_filter, bool has_limit) {
  std::string sql = R"SQL(
SELECT
  fo.order_id::text AS order_id,
  fo.user_id::text AS user_id,
  fo.p_low::text AS p_low,
  fo.p_high::text AS p_high,
  fo.q_rate::text AS q_rate,
  fo.q_max::text AS q_max,
  fo.filled_cum::text AS filled_cum,
  fo.time_in_force::text AS time_in_force,
  fo.status::text AS status,
  EXTRACT(EPOCH FROM fo.window_start)::bigint AS window_start_epoch,
  CASE
    WHEN fo.window_end IS NULL THEN NULL
    ELSE EXTRACT(EPOCH FROM fo.window_end)::bigint
  END AS window_end_epoch,
  EXTRACT(EPOCH FROM fo.created_at)::bigint AS created_at_epoch,
  EXTRACT(EPOCH FROM fo.updated_at)::bigint AS updated_at_epoch
FROM flow_orders fo
WHERE
  fo.status IN ('active', 'partially_filled')
  AND fo.filled_cum < fo.q_max
  AND fo.window_start <= to_timestamp($1::double precision)
  AND (fo.window_end IS NULL OR to_timestamp($1::double precision) < fo.window_end)
  -- IOC orders are excluded from periodic batch-clearing by design.
  AND fo.time_in_force <> 'IOC'
)SQL";

  if (has_instrument_filter) {
    sql += R"SQL(
  AND EXISTS (
    SELECT 1
    FROM flow_order_legs fol
    WHERE fol.order_id = fo.order_id
      AND fol.instrument_symbol = $2
  )
)SQL";
  }

  sql += " ORDER BY fo.window_start, fo.order_id";
  if (has_limit) {
    sql += has_instrument_filter ? " LIMIT $3" : " LIMIT $2";
  }
  return sql;
}

std::string build_load_legs_query(
    std::size_t order_ids_count) {
  std::string sql = R"SQL(
SELECT
  order_id::text AS order_id,
  instrument_symbol,
  weight::text AS weight
FROM flow_order_legs
WHERE order_id::text IN ()SQL";

  for (std::size_t i = 0; i < order_ids_count; ++i) {
    if (i > 0) {
      sql += ", ";
    }
    sql += "$";
    sql += std::to_string(i + 1);
  }

  sql += ") ORDER BY order_id, instrument_symbol";
  return sql;
}

void validate_loaded_order(const domain::FlowOrder& order) {
  if (!order.has_valid_legs()) {
    throw std::runtime_error("flow_orders has invalid legs for order_id=" + order.order_id);
  }
  if (!order.has_valid_fill_state()) {
    throw std::runtime_error("flow_orders has filled_cum > q_max for order_id=" + order.order_id);
  }
}

}  // namespace

PostgresFlowOrderRepository::PostgresFlowOrderRepository(std::string connection_string)
    : connection_factory_([conn_str = std::move(connection_string)]() {
        return std::make_unique<pqxx::connection>(conn_str);
      }) {}

PostgresFlowOrderRepository::PostgresFlowOrderRepository(ConnectionFactory connection_factory)
    : connection_factory_(std::move(connection_factory)) {
  if (!connection_factory_) {
    throw std::invalid_argument("PostgresFlowOrderRepository requires a valid connection factory");
  }
}

std::vector<domain::FlowOrder> PostgresFlowOrderRepository::LoadActiveFlowOrders(
    std::chrono::system_clock::time_point as_of_time,
    const std::optional<std::string>& instrument_filter,
    const std::optional<std::int32_t>& limit) {
  if (limit.has_value() && *limit <= 0) {
    throw std::invalid_argument("LoadActiveFlowOrders limit must be positive");
  }
  if (instrument_filter.has_value() && instrument_filter->empty()) {
    throw std::invalid_argument("LoadActiveFlowOrders instrument_filter must not be empty");
  }

  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }

  const auto as_of_epoch = to_epoch_seconds(as_of_time);
  pqxx::work tx(*conn);
  const std::string sql =
      build_load_active_orders_query(instrument_filter.has_value(), limit.has_value());

  pqxx::result rows;
  if (instrument_filter.has_value() && limit.has_value()) {
    rows = tx.exec_params(sql, as_of_epoch, *instrument_filter, *limit);
  } else if (instrument_filter.has_value()) {
    rows = tx.exec_params(sql, as_of_epoch, *instrument_filter);
  } else if (limit.has_value()) {
    rows = tx.exec_params(sql, as_of_epoch, *limit);
  } else {
    rows = tx.exec_params(sql, as_of_epoch);
  }

  std::vector<domain::FlowOrder> orders;
  orders.reserve(rows.size());
  std::vector<std::string> order_ids;
  order_ids.reserve(rows.size());
  std::unordered_map<std::string, std::size_t> order_index_by_id;
  order_index_by_id.reserve(rows.size());

  for (const auto& row : rows) {
    orders.push_back(map_flow_order(row));
    order_ids.push_back(orders.back().order_id);
    order_index_by_id.emplace(orders.back().order_id, orders.size() - 1);
  }

  if (!order_ids.empty()) {
    const auto leg_rows = tx.exec_params(
        build_load_legs_query(order_ids.size()),
        pqxx::prepare::make_dynamic_params(order_ids.begin(), order_ids.end()));
    for (const auto& row : leg_rows) {
      const auto order_id = row["order_id"].as<std::string>();
      const auto index_it = order_index_by_id.find(order_id);
      if (index_it == order_index_by_id.end()) {
        throw std::runtime_error("flow_order_legs contains unknown order_id=" + order_id);
      }

      domain::FlowOrderLeg leg;
      leg.instrument_symbol = row["instrument_symbol"].as<std::string>();
      leg.weight = postgres::ParsePgNumeric(row["weight"].as<std::string>());
      orders[index_it->second].legs.push_back(std::move(leg));
    }
  }

  for (auto& order : orders) {
    order.sort_legs_by_symbol();
    validate_loaded_order(order);
  }

  tx.commit();
  return orders;
}

void PostgresFlowOrderRepository::UpdateFilledVolumes(
    const std::vector<domain::OrderFillDelta>& deltas,
    std::chrono::system_clock::time_point batch_time) {
  if (deltas.empty()) {
    return;
  }

  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }

  const auto batch_epoch = to_epoch_seconds(batch_time);
  pqxx::work tx(*conn);

  for (const auto& delta : deltas) {
    if (delta.order_id.empty()) {
      throw std::invalid_argument("OrderFillDelta.order_id must not be empty");
    }

    const cex::common::Decimal zero{0, delta.executed_qty_delta.scale};
    if (cex::common::Decimal::cmp(delta.executed_qty_delta, zero) <= 0) {
      continue;
    }

    const auto locked_rows = tx.exec_params(R"SQL(
SELECT
  q_max::text AS q_max,
  filled_cum::text AS filled_cum
FROM flow_orders
WHERE order_id = $1
FOR UPDATE
)SQL",
                                            delta.order_id);

    if (locked_rows.empty()) {
      throw std::runtime_error("flow_orders row not found for order_id=" + delta.order_id);
    }

    const auto& row = locked_rows.front();
    const auto q_max = postgres::ParsePgNumeric(row["q_max"].as<std::string>());
    const auto current_filled = postgres::ParsePgNumeric(row["filled_cum"].as<std::string>());

    const auto update = domain::ComputeFilledVolumeUpdate(
        q_max, current_filled, delta.executed_qty_delta);

    if (update.new_status == domain::FlowOrderStatus::kFilled) {
      tx.exec_params(R"SQL(
UPDATE flow_orders
SET
  filled_cum = $1::numeric,
  status = 'filled',
  updated_at = to_timestamp($2::double precision)
WHERE order_id = $3
)SQL",
                     postgres::ToPgNumeric(update.new_filled_cum),
                     batch_epoch,
                     delta.order_id);
    } else {
      tx.exec_params(R"SQL(
UPDATE flow_orders
SET
  filled_cum = $1::numeric,
  status = 'partially_filled',
  updated_at = to_timestamp($2::double precision)
WHERE order_id = $3
)SQL",
                     postgres::ToPgNumeric(update.new_filled_cum),
                     batch_epoch,
                     delta.order_id);
    }
  }

  tx.commit();
}

}  // namespace cex::matching::infra
