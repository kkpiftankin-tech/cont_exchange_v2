// ============================================================================
// postgres_combo_compensation_repository_test.cpp — F-09 MVP-5 (ADR-037).
// Интеграционный тест с реальным PostgreSQL (TEST_PG_DSN; иначе SKIP).
// RecordPending идемпотентно по (parent, leg, report); CountPending.
// ============================================================================

#include <cstdlib>
#include <iostream>
#include <string>

#include <pqxx/pqxx>

#include "infra/postgres_combo_compensation_repository.hpp"

namespace {
bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}
}  // namespace

int main() {
  const char* dsn = std::getenv("TEST_PG_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) {
    std::cout << "postgres_combo_compensation_repository_test: SKIPPED (no TEST_PG_DSN)\n";
    return 0;
  }

  using cex::common::Decimal;
  bool ok = true;
  const std::string parent_id = "dddddddd-1111-2222-3333-444444444444";
  const std::string leg_id = "dddddddd-0000-0000-0000-0000000000aa";

  try {
    pqxx::connection conn{std::string(dsn)};
    {
      pqxx::work tx{conn};
      tx.exec_params(R"SQL(
INSERT INTO combo_orders (combo_order_id, combo_type, execution_mode, status, ratio_basis,
                          atomicity_policy, atomicity_scope, fallback_policy)
VALUES ($1::uuid,'basket','multileg_vector_solver','filled','notional_weight',
        'scalable_atomic','external_compensating','compensate')
ON CONFLICT (combo_order_id) DO NOTHING
)SQL",
                     parent_id);
      // Нога, чтобы FindComboLegParent её нашёл.
      tx.exec_params(R"SQL(
INSERT INTO combo_order_legs (leg_id, parent_order_id, instrument_symbol, side, weight,
                             ratio_basis, p_low, p_high, q_rate, q_max, filled_cum, status)
VALUES ($1::uuid,$2::uuid,'ETHUSDT','sell',0.4,'notional_weight',100,200,100,10,0,'active')
ON CONFLICT (leg_id) DO NOTHING
)SQL",
                     leg_id, parent_id);
      tx.commit();
    }

    cex::matching::infra::PostgresComboCompensationRepository repo{std::string(dsn)};

    cex::matching::infra::ComboCompensation c;
    c.parent_order_id = parent_id;
    c.leg_id = leg_id;
    c.report_id = "rep-1";
    c.reason = "rejected";
    c.internal_filled_qty = Decimal{5, 0};

    ok = expect(repo.RecordPending(c), "first record → inserted") && ok;
    ok = expect(!repo.RecordPending(c), "same report → idempotent no-op") && ok;
    ok = expect(repo.CountPending(parent_id) == 1, "one pending compensation") && ok;

    cex::matching::infra::ComboCompensation c2 = c;
    c2.report_id = "rep-2";
    c2.reason = "timeout";
    ok = expect(repo.RecordPending(c2), "second distinct report → inserted") && ok;
    ok = expect(repo.CountPending(parent_id) == 2, "two pending compensations") && ok;

    // FindComboLegParent: combo-нога → parent; неизвестный leg → nullopt.
    const auto found = repo.FindComboLegParent(leg_id);
    ok = expect(found.has_value() && *found == parent_id, "FindComboLegParent → parent") && ok;
    const auto missing = repo.FindComboLegParent("dddddddd-9999-9999-9999-999999999999");
    ok = expect(!missing.has_value(), "FindComboLegParent unknown → nullopt") && ok;

    // MVP-6: ListPending + ResolvePending (идемпотентно).
    std::string my_comp_id;
    int my_pending = 0;
    for (const auto& p : repo.ListPending()) {
      if (p.parent_order_id == parent_id) {
        ++my_pending;
        if (my_comp_id.empty()) my_comp_id = p.compensation_id;
      }
    }
    ok = expect(my_pending == 2, "ListPending sees our 2 pending") && ok;
    ok = expect(repo.ResolvePending(my_comp_id, "reverse_internal", "op-1", "fo-99"),
                "ResolvePending pending → applied") && ok;
    ok = expect(!repo.ResolvePending(my_comp_id, "reverse_internal", "op-1", "fo-99"),
                "re-resolve → idempotent no-op") && ok;
    ok = expect(repo.CountPending(parent_id) == 1, "one pending after resolve") && ok;

    // MVP-5 fix: терминальный статус external-ноги (идемпотентно).
    ok = expect(repo.MarkExternalLegFailed(leg_id),
                "MarkExternalLegFailed: active→failed_external") && ok;
    ok = expect(!repo.MarkExternalLegFailed(leg_id),
                "MarkExternalLegFailed again → no-op") && ok;
    ok = expect(!repo.MarkExternalLegFilled(leg_id, Decimal{1, 0}),
                "MarkExternalLegFilled from failed → no-op (не active)") && ok;
    const std::string leg2 = "dddddddd-0000-0000-0000-0000000000bb";
    {
      pqxx::work tx{conn};
      tx.exec_params(R"SQL(
INSERT INTO combo_order_legs (leg_id, parent_order_id, instrument_symbol, side, weight,
                             ratio_basis, p_low, p_high, q_rate, q_max, filled_cum, status)
VALUES ($1::uuid,$2::uuid,'BTCUSDT','buy',0.6,'notional_weight',100,200,100,10,0,'active')
ON CONFLICT (leg_id) DO NOTHING
)SQL",
                     leg2, parent_id);
      tx.commit();
    }
    ok = expect(repo.MarkExternalLegFilled(leg2, Decimal{3, 0}),
                "MarkExternalLegFilled: active→filled") && ok;
    ok = expect(!repo.MarkExternalLegFilled(leg2, Decimal{3, 0}),
                "MarkExternalLegFilled again → no-op (filled_cum не двоится)") && ok;
    {
      pqxx::work tx{conn};
      const auto r = tx.exec_params(
          "SELECT filled_cum::text FROM combo_order_legs WHERE leg_id=$1::uuid", leg2);
      tx.commit();
      ok = expect(!r.empty() && r[0][0].as<std::string>().rfind("3", 0) == 0,
                  "filled_cum=3 (идемпотентно, не 6)") && ok;
    }

    {  // execution_groups FK не cascade → удаляем первым (на случай live-matching).
      pqxx::work tx{conn};
      tx.exec_params("DELETE FROM execution_groups WHERE parent_order_id=$1::uuid", parent_id);
      tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id=$1::uuid", parent_id);
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) { std::cout << "postgres_combo_compensation_repository_test: ALL PASSED\n"; return 0; }
  return 1;
}
