// ============================================================================
// postgres_combo_order_repository_test.cpp — F-09 (T-F09-034).
//
// Интеграционный тест с реальным PostgreSQL. Требует env TEST_PG_DSN; при его
// отсутствии тест ГРАЦИОЗНО ПРОПУСКАЕТСЯ (return 0), чтобы CI без БД был зелёным
// (паттерн совместим со сборкой docker без поднятого PG).
//
// Сценарии (при наличии DSN): insert basket(2 ноги) round-trip + идемпотентный
// повторный insert (ON CONFLICT DO NOTHING) не бросает и не дублирует.
// ============================================================================

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include <pqxx/pqxx>

#include "domain/combo_order.hpp"
#include "infra/postgres_combo_order_repository.hpp"

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
  leg.p_low = Decimal{1, 0};
  leg.p_high = Decimal{2, 0};
  leg.q_rate = Decimal{1, 0};
  leg.q_max = Decimal{10, 0};
  leg.filled_cum = Decimal::zero();
  leg.status = d::LegStatus::kActive;
  return leg;
}

d::ComboOrder MakeCombo(const std::string& id) {
  d::ComboOrder combo;
  combo.combo_order_id = id;
  combo.combo_type = d::ComboType::kBasket;
  combo.execution_mode = d::ExecutionMode::kMultilegVectorSolver;
  combo.atomicity_policy = d::AtomicityPolicy::kScalableAtomic;
  combo.atomicity_scope = d::AtomicityScope::kInternalBatch;
  combo.fallback_policy = d::FallbackPolicy::kScaleDown;
  combo.ratio_basis = d::RatioBasis::kNotionalWeight;
  combo.legs.push_back(MakeLeg("11111111-1111-1111-1111-111111111111", "BTCUSDT", d::Side::kBuy));
  combo.legs.push_back(MakeLeg("22222222-2222-2222-2222-222222222222", "ETHUSDT", d::Side::kBuy));
  combo.status = d::ParentOrderStatus::kActive;
  return combo;
}

}  // namespace

int main() {
  const char* dsn = std::getenv("TEST_PG_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) {
    std::cout << "postgres_combo_order_repository_test: SKIPPED (no TEST_PG_DSN)\n";
    return 0;
  }

  bool ok = true;
  const std::string combo_id = "33333333-3333-3333-3333-333333333333";

  try {
    cex::order_flow::infra::PostgresComboOrderRepository repo{std::string(dsn)};
    const auto combo = MakeCombo(combo_id);

    repo.InsertComboOrder(combo, std::nullopt);
    // Идемпотентность: повторный insert не должен бросить.
    repo.InsertComboOrder(combo, std::nullopt);

    pqxx::connection conn{std::string(dsn)};
    pqxx::work tx{conn};
    const auto combo_rows =
        tx.query_value<int>("SELECT count(*) FROM combo_orders WHERE combo_order_id = " +
                            tx.quote(combo_id) + "::uuid");
    const auto leg_rows =
        tx.query_value<int>("SELECT count(*) FROM combo_order_legs WHERE parent_order_id = " +
                            tx.quote(combo_id) + "::uuid");
    ok = expect(combo_rows == 1, "combo persisted exactly once (idempotent)") && ok;
    ok = expect(leg_rows == 2, "two legs persisted") && ok;

    // cleanup (FK ON DELETE CASCADE удалит ноги).
    tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id = $1::uuid", combo_id);
    tx.commit();
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) {
    std::cout << "postgres_combo_order_repository_test: ALL PASSED\n";
    return 0;
  }
  return 1;
}
