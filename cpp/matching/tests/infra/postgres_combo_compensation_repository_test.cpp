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
VALUES ($1::uuid,'basket','multileg_vector_solver','partially_filled','notional_weight',
        'scalable_atomic','external_compensating','compensate')
ON CONFLICT (combo_order_id) DO NOTHING
)SQL",
                     parent_id);
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

    {
      pqxx::work tx{conn};
      tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id=$1::uuid", parent_id);  // cascade
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) { std::cout << "postgres_combo_compensation_repository_test: ALL PASSED\n"; return 0; }
  return 1;
}
