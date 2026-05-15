#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cex/common/decimal.hpp"
#include "domain/flow_order.hpp"
#include "domain/solver_config.hpp"
#include "domain/solver_impl.hpp"
#include "fob/common/v1/common.pb.h"
#include "fob/orders/v1/orders.pb.h"

namespace {

using cex::common::Decimal;
using cex::matching::domain::ContinuousClearingSolver;
using cex::matching::domain::FlowOrder;
using cex::matching::domain::FlowOrderLeg;
using cex::matching::domain::FlowOrderStatus;
using cex::matching::domain::FlowOrderTimeInForce;

bool Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

bool ExpectNear(const double actual, const double expected, const double eps,
                const std::string& message) {
  if (std::abs(actual - expected) > eps) {
    std::cerr << "FAILED: " << message << " (actual=" << actual
              << ", expected=" << expected << ", eps=" << eps << ")\n";
    return false;
  }
  return true;
}

fob::common::v1::Decimal DecProto(const int64_t units,
                                  const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

Decimal Dec(const int64_t units, const int32_t scale = 0) {
  return Decimal{units, scale};
}

double AsDouble(const fob::common::v1::Decimal& value) {
  return static_cast<double>(Decimal::from_proto(value));
}

FlowOrder MakeSingleAssetOrder(const std::string& order_id,
                               const std::string& symbol,
                               const fob::common::v1::Side side,
                               const int64_t q_max,
                               const int64_t remaining_qty,
                               const int64_t q_rate,
                               const int64_t p_low,
                               const int64_t p_high) {
  fob::orders::v1::FlowOrder proto;
  proto.set_order_id(order_id);
  proto.set_user_id("u-" + order_id);
  proto.mutable_instrument()->set_symbol(symbol);
  proto.set_side(side);
  *proto.mutable_total_qty() = DecProto(q_max);
  *proto.mutable_remaining_qty() = DecProto(remaining_qty);
  *proto.mutable_max_speed() = DecProto(q_rate);
  *proto.mutable_price_low() = DecProto(p_low);
  *proto.mutable_price_high() = DecProto(p_high);
  proto.set_status(fob::common::v1::ORDER_STATUS_NEW);
  return FlowOrder::from_proto(std::move(proto));
}

FlowOrder MakePortfolioOrder(
    const std::string& order_id,
    const std::vector<std::pair<std::string, int64_t>>& legs,
    const int64_t p_low,
    const int64_t p_high,
    const int64_t q_rate,
    const int64_t q_max) {
  FlowOrder order;
  order.order_id = order_id;
  order.user_id = "u-" + order_id;
  for (const auto& [symbol, weight] : legs) {
    order.legs.push_back(FlowOrderLeg{
        .instrument_symbol = symbol,
        .weight = Dec(weight),
    });
  }
  order.sort_legs_by_symbol();
  order.p_low = Dec(p_low);
  order.p_high = Dec(p_high);
  order.q_rate = Dec(q_rate);
  order.q_max = Dec(q_max);
  order.filled_cum = Dec(0);
  order.time_in_force = FlowOrderTimeInForce::kGtc;
  order.status = FlowOrderStatus::kActive;
  order.created_at = std::chrono::system_clock::now();
  order.updated_at = order.created_at;
  order.window_start = order.created_at;
  order.window_end = std::nullopt;
  return order;
}

double ExecutedRate(const fob::matching::v1::BatchResult& batch,
                    const std::string& order_id) {
  const auto it = batch.executed_rates().find(order_id);
  if (it == batch.executed_rates().end()) return 0.0;
  return AsDouble(it->second);
}

double SignedRate(const fob::matching::v1::BatchResult& batch,
                  const std::string& order_id,
                  const fob::common::v1::Side side) {
  const double rate = ExecutedRate(batch, order_id);
  return side == fob::common::v1::SIDE_SELL ? -rate : rate;
}

double QtyByOrderSymbolSide(const fob::matching::v1::BatchResult& batch,
                            const std::string& order_id,
                            const std::string& symbol,
                            const fob::common::v1::Side side) {
  double total = 0.0;
  for (const auto& fill : batch.fills()) {
    if (fill.order_id() == order_id &&
        fill.instrument().symbol() == symbol &&
        fill.side() == side) {
      total += AsDouble(fill.executed_qty());
    }
  }
  return total;
}

double QtyByOrder(const fob::matching::v1::BatchResult& batch,
                  const std::string& order_id) {
  double total = 0.0;
  for (const auto& fill : batch.fills()) {
    if (fill.order_id() == order_id) {
      total += AsDouble(fill.executed_qty());
    }
  }
  return total;
}

double NetQtyBySymbol(const fob::matching::v1::BatchResult& batch,
                      const std::string& symbol) {
  double net = 0.0;
  for (const auto& fill : batch.fills()) {
    if (fill.instrument().symbol() != symbol) continue;
    const double qty = AsDouble(fill.executed_qty());
    if (fill.side() == fob::common::v1::SIDE_BUY) {
      net += qty;
    } else if (fill.side() == fob::common::v1::SIDE_SELL) {
      net -= qty;
    }
  }
  return net;
}

fob::common::v1::OrderStatus StatusForOrder(
    const fob::matching::v1::BatchResult& batch,
    const std::string& order_id) {
  for (const auto& update : batch.order_updates()) {
    if (update.order_id() == order_id) return update.status();
  }
  return fob::common::v1::ORDER_STATUS_UNSPECIFIED;
}

std::unordered_map<std::string, fob::common::v1::Decimal> RefPrices(
    const std::initializer_list<std::pair<std::string, int64_t>>& refs) {
  std::unordered_map<std::string, fob::common::v1::Decimal> out;
  for (const auto& [symbol, px] : refs) out.emplace(symbol, DecProto(px));
  return out;
}

fob::venue::v1::VenueLiquidityCurve MakeExternalCurve(const std::string& venue_id,
                                                      const std::string& symbol) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id(venue_id);
  curve.set_snapshot_id("snapshot-" + venue_id);
  curve.set_curve_id("curve-" + venue_id);
  curve.mutable_instrument()->set_symbol(symbol);

  auto* ask = curve.mutable_ask_curve();
  ask->add_q_grid(1.0);
  ask->add_q_grid(2.0);
  ask->add_p_of_q(101.0);
  ask->add_p_of_q(102.0);

  auto* bid = curve.mutable_bid_curve();
  bid->add_q_grid(1.0);
  bid->add_q_grid(2.0);
  bid->add_p_of_q(99.0);
  bid->add_p_of_q(98.0);

  return curve;
}

cex::matching::domain::SolverConfig MakeConfig(const int version,
                                               const int batch_interval_ms,
                                               const size_t max_iterations,
                                               const double tolerance) {
  return cex::matching::domain::SolverConfig{
      .version = version,
      .batch_interval = std::chrono::milliseconds(batch_interval_ms),
      .max_iterations = max_iterations,
      .epsilon_liquidity = 1e-9,
      .tolerance = tolerance,
  };
}

struct LinearSingleAssetExpectation {
  double x{0.0};
  double clear_price{0.0};
};

LinearSingleAssetExpectation ExpectedLinearTwoOrderPair(const FlowOrder& buy,
                                                        const FlowOrder& sell) {
  const double d_buy =
      static_cast<double>(Decimal::sub(buy.p_high, buy.p_low)) /
      static_cast<double>(buy.q_rate);
  const double d_sell =
      static_cast<double>(Decimal::sub(sell.p_high, sell.p_low)) /
      static_cast<double>(sell.q_rate);

  const double qh_buy =
      std::min(static_cast<double>(buy.q_rate), static_cast<double>(buy.remaining_qty()));
  const double qh_sell =
      std::min(static_cast<double>(sell.q_rate), static_cast<double>(sell.remaining_qty()));
  const double cap = std::min(qh_buy, qh_sell);

  const double denom = d_buy + d_sell;
  double x = 0.0;
  if (std::isfinite(denom) && std::abs(denom) > 1e-12) {
    const double p_sum = static_cast<double>(Decimal::add(buy.p_high, sell.p_high));
    x = p_sum / denom;
  }
  x = std::clamp(x, 0.0, std::max(0.0, cap));

  const double clear_price = static_cast<double>(buy.p_high) - d_buy * x;
  return {.x = x, .clear_price = clear_price};
}

bool test_u1_symmetric_buy_sell() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(1, 1000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u1_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 99, 101),
      MakeSingleAssetOrder("u1_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 99, 101),
  };

  const auto batch = solver.Solve(orders, RefPrices({{"BTC/USDT", 100}}));
  const auto expected = ExpectedLinearTwoOrderPair(orders[0], orders[1]);
  bool ok = true;
  ok = Expect(batch.clear_prices().contains("BTC/USDT"), "U1: clear price for BTC/USDT exists") && ok;
  if (batch.clear_prices().contains("BTC/USDT")) {
    ok = ExpectNear(AsDouble(batch.clear_prices().at("BTC/USDT")), expected.clear_price, 5e-2,
                    "U1: clearPrice matches linear expectation") &&
         ok;
  }
  ok = ExpectNear(SignedRate(batch, "u1_buy", fob::common::v1::SIDE_BUY), expected.x, 1e-6,
                  "U1: buy executed rate matches linear x*") &&
       ok;
  ok = ExpectNear(SignedRate(batch, "u1_sell", fob::common::v1::SIDE_SELL), -expected.x, 1e-6,
                  "U1: sell executed rate matches linear x*") &&
       ok;
  ok = ExpectNear(NetQtyBySymbol(batch, "BTC/USDT"), 0.0, 1e-6,
                  "U1: total buy qty equals total sell qty") &&
       ok;
  return ok;
}

bool test_u2_match_limited_by_smaller_rate() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(2, 1000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u2_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 2, 99, 101),
      MakeSingleAssetOrder("u2_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 99, 101),
  };

  const auto batch = solver.Solve(orders, RefPrices({{"BTC/USDT", 100}}));
  const auto expected = ExpectedLinearTwoOrderPair(orders[0], orders[1]);
  bool ok = true;
  ok = Expect(batch.clear_prices().contains("BTC/USDT"), "U2: clear price exists") && ok;
  if (batch.clear_prices().contains("BTC/USDT")) {
    ok = ExpectNear(AsDouble(batch.clear_prices().at("BTC/USDT")), expected.clear_price, 5e-2,
                    "U2: clearPrice matches linear expectation") &&
         ok;
  }
  ok = ExpectNear(SignedRate(batch, "u2_buy", fob::common::v1::SIDE_BUY), expected.x, 1e-6,
                  "U2: buy executed rate matches linear x*") &&
       ok;
  ok = ExpectNear(SignedRate(batch, "u2_sell", fob::common::v1::SIDE_SELL), -expected.x, 1e-6,
                  "U2: sell executed rate matches linear x*") &&
       ok;
  return ok;
}

bool test_u3_qmax_clamp_and_statuses() {
  ContinuousClearingSolver solver;
  // With linear x*=0.5 and 2s interval expected execQty ~= 1.
  solver.SetSolverConfig(MakeConfig(3, 2000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u3_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 2, 2, 1, 99, 101),
      MakeSingleAssetOrder("u3_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 99, 101),
  };

  const auto batch = solver.Solve(orders, RefPrices({{"BTC/USDT", 100}}));
  const auto expected = ExpectedLinearTwoOrderPair(orders[0], orders[1]);
  bool ok = true;
  ok = Expect(batch.clear_prices().contains("BTC/USDT"), "U3: clear price exists") && ok;
  if (batch.clear_prices().contains("BTC/USDT")) {
    ok = ExpectNear(AsDouble(batch.clear_prices().at("BTC/USDT")), expected.clear_price, 5e-2,
                    "U3: clearPrice matches linear expectation") &&
         ok;
  }
  const double expected_qty = expected.x * 2.0;
  ok = ExpectNear(QtyByOrder(batch, "u3_buy"), expected_qty, 1e-6,
                  "U3: buy executed qty matches linear x*dt") &&
       ok;
  ok = ExpectNear(QtyByOrder(batch, "u3_sell"), expected_qty, 1e-6,
                  "U3: sell executed qty matches linear x*dt") &&
       ok;
  ok = Expect(StatusForOrder(batch, "u3_buy") ==
                  fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED,
              "U3: status1 == PARTIALLY_FILLED") &&
       ok;
  ok = Expect(StatusForOrder(batch, "u3_sell") ==
                  fob::common::v1::ORDER_STATUS_PARTIALLY_FILLED,
              "U3: status2 == PARTIALLY_FILLED") &&
       ok;
  return ok;
}

bool test_u4_boundary_intersection() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(4, 1000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u4_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 99, 100),
      MakeSingleAssetOrder("u4_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 100, 101),
  };

  const auto batch = solver.Solve(orders, RefPrices({{"BTC/USDT", 100}}));
  const auto expected = ExpectedLinearTwoOrderPair(orders[0], orders[1]);
  bool ok = true;
  ok = Expect(batch.clear_prices().contains("BTC/USDT"), "U4: clear price exists") && ok;
  if (batch.clear_prices().contains("BTC/USDT")) {
    ok = ExpectNear(AsDouble(batch.clear_prices().at("BTC/USDT")), expected.clear_price, 5e-2,
                    "U4: clearPrice matches linear expectation") &&
         ok;
  }
  ok = ExpectNear(SignedRate(batch, "u4_buy", fob::common::v1::SIDE_BUY), expected.x, 1e-4,
                  "U4: buy executed rate matches linear boundary behavior") &&
       ok;
  ok = ExpectNear(SignedRate(batch, "u4_sell", fob::common::v1::SIDE_SELL), -expected.x, 1e-4,
                  "U4: sell executed rate matches linear boundary behavior") &&
       ok;
  return ok;
}

bool test_u5_no_overlap_exec_rates_near_zero() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(5, 1000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u5_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 99, 100),
      MakeSingleAssetOrder("u5_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 101, 102),
  };

  const auto batch = solver.Solve(orders, RefPrices({{"BTC/USDT", 100}}));
  const auto expected = ExpectedLinearTwoOrderPair(orders[0], orders[1]);
  bool ok = true;
  ok = Expect(batch.clear_prices().contains("BTC/USDT"), "U5: clear price exists") && ok;
  if (batch.clear_prices().contains("BTC/USDT")) {
    ok = ExpectNear(AsDouble(batch.clear_prices().at("BTC/USDT")), expected.clear_price, 5e-2,
                    "U5: clearPrice matches linear expectation") &&
         ok;
  }
  ok = ExpectNear(SignedRate(batch, "u5_buy", fob::common::v1::SIDE_BUY), expected.x, 1e-6,
                  "U5: buy executed rate matches linear no-overlap case") &&
       ok;
  ok = ExpectNear(SignedRate(batch, "u5_sell", fob::common::v1::SIDE_SELL), -expected.x, 1e-6,
                  "U5: sell executed rate matches linear no-overlap case") &&
       ok;
  return ok;
}

struct PortfolioScenarioResult {
  fob::matching::v1::BatchResult batch;
  double a_btc_buy{0.0};
  double a_eth_sell{0.0};
  double net_btc{0.0};
  double net_eth{0.0};
};

PortfolioScenarioResult RunU6Scenario() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(6, 1000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakePortfolioOrder("u6_A", {{"BTC", 1}, {"ETH", -15}}, -100000, 100000, 1, 5),
      MakePortfolioOrder("u6_B", {{"BTC", -1}, {"ETH", 15}}, -100000, 100000, 1, 5),
  };

  PortfolioScenarioResult out;
  out.batch = solver.Solve(orders, RefPrices({{"BTC", 60000}, {"ETH", 4000}}));
  out.a_btc_buy = QtyByOrderSymbolSide(out.batch, "u6_A", "BTC", fob::common::v1::SIDE_BUY);
  out.a_eth_sell =
      QtyByOrderSymbolSide(out.batch, "u6_A", "ETH", fob::common::v1::SIDE_SELL);
  out.net_btc = NetQtyBySymbol(out.batch, "BTC");
  out.net_eth = NetQtyBySymbol(out.batch, "ETH");
  return out;
}

bool test_u6_portfolio_legs_are_mirrored() {
  const auto r = RunU6Scenario();
  const auto& batch = r.batch;
  const double b_btc_sell =
      QtyByOrderSymbolSide(batch, "u6_B", "BTC", fob::common::v1::SIDE_SELL);
  const double b_eth_buy =
      QtyByOrderSymbolSide(batch, "u6_B", "ETH", fob::common::v1::SIDE_BUY);

  bool ok = true;
  ok = Expect(r.a_btc_buy > 0.0, "U6: A must execute BTC buy leg") && ok;
  ok = ExpectNear(r.a_eth_sell, 15.0 * r.a_btc_buy, 1e-6,
                  "U6: A ETH leg mirrors BTC leg with 15x factor") &&
       ok;
  ok = ExpectNear(b_eth_buy, 15.0 * b_btc_sell, 1e-6,
                  "U6: B ETH leg mirrors BTC leg with 15x factor") &&
       ok;
  ok = ExpectNear(r.a_btc_buy, b_btc_sell, 1e-6,
                  "U6: mirrored BTC legs between A and B") &&
       ok;
  ok = ExpectNear(r.a_eth_sell, b_eth_buy, 1e-6,
                  "U6: mirrored ETH legs between A and B") &&
       ok;
  ok = ExpectNear(r.net_btc, 0.0, 1e-6, "U6: net BTC in system is zero") && ok;
  ok = ExpectNear(r.net_eth, 0.0, 1e-6, "U6: net ETH in system is zero") && ok;
  return ok;
}

bool test_u7_split_counterparty_is_equivalent_to_u6() {
  const auto u6 = RunU6Scenario();

  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(7, 1000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakePortfolioOrder("u7_A", {{"BTC", 1}, {"ETH", -15}}, -100000, 100000, 1, 5),
      MakePortfolioOrder("u7_B1", {{"BTC", -1}}, -100000, 100000, 1, 5),
      MakePortfolioOrder("u7_B2", {{"ETH", 15}}, -100000, 100000, 1, 5),
  };
  const auto batch = solver.Solve(orders, RefPrices({{"BTC", 60000}, {"ETH", 4000}}));

  const double a_btc_buy =
      QtyByOrderSymbolSide(batch, "u7_A", "BTC", fob::common::v1::SIDE_BUY);
  const double a_eth_sell =
      QtyByOrderSymbolSide(batch, "u7_A", "ETH", fob::common::v1::SIDE_SELL);

  bool ok = true;
  ok = ExpectNear(NetQtyBySymbol(batch, "BTC"), 0.0, 1e-3, "U7: net BTC in system is zero") &&
       ok;
  ok = ExpectNear(NetQtyBySymbol(batch, "ETH"), 0.0, 1e-3, "U7: net ETH in system is zero") &&
       ok;
  ok = ExpectNear(a_eth_sell, 15.0 * a_btc_buy, 1e-6,
                  "U7: split B still gives 15x mirrored legs") &&
       ok;
  ok = ExpectNear(a_btc_buy, u6.a_btc_buy, 1e-3,
                  "U7: BTC outcome equivalent to U6") &&
       ok;
  ok = ExpectNear(a_eth_sell, u6.a_eth_sell, 1e-3,
                  "U7: ETH outcome equivalent to U6") &&
       ok;
  return ok;
}

bool test_u8_converged_case_residual_below_tolerance() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(8, 1000, 1000, 1e-6));
  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u8_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 99, 101),
      MakeSingleAssetOrder("u8_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 99, 101),
  };
  const auto batch = solver.Solve(orders, RefPrices({{"BTC/USDT", 100}}));

  bool ok = true;
  ok = Expect(batch.diagnostics().residual_norm() <= 1e-6,
              "U8: residualNorm <= tolerance") &&
       ok;
  return ok;
}

bool test_u9_hard_case_residual_above_tolerance() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(9, 1000, 10, 1e-12));
  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u9_hard_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 99, 101),
  };
  const auto batch = solver.Solve(orders, RefPrices({{"BTC/USDT", 100}}));

  bool ok = true;
  ok = Expect(batch.diagnostics().residual_norm() > 1e-12,
              "U9: residualNorm > tolerance for hard case") &&
       ok;
  return ok;
}

bool test_u10_clear_price_within_global_bounds() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(10, 1000, 1000, 1e-6));

  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u10_btc_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 95, 105),
      MakeSingleAssetOrder("u10_btc_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 90, 110),
      MakeSingleAssetOrder("u10_eth_buy", "ETH/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 1900, 2100),
      MakeSingleAssetOrder("u10_eth_sell", "ETH/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 1800, 2200),
  };
  const auto batch = solver.Solve(
      orders,
      RefPrices({{"BTC/USDT", 100}, {"ETH/USDT", 2000}}));

  const std::unordered_map<std::string, std::pair<double, double>> bounds{
      {"BTC/USDT", {90.0, 110.0}},
      {"ETH/USDT", {1800.0, 2200.0}},
  };

  bool ok = true;
  for (const auto& [symbol, range] : bounds) {
    ok = Expect(batch.clear_prices().contains(symbol),
                "U10: clear price must be present for symbol " + symbol) &&
         ok;
    if (!batch.clear_prices().contains(symbol)) continue;
    const double px = AsDouble(batch.clear_prices().at(symbol));
    ok = Expect(px >= range.first && px <= range.second,
                "U10: min(pL) <= clearPrice <= max(pH) for " + symbol) &&
         ok;
  }
  return ok;
}

bool test_u11_external_curve_consumes_single_asset_residual() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(11, 1000, 1000, 1e-6));

  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u11_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 3, 95, 110),
      MakeSingleAssetOrder("u11_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 95, 110),
  };

  cex::matching::domain::ExternalLiquidityBySymbol external_liquidity;
  auto residual_curve = MakeExternalCurve("binance", "BTC/USDT");
  residual_curve.mutable_ask_curve()->set_p_of_q(0, 109.0);
  residual_curve.mutable_ask_curve()->set_p_of_q(1, 109.5);
  external_liquidity.emplace("BTC/USDT", residual_curve);

  const auto batch = solver.Solve(
      orders,
      RefPrices({{"BTC/USDT", 100}}),
      external_liquidity);

  int internal_count = 0;
  int external_count = 0;
  bool saw_external_provenance = false;
  for (const auto& fill : batch.fills()) {
    if (fill.liquidity_source() == "internal") {
      ++internal_count;
    }
    if (fill.liquidity_source() == "cex_hedge") {
      ++external_count;
      saw_external_provenance =
          fill.provenance().venue_id() == "binance" &&
          fill.provenance().snapshot_id() == "snapshot-binance" &&
          fill.provenance().curve_id() == "curve-binance";
    }
  }

  bool ok = true;
  const double buy_qty = QtyByOrder(batch, "u11_buy");
  const double sell_qty = QtyByOrder(batch, "u11_sell");
  ok = Expect(buy_qty > 1.0,
              "U11: buy order receives additional external execution beyond internal-only level") &&
       ok;
  ok = Expect(sell_qty >= 0.75 - 1e-6,
              "U11: sell side keeps at least internal solver execution") &&
       ok;
  ok = Expect(internal_count >= 2, "U11: internal fills remain in batch") && ok;
  ok = Expect(external_count >= 1, "U11: at least one external fill is emitted") && ok;
  ok = Expect(saw_external_provenance, "U11: external fill carries venue provenance") && ok;
  return ok;
}

bool test_u12_external_better_price_has_priority_over_internal() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(12, 1000, 1000, 1e-6));

  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u12_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 95, 105),
      MakeSingleAssetOrder("u12_sell", "BTC/USDT", fob::common::v1::SIDE_SELL, 10, 10, 1, 95, 105),
  };

  auto curve = MakeExternalCurve("binance", "BTC/USDT");
  curve.mutable_ask_curve()->set_p_of_q(0, 99.0);
  curve.mutable_ask_curve()->set_p_of_q(1, 99.5);
  curve.mutable_bid_curve()->set_p_of_q(0, 90.0);
  curve.mutable_bid_curve()->set_p_of_q(1, 89.0);

  cex::matching::domain::ExternalLiquidityBySymbol external_liquidity;
  external_liquidity.emplace("BTC/USDT", curve);

  const auto batch = solver.Solve(
      orders,
      RefPrices({{"BTC/USDT", 100}}),
      external_liquidity);

  double buy_external = 0.0;
  double buy_internal = 0.0;
  double buy_external_price = 0.0;
  for (const auto& fill : batch.fills()) {
    if (fill.order_id() != "u12_buy") continue;
    const double qty = AsDouble(fill.executed_qty());
    if (fill.liquidity_source() == "cex_hedge") {
      buy_external += qty;
      buy_external_price = AsDouble(fill.price());
    } else if (fill.liquidity_source() == "internal") {
      buy_internal += qty;
    }
  }

  bool ok = true;
  ok = Expect(buy_external > 0.99,
              "U12: buy should route to better external ask before local match") && ok;
  ok = ExpectNear(buy_internal, 0.0, 1e-9,
                  "U12: buy should not consume worse internal liquidity first") && ok;
  ok = Expect(buy_external_price <= 99.5 + 1e-9,
              "U12: external fill should use external curve price") && ok;
  return ok;
}

bool test_u13_external_executes_when_local_liquidity_is_absent() {
  ContinuousClearingSolver solver;
  solver.SetSolverConfig(MakeConfig(13, 1000, 1000, 1e-6));

  const std::vector<FlowOrder> orders{
      MakeSingleAssetOrder("u13_buy", "BTC/USDT", fob::common::v1::SIDE_BUY, 10, 10, 1, 95, 105),
  };

  cex::matching::domain::ExternalLiquidityBySymbol external_liquidity;
  external_liquidity.emplace("BTC/USDT", MakeExternalCurve("binance", "BTC/USDT"));

  const auto batch = solver.Solve(
      orders,
      RefPrices({{"BTC/USDT", 100}}),
      external_liquidity);

  double external_qty = 0.0;
  bool saw_provenance = false;
  for (const auto& fill : batch.fills()) {
    if (fill.order_id() != "u13_buy") continue;
    if (fill.liquidity_source() == "cex_hedge") {
      external_qty += AsDouble(fill.executed_qty());
      saw_provenance = fill.provenance().venue_id() == "binance";
    }
  }

  bool ok = true;
  ok = Expect(external_qty > 0.0,
              "U13: order should execute externally when local liquidity is absent") && ok;
  ok = Expect(saw_provenance, "U13: external-only fill carries venue provenance") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_u1_symmetric_buy_sell() && ok;
  ok = test_u2_match_limited_by_smaller_rate() && ok;
  ok = test_u3_qmax_clamp_and_statuses() && ok;
  ok = test_u4_boundary_intersection() && ok;
  ok = test_u5_no_overlap_exec_rates_near_zero() && ok;
  ok = test_u6_portfolio_legs_are_mirrored() && ok;
  ok = test_u7_split_counterparty_is_equivalent_to_u6() && ok;
  ok = test_u8_converged_case_residual_below_tolerance() && ok;
  ok = test_u9_hard_case_residual_above_tolerance() && ok;
  ok = test_u10_clear_price_within_global_bounds() && ok;
  ok = test_u11_external_curve_consumes_single_asset_residual() && ok;
  ok = test_u12_external_better_price_has_priority_over_internal() && ok;
  ok = test_u13_external_executes_when_local_liquidity_is_absent() && ok;
  return ok ? 0 : 1;
}
