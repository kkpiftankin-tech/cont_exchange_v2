// ============================================================================
// orders_normalized_grouped_producer_test.cpp — F-09 (T-F09-035).
// Hand-rolled harness; мок PublishFn захватывает OrdersNormalized.
// Проверяет: key = combo_order_id, grouped tags, маппинг ноги → FlowOrder.
// ============================================================================

#include <iostream>
#include <string>
#include <vector>

#include "domain/combo_order.hpp"
#include "infra/orders_normalized_grouped_producer.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::order_flow::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

d::Leg MakeLeg(const std::string& leg_id, const std::string& symbol, d::Side side) {
  d::Leg leg;
  leg.leg_id = leg_id;
  leg.instrument_symbol = symbol;
  leg.side = side;
  leg.weight = Decimal{6, 1};
  leg.ratio_basis = d::RatioBasis::kNotionalWeight;
  leg.p_low = Decimal{100, 0};
  leg.p_high = Decimal{200, 0};
  leg.q_rate = Decimal{1, 0};
  leg.q_max = Decimal{10, 0};
  leg.filled_cum = Decimal::zero();
  leg.status = d::LegStatus::kActive;
  return leg;
}

}  // namespace

int main() {
  bool ok = true;

  d::ComboOrder combo;
  combo.combo_order_id = "co-parent-1";
  combo.combo_type = d::ComboType::kBasket;
  combo.execution_mode = d::ExecutionMode::kOrchestrationOnly;  // orchestration_only
  combo.ratio_basis = d::RatioBasis::kNotionalWeight;
  combo.legs.push_back(MakeLeg("leg-btc", "BTCUSDT", d::Side::kBuy));
  combo.legs.push_back(MakeLeg("leg-eth", "ETHUSDT", d::Side::kSell));

  std::vector<fob::orders::v1::OrdersNormalized> captured;
  cex::order_flow::infra::OrdersNormalizedGroupedProducer producer{
      [&captured](const fob::orders::v1::OrdersNormalized& e) {
        captured.push_back(e);
        return true;
      }};

  const bool published = producer.PublishOrchestration(combo, "user-1", "acc-1");
  ok = expect(published, "PublishOrchestration returns true") && ok;
  ok = expect(captured.size() == 2, "two leg events published") && ok;

  if (captured.size() == 2) {
    for (const auto& e : captured) {
      ok = expect(e.meta().partition_key() == "co-parent-1",
                  "partition_key == combo_order_id (key = parentOrderId)") && ok;
      ok = expect(e.has_create() && e.create().has_order(), "event carries FlowOrderCreate") && ok;
      const auto& order = e.create().order();
      const auto& tags = order.tags();
      ok = expect(tags.at("combo_id") == "co-parent-1", "tag combo_id") && ok;
      ok = expect(tags.at("execution_mode") == "orchestration_only", "tag execution_mode") && ok;
      ok = expect(!tags.at("leg_id").empty(), "tag leg_id present") && ok;
    }
    // Маппинг конкретной ноги.
    const auto& first = captured[0].create().order();
    ok = expect(first.order_id() == "leg-btc", "first event order_id == leg_id") && ok;
    ok = expect(first.side() == fob::common::v1::SIDE_BUY, "first leg side BUY") && ok;
    const auto& second = captured[1].create().order();
    ok = expect(second.side() == fob::common::v1::SIDE_SELL, "second leg side SELL") && ok;
  }

  // Guard: multileg_vector_solver не публикуется этим путём.
  {
    auto solver_combo = combo;
    solver_combo.execution_mode = d::ExecutionMode::kMultilegVectorSolver;
    solver_combo.atomicity_policy = d::AtomicityPolicy::kScalableAtomic;
    bool threw = false;
    try {
      cex::order_flow::infra::OrdersNormalizedGroupedProducer p2{
          [](const fob::orders::v1::OrdersNormalized&) { return true; }};
      p2.PublishOrchestration(solver_combo, "u", "a");
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    ok = expect(threw, "multileg_vector_solver rejected by PublishOrchestration") && ok;
  }

  if (ok) {
    std::cout << "orders_normalized_grouped_producer_test: ALL PASSED\n";
    return 0;
  }
  return 1;
}
