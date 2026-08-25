// ============================================================================
// postgres_execution_groups_repository_test.cpp — F-09 (T-F09-047).
// Интеграционный тест с реальным PostgreSQL. Требует TEST_PG_DSN; иначе SKIP
// (return 0). Сценарий: создать parent combo → persist execution_group →
// идемпотентный re-persist → проверить статус combo → cleanup.
// ============================================================================

#include <cstdlib>
#include <iostream>
#include <string>

#include <pqxx/pqxx>

#include "cex/common/decimal.hpp"
#include "infra/postgres_execution_groups_repository.hpp"

namespace {

using cex::common::Decimal;
namespace mv1 = fob::matching::v1;

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

mv1::ExecutionGroup MakeEg(const std::string& eg_id, const std::string& parent) {
  mv1::ExecutionGroup eg;
  eg.set_execution_group_id(eg_id);
  eg.set_batch_id("batch-eg-test");
  eg.set_parent_order_id(parent);
  eg.set_execution_mode(fob::orders::v1::EXECUTION_MODE_MULTILEG_VECTOR_SOLVER);
  eg.set_atomicity_policy(fob::orders::v1::ATOMICITY_POLICY_SCALABLE_ATOMIC);
  eg.set_atomicity_scope(fob::orders::v1::ATOMICITY_SCOPE_INTERNAL_BATCH);
  eg.set_group_status(mv1::GROUP_STATUS_PARTIAL);  // scale<1 → partially_filled
  *eg.mutable_execution_scale() = Decimal{6, 1}.to_proto();  // 0.6
  auto* lr = eg.add_leg_results();
  lr->set_leg_id("00000000-0000-0000-0000-0000000000aa");
  *lr->mutable_exec_qty() = Decimal{6, 0}.to_proto();
  *lr->mutable_exec_price() = Decimal{100, 0}.to_proto();
  return eg;
}

}  // namespace

int main() {
  const char* dsn = std::getenv("TEST_PG_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) {
    std::cout << "postgres_execution_groups_repository_test: SKIPPED (no TEST_PG_DSN)\n";
    return 0;
  }

  bool ok = true;
  const std::string parent_id = "eeeeeeee-1111-2222-3333-444444444444";
  const std::string eg_id = "99999999-8888-7777-6666-555555555555";

  try {
    pqxx::connection conn{std::string(dsn)};
    {
      // Подготовка: создаём parent combo (FK execution_groups → combo_orders).
      pqxx::work tx{conn};
      tx.exec_params(
          R"SQL(
INSERT INTO combo_orders
  (combo_order_id, combo_type, execution_mode, status, ratio_basis,
   atomicity_policy, atomicity_scope, fallback_policy)
VALUES ($1::uuid,'basket','multileg_vector_solver','active','notional_weight',
        'scalable_atomic','internal_batch','scale_down')
ON CONFLICT (combo_order_id) DO NOTHING
)SQL",
          parent_id);
      // Нога, на которую ссылается leg_result (для проверки накопления filled_cum).
      tx.exec_params(R"SQL(
INSERT INTO combo_order_legs
  (leg_id, parent_order_id, instrument_symbol, side, weight, ratio_basis,
   p_low, p_high, q_rate, q_max, filled_cum, status)
VALUES ($1::uuid,$2::uuid,'BTCUSDT','buy',0.6,'notional_weight',
        100,200,1,10,0,'active')
ON CONFLICT (leg_id) DO NOTHING
)SQL",
                     "00000000-0000-0000-0000-0000000000aa", parent_id);
      tx.commit();
    }

    cex::matching::infra::PostgresExecutionGroupsRepository repo{std::string(dsn)};
    const auto eg = MakeEg(eg_id, parent_id);

    repo.PersistExecutionGroup(eg);
    repo.PersistExecutionGroup(eg);  // идемпотентный повтор

    {
      pqxx::work tx{conn};
      const int eg_rows =
          tx.exec_params("SELECT count(*) FROM execution_groups WHERE execution_group_id=$1::uuid",
                         eg_id)[0][0].as<int>();
      const int tr_rows =
          tx.exec_params("SELECT count(*) FROM group_state_transitions WHERE group_id=$1::uuid",
                         eg_id)[0][0].as<int>();
      const std::string combo_status =
          tx.exec_params("SELECT status FROM combo_orders WHERE combo_order_id=$1::uuid",
                         parent_id)[0][0].as<std::string>();
      // exec_qty=6 применён один раз (два persist идемпотентны) → filled_cum=6, не 12.
      const bool leg_six = tx.exec_params(
          "SELECT filled_cum = 6 FROM combo_order_legs WHERE leg_id=$1::uuid",
          "00000000-0000-0000-0000-0000000000aa")[0][0].as<bool>();
      ok = expect(eg_rows == 1, "execution_group persisted exactly once (idempotent)") && ok;
      ok = expect(tr_rows == 1, "one transition (idempotent by idempotency_key)") && ok;
      ok = expect(leg_six, "leg filled_cum = 6 (fill applied once, not double-counted)") && ok;
      ok = expect(combo_status == "partially_filled", "combo → partially_filled (6 < q_max 10)") && ok;
      tx.commit();
    }

    {  // cleanup: execution_groups (cascade transitions) → legs → combo_orders.
      pqxx::work tx{conn};
      tx.exec_params("DELETE FROM execution_groups WHERE parent_order_id=$1::uuid", parent_id);
      tx.exec_params("DELETE FROM combo_order_legs WHERE parent_order_id=$1::uuid", parent_id);
      tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id=$1::uuid", parent_id);
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) {
    std::cout << "postgres_execution_groups_repository_test: ALL PASSED\n";
    return 0;
  }
  return 1;
}
