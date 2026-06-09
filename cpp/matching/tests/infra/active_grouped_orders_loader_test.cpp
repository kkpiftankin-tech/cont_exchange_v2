// ============================================================================
// active_grouped_orders_loader_test.cpp — F-09 (T-F09-048).
// Интеграционный тест: вставить combo (multileg_vector_solver) + ноги → загрузить
// → проверить MultiLegVectorOrder. Требует TEST_PG_DSN, иначе SKIP.
// ============================================================================

#include <cstdlib>
#include <iostream>
#include <string>

#include <pqxx/pqxx>

#include "cex/common/decimal.hpp"
#include "infra/active_grouped_orders_loader.hpp"

namespace {

using cex::common::Decimal;
namespace d = cex::matching::domain;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

const d::VectorLeg* Find(const d::MultiLegVectorOrder& g, const std::string& sym) {
  for (const auto& l : g.legs) if (l.instrument_symbol == sym) return &l;
  return nullptr;
}

}  // namespace

int main() {
  const char* dsn = std::getenv("TEST_PG_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) {
    std::cout << "active_grouped_orders_loader_test: SKIPPED (no TEST_PG_DSN)\n";
    return 0;
  }

  bool ok = true;
  const std::string parent_id = "dddddddd-1111-2222-3333-444444444444";
  const std::string leg_btc = "dddddddd-aaaa-0000-0000-000000000001";
  const std::string leg_eth = "dddddddd-bbbb-0000-0000-000000000002";

  try {
    pqxx::connection conn{std::string(dsn)};
    {  // подготовка combo + ноги
      pqxx::work tx{conn};
      tx.exec_params(R"SQL(
INSERT INTO combo_orders
  (combo_order_id, combo_type, execution_mode, status, ratio_basis,
   atomicity_policy, atomicity_scope, fallback_policy, min_execution_scale, max_ratio_deviation_bps)
VALUES ($1::uuid,'basket','multileg_vector_solver','active','notional_weight',
        'scalable_atomic','internal_batch','scale_down', 0.1, 50)
ON CONFLICT (combo_order_id) DO NOTHING
)SQL",
                     parent_id);
      const char* leg_sql = R"SQL(
INSERT INTO combo_order_legs
  (leg_id, parent_order_id, instrument_symbol, side, weight, ratio_basis,
   p_low, p_high, q_rate, q_max, filled_cum, status)
VALUES ($1::uuid,$2::uuid,$3,$4,$5::numeric,'notional_weight',
        100,200,1,10,0,'active')
ON CONFLICT (leg_id) DO NOTHING
)SQL";
      tx.exec_params(leg_sql, leg_btc, parent_id, "BTCUSDT", "buy", "0.6");
      tx.exec_params(leg_sql, leg_eth, parent_id, "ETHUSDT", "sell", "0.4");
      tx.commit();
    }

    cex::matching::infra::PostgresActiveGroupsLoader loader{std::string(dsn)};
    const auto groups = loader.LoadActiveGroups();

    // Находим нашу группу среди активных.
    const d::MultiLegVectorOrder* mine = nullptr;
    for (const auto& g : groups) if (g.parent_order_id == parent_id) mine = &g;

    ok = expect(mine != nullptr, "loaded group present") && ok;
    if (mine != nullptr) {
      ok = expect(mine->atomicity_policy == d::GroupAtomicityPolicy::kScalableAtomic,
                  "policy = scalable_atomic") && ok;
      ok = expect(Decimal::cmp(mine->min_execution_scale, Decimal{1, 1}) == 0,
                  "min_execution_scale = 0.1") && ok;
      ok = expect(mine->max_ratio_deviation_bps == 50, "max_ratio_deviation_bps = 50") && ok;
      ok = expect(mine->legs.size() == 2, "two legs") && ok;
      const auto* btc = Find(*mine, "BTCUSDT");
      const auto* eth = Find(*mine, "ETHUSDT");
      // buy → +0.6, sell → -0.4 (знак из side).
      ok = expect(btc && Decimal::cmp(btc->target_ratio, Decimal{6, 1}) == 0,
                  "BTC target_ratio = +0.6") && ok;
      ok = expect(eth && Decimal::cmp(eth->target_ratio, Decimal{-4, 1}) == 0,
                  "ETH target_ratio = -0.4 (sell)") && ok;
      ok = expect(btc && Decimal::cmp(btc->q_max, Decimal{10, 0}) == 0, "BTC q_max = 10") && ok;
    }

    {  // cleanup
      pqxx::work tx{conn};
      tx.exec_params("DELETE FROM combo_order_legs WHERE parent_order_id=$1::uuid", parent_id);
      tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id=$1::uuid", parent_id);
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) {
    std::cout << "active_grouped_orders_loader_test: ALL PASSED\n";
    return 0;
  }
  return 1;
}
