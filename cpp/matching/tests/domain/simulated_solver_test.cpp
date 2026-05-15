#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/simulated_solver.hpp"

namespace {

fob::common::v1::Decimal Dec(const int64_t units, const int32_t scale = 0) {
  fob::common::v1::Decimal out;
  out.set_units(units);
  out.set_scale(scale);
  return out;
}

fob::orders::v1::FlowOrder MakeOrder(const std::string& order_id,
                                     const std::string& symbol,
                                     const fob::common::v1::Side side,
                                     const int64_t total_qty,
                                     const int64_t remaining_qty,
                                     const int64_t max_speed,
                                     const int64_t price_low,
                                     const int64_t price_high) {
  fob::orders::v1::FlowOrder order;
  order.set_order_id(order_id);
  order.set_user_id("demo-user");
  order.mutable_instrument()->set_symbol(symbol);
  if (symbol == "BTC/USDT") {
    order.mutable_instrument()->set_base("BTC");
    order.mutable_instrument()->set_quote("USDT");
  } else {
    order.mutable_instrument()->set_base("ETH");
    order.mutable_instrument()->set_quote("USDT");
  }
  order.set_side(side);
  *order.mutable_total_qty() = Dec(total_qty);
  *order.mutable_remaining_qty() = Dec(remaining_qty);
  *order.mutable_max_speed() = Dec(max_speed);
  *order.mutable_price_low() = Dec(price_low);
  *order.mutable_price_high() = Dec(price_high);
  order.set_status(fob::common::v1::ORDER_STATUS_NEW);
  return order;
}

fob::venue::v1::VenueLiquidityCurve MakeCurve(const std::string& venue_id,
                                              const std::string& symbol) {
  fob::venue::v1::VenueLiquidityCurve curve;
  curve.set_venue_id(venue_id);
  curve.mutable_instrument()->set_symbol(symbol);
  curve.mutable_mid_price()->set_units(101);
  curve.mutable_mid_price()->set_scale(0);

  auto* ask = curve.mutable_ask_curve();
  ask->add_q_grid(0.0);
  ask->add_q_grid(1.0);
  ask->add_q_grid(5.0);
  ask->add_p_of_q(101.0);
  ask->add_p_of_q(102.0);
  ask->add_p_of_q(105.0);

  auto* bid = curve.mutable_bid_curve();
  bid->add_q_grid(0.0);
  bid->add_q_grid(1.0);
  bid->add_q_grid(5.0);
  bid->add_p_of_q(99.0);
  bid->add_p_of_q(98.0);
  bid->add_p_of_q(95.0);

  return curve;
}

void test_executes_internal_match() {
  cex::matching::domain::SimulatedContinuousClearingSolver solver(500);
  std::vector<fob::orders::v1::FlowOrder> orders;
  orders.push_back(MakeOrder("b1", "BTC/USDT", fob::common::v1::SIDE_BUY,
                             10, 10, 4, 100, 102));
  orders.push_back(MakeOrder("s1", "BTC/USDT", fob::common::v1::SIDE_SELL,
                             10, 10, 4, 100, 102));

  const auto batch = solver.Solve(orders, {}, {});
  assert(batch.fills_size() == 2);
  assert(batch.order_updates_size() == 2);

  for (const auto& fill : batch.fills()) {
    assert(fill.executed_qty().units() == 2);
    assert(fill.liquidity_source() == "internal");
  }

  assert(batch.clear_prices().at("BTC/USDT").units() == 101);
}

void test_clamps_to_remaining_qty_inside_internal_match() {
  cex::matching::domain::SimulatedContinuousClearingSolver solver(1000);
  std::vector<fob::orders::v1::FlowOrder> orders;
  orders.push_back(MakeOrder("b1", "BTC/USDT", fob::common::v1::SIDE_BUY,
                             10, 10, 10, 100, 104));
  orders.push_back(MakeOrder("s1", "BTC/USDT", fob::common::v1::SIDE_SELL,
                             10, 3, 10, 100, 104));

  const auto batch = solver.Solve(orders, {}, {});
  assert(batch.fills_size() == 2);

  bool saw_sell = false;
  for (const auto& fill : batch.fills()) {
    if (fill.order_id() == "s1") {
      saw_sell = true;
      assert(fill.executed_qty().units() == 3);
    }
  }
  assert(saw_sell);
}

void test_clear_price_is_weighted_average_per_symbol() {
  cex::matching::domain::SimulatedContinuousClearingSolver solver(1000);
  std::vector<fob::orders::v1::FlowOrder> orders;
  orders.push_back(MakeOrder("b1", "BTC/USDT", fob::common::v1::SIDE_BUY,
                             10, 10, 1, 100, 102));  // 101
  orders.push_back(MakeOrder("s1", "BTC/USDT", fob::common::v1::SIDE_SELL,
                             10, 10, 1, 98, 100));   // 99
  orders.push_back(MakeOrder("b2", "ETH/USDT", fob::common::v1::SIDE_BUY,
                             10, 10, 1, 2000, 2002));  // 2001
  orders.push_back(MakeOrder("s2", "ETH/USDT", fob::common::v1::SIDE_SELL,
                             10, 10, 1, 1998, 2000));  // 1999

  const auto batch = solver.Solve(orders, {}, {});

  assert(batch.clear_prices().size() == 2);
  assert(batch.clear_prices().at("BTC/USDT").units() == 100);
  assert(batch.clear_prices().at("ETH/USDT").units() == 2000);
}

void test_uses_external_curve_for_residual() {
  cex::matching::domain::SimulatedContinuousClearingSolver solver(1000);
  std::vector<fob::orders::v1::FlowOrder> orders;
  orders.push_back(MakeOrder("b1", "BTC/USDT", fob::common::v1::SIDE_BUY,
                             10, 10, 3, 95, 110));
  orders.push_back(MakeOrder("s1", "BTC/USDT", fob::common::v1::SIDE_SELL,
                             10, 10, 1, 95, 110));

  cex::matching::domain::ExternalLiquidityBySymbol external;
  external.emplace("BTC/USDT", MakeCurve("binance", "BTC/USDT"));

  const auto batch = solver.Solve(orders, {}, external);
  assert(batch.fills_size() == 3);

  int internal_count = 0;
  int external_count = 0;
  for (const auto& fill : batch.fills()) {
    if (fill.liquidity_source() == "internal") ++internal_count;
    if (fill.liquidity_source() == "cex_hedge") ++external_count;
  }
  assert(internal_count == 2);
  assert(external_count == 1);
}

void test_single_sided_without_external_stays_unfilled() {
  cex::matching::domain::SimulatedContinuousClearingSolver solver(1000);
  std::vector<fob::orders::v1::FlowOrder> orders;
  orders.push_back(MakeOrder("o1", "BTC/USDT", fob::common::v1::SIDE_BUY,
                             10, 10, 4, 100, 102));

  const auto batch = solver.Solve(orders, {}, {});
  assert(batch.fills_size() == 0);
  assert(batch.order_updates_size() == 0);
}

}  // namespace

int main() {
  test_executes_internal_match();
  test_clamps_to_remaining_qty_inside_internal_match();
  test_clear_price_is_weighted_average_per_symbol();
  test_uses_external_curve_for_residual();
  test_single_sided_without_external_stays_unfilled();
  return 0;
}
