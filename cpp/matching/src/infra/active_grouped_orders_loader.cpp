// ============================================================================
// active_grouped_orders_loader.cpp — F-09 (T-F09-048). См. .hpp.
// ============================================================================

#include "infra/active_grouped_orders_loader.hpp"

#include <stdexcept>

#include "cex/common/decimal.hpp"
#include "infra/postgres/decimal_conversion.hpp"

namespace cex::matching::infra {

namespace {

namespace d = cex::matching::domain;
using cex::common::Decimal;
using cex::matching::infra::postgres::ParsePgNumeric;

d::GroupAtomicityPolicy PolicyFromDb(const std::string& s) {
  if (s == "strict_atomic") return d::GroupAtomicityPolicy::kStrictAtomic;
  if (s == "scalable_atomic") return d::GroupAtomicityPolicy::kScalableAtomic;
  if (s == "sequential_fallback") return d::GroupAtomicityPolicy::kSequentialFallback;
  if (s == "external_compensating") return d::GroupAtomicityPolicy::kExternalCompensating;
  return d::GroupAtomicityPolicy::kBestEffort;
}

d::GroupFallbackPolicy FallbackFromDb(const std::string& s) {
  if (s == "wait_next_batch" || s == "skip_batch") return d::GroupFallbackPolicy::kWaitNextBatch;
  if (s == "cancel" || s == "cancel_group") return d::GroupFallbackPolicy::kCancel;
  if (s == "degrade") return d::GroupFallbackPolicy::kDegrade;
  if (s == "compensate") return d::GroupFallbackPolicy::kCompensate;
  return d::GroupFallbackPolicy::kScaleDown;
}

// Decimal из nullable NUMERIC-поля (NULL → zero).
Decimal NumOrZero(const pqxx::field& f) {
  return f.is_null() ? Decimal::zero() : ParsePgNumeric(f.as<std::string>());
}

}  // namespace

PostgresActiveGroupsLoader::PostgresActiveGroupsLoader(std::string dsn)
    : connection_factory_([dsn = std::move(dsn)]() {
        return std::make_unique<pqxx::connection>(dsn);
      }) {}

PostgresActiveGroupsLoader::PostgresActiveGroupsLoader(ConnectionFactory factory)
    : connection_factory_(std::move(factory)) {}

std::vector<d::MultiLegVectorOrder> PostgresActiveGroupsLoader::LoadActiveGroups() {
  auto conn = connection_factory_();
  if (!conn || !conn->is_open()) {
    throw std::runtime_error("Failed to open PostgreSQL connection");
  }
  pqxx::work tx(*conn);

  // Активные группы grouped-режима (orchestration_only исключён — он идёт как
  // независимые FlowOrder через orders.normalized).
  const pqxx::result groups = tx.exec(R"SQL(
SELECT combo_order_id::text, user_id, atomicity_policy, fallback_policy, ratio_basis,
       min_execution_scale, max_ratio_deviation_bps
FROM combo_orders
WHERE execution_mode = 'multileg_vector_solver'
  AND status IN ('active','partially_filled')
ORDER BY combo_order_id
)SQL");

  std::vector<d::MultiLegVectorOrder> result;
  result.reserve(groups.size());

  for (const auto& g : groups) {
    d::MultiLegVectorOrder order;
    order.parent_order_id = g["combo_order_id"].as<std::string>();
    order.user_id = g["user_id"].is_null() ? std::string() : g["user_id"].as<std::string>();
    order.atomicity_policy = PolicyFromDb(g["atomicity_policy"].as<std::string>());
    order.fallback_policy = FallbackFromDb(g["fallback_policy"].as<std::string>());
    order.min_execution_scale = NumOrZero(g["min_execution_scale"]);
    order.max_ratio_deviation_bps =
        g["max_ratio_deviation_bps"].is_null() ? 0 : g["max_ratio_deviation_bps"].as<int>();
    // ratio_basis в matching-домене не моделируется отдельно: target_ratio уже
    // знаковый. Строка нужна лишь чтобы выбрать weight vs ratio при сборке ног.
    const std::string ratio_basis = g["ratio_basis"].as<std::string>();

    const pqxx::result legs = tx.exec_params(R"SQL(
SELECT leg_id::text, instrument_symbol, side, ratio, weight,
       p_low, p_high, q_rate, q_max, filled_cum
FROM combo_order_legs
WHERE parent_order_id = $1::uuid
ORDER BY leg_id
)SQL",
                                             order.parent_order_id);

    for (const auto& l : legs) {
      d::VectorLeg leg;
      leg.leg_id = l["leg_id"].as<std::string>();
      leg.instrument_symbol = l["instrument_symbol"].as<std::string>();
      // target_ratio: по notional_weight — weight, иначе ratio; знак из side.
      const Decimal magnitude =
          ratio_basis == "quantity" ? NumOrZero(l["ratio"]) : NumOrZero(l["weight"]);
      const bool is_sell = l["side"].as<std::string>() == "sell";
      leg.target_ratio = is_sell ? Decimal::sub(Decimal::zero(), magnitude) : magnitude;
      leg.p_low = NumOrZero(l["p_low"]);
      leg.p_high = NumOrZero(l["p_high"]);
      leg.q_rate = NumOrZero(l["q_rate"]);
      leg.q_max = NumOrZero(l["q_max"]);
      leg.filled_cum = NumOrZero(l["filled_cum"]);
      order.legs.push_back(std::move(leg));
    }

    result.push_back(std::move(order));
  }

  tx.commit();
  return result;
}

}  // namespace cex::matching::infra
