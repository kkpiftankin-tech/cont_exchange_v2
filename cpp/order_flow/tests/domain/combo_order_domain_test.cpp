// ============================================================================
// combo_order_domain_test.cpp — unit-тесты domain-инвариантов F-09 (T-F09-030).
// Hand-rolled harness (без GTest), как matching_domain_tests: main() возвращает
// 0 при успехе, 1 при провале. Покрывает AC-F09-001..004 на уровне validate().
// ============================================================================

#include <iostream>
#include <optional>

#include "domain/combo_order.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::order_flow::domain;

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    return false;
  }
  return true;
}

/// Валидная нога basket (weight-based, internal, без fill).
d::Leg MakeLeg(const std::string& leg_id, const std::string& symbol, d::Side side) {
  d::Leg leg;
  leg.leg_id = leg_id;
  leg.parent_order_id = "co_1";
  leg.instrument_symbol = symbol;
  leg.side = side;
  leg.weight = Decimal{6, 1};  // 0.6
  leg.ratio_basis = d::RatioBasis::kNotionalWeight;
  leg.p_low = Decimal{1, 0};   // 1
  leg.p_high = Decimal{2, 0};  // 2
  leg.q_rate = Decimal{1, 0};  // 1
  leg.q_max = Decimal{10, 0};  // 10
  leg.filled_cum = Decimal::zero();
  leg.status = d::LegStatus::kActive;
  return leg;
}

/// Валидная basket-combo (multileg_vector_solver + scalable_atomic).
d::ComboOrder MakeValidCombo() {
  d::ComboOrder combo;
  combo.combo_order_id = "co_1";
  combo.combo_type = d::ComboType::kBasket;
  combo.execution_mode = d::ExecutionMode::kMultilegVectorSolver;
  combo.atomicity_policy = d::AtomicityPolicy::kScalableAtomic;
  combo.atomicity_scope = d::AtomicityScope::kInternalBatch;
  combo.fallback_policy = d::FallbackPolicy::kScaleDown;
  combo.ratio_basis = d::RatioBasis::kNotionalWeight;
  combo.legs.push_back(MakeLeg("leg_btc", "BTCUSDT", d::Side::kBuy));
  combo.legs.push_back(MakeLeg("leg_eth", "ETHUSDT", d::Side::kBuy));
  combo.status = d::ParentOrderStatus::kActive;
  return combo;
}

}  // namespace

int main() {
  bool ok = true;

  // NormalizeComboOrder — позитивный кейс: валидная basket проходит.
  {
    const auto combo = MakeValidCombo();
    ok = expect(!combo.validate().has_value(),
                "NormalizeComboOrder: valid basket must pass validate()") && ok;
  }

  // ComboOrderInvariantViolation — p_low > p_high → ошибка.
  {
    auto combo = MakeValidCombo();
    combo.legs[0].p_low = Decimal{3, 0};   // 3 > p_high(2)
    const auto err = combo.validate();
    ok = expect(err.has_value() && err->code == "LEG_PRICE_RANGE",
                "ComboOrderInvariantViolation: p_low>p_high must fail") && ok;
  }

  // LegFilledCumExceedsQMax → ошибка.
  {
    auto combo = MakeValidCombo();
    combo.legs[1].filled_cum = Decimal{20, 0};  // 20 > q_max(10)
    const auto err = combo.validate();
    ok = expect(err.has_value() && err->code == "LEG_FILLED_CUM_EXCEEDS_QMAX",
                "LegFilledCumExceedsQMax must fail") && ok;
  }

  // multileg_vector_solver без atomicity_policy → обязательная ошибка (AC-F09-001).
  {
    auto combo = MakeValidCombo();
    combo.atomicity_policy = d::AtomicityPolicy::kUnspecified;
    const auto err = combo.validate();
    ok = expect(err.has_value() && err->code == "COMBO_POLICY_REQUIRED",
                "multileg_vector_solver requires atomicity_policy") && ok;
  }

  // orchestration_only без constraints → проходит (AC: §17).
  {
    auto combo = MakeValidCombo();
    combo.execution_mode = d::ExecutionMode::kOrchestrationOnly;
    combo.atomicity_policy = d::AtomicityPolicy::kUnspecified;  // не требуется
    combo.constraints.clear();
    ok = expect(!combo.validate().has_value(),
                "orchestration_only without constraints must pass") && ok;
  }

  // strict_atomic + external_compensating scope → ошибка (AC-F09-006).
  {
    auto combo = MakeValidCombo();
    combo.atomicity_policy = d::AtomicityPolicy::kStrictAtomic;
    combo.atomicity_scope = d::AtomicityScope::kExternalCompensating;
    const auto err = combo.validate();
    ok = expect(err.has_value() && err->code == "COMBO_STRICT_EXTERNAL_SCOPE",
                "strict_atomic + external_compensating scope must fail") && ok;
  }

  // Leg::remaining_qty = max(q_max - filled_cum, 0).
  {
    auto leg = MakeLeg("leg_x", "BTCUSDT", d::Side::kBuy);
    leg.filled_cum = Decimal{4, 0};  // 4; q_max=10 → remaining=6
    const auto rem = leg.remaining_qty();
    ok = expect(Decimal::cmp(rem, Decimal{6, 0}) == 0,
                "remaining_qty = q_max - filled_cum") && ok;
    leg.filled_cum = Decimal{10, 0};  // == q_max → remaining=0
    ok = expect(Decimal::cmp(leg.remaining_qty(), Decimal::zero()) == 0,
                "remaining_qty clamps to 0 at full fill") && ok;
  }

  // BatchOrder: пустой child_refs → ошибка.
  {
    d::BatchOrder batch;
    batch.batch_order_id = "bo_1";
    batch.user_id = "u_1";
    batch.account_id = "acc_1";
    const auto err = batch.validate();
    ok = expect(err.has_value() && err->code == "BATCH_NO_CHILDREN",
                "batch with no children must fail") && ok;
  }

  if (ok) {
    std::cout << "combo_order_domain_test: ALL PASSED\n";
    return 0;
  }
  std::cerr << "combo_order_domain_test: FAILURES\n";
  return 1;
}
