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

    // ------------------------------------------------------------------
    // Fix: SumInternalFilledQty — реальная внутренняя экспозиция к откату.
    // Раньше internal_filled_qty брался из report.filled_qty() (=0 у reject) →
    // компенсации с qty=0. Теперь = Σ filled_cum внутренних ног, кроме упавшей.
    // ------------------------------------------------------------------
    const std::string p2 = "dddddddd-2222-2222-2222-222222222222";
    const std::string p2_ext = "dddddddd-2000-0000-0000-0000000000ee";  // упавшая внешняя
    const std::string p2_int1 = "dddddddd-2000-0000-0000-0000000000a1";  // internal, fill=4
    const std::string p2_int2 = "dddddddd-2000-0000-0000-0000000000a2";  // internal, fill=1.5
    const std::string p3 = "dddddddd-3333-3333-3333-333333333333";
    const std::string p3_ext = "dddddddd-3000-0000-0000-0000000000ee";  // только внешняя, fill=0
    {
      pqxx::work tx{conn};
      for (const auto& pid : {p2, p3}) {
        tx.exec_params(R"SQL(
INSERT INTO combo_orders (combo_order_id, combo_type, execution_mode, status, ratio_basis,
                          atomicity_policy, atomicity_scope, fallback_policy)
VALUES ($1::uuid,'basket','multileg_vector_solver','active','notional_weight',
        'scalable_atomic','external_compensating','compensate')
ON CONFLICT (combo_order_id) DO NOTHING
)SQL", pid);
      }
      auto ins_leg = [&](const std::string& lid, const std::string& pid, const std::string& sym,
                         const std::string& venue, const std::string& fill) {
        tx.exec_params(R"SQL(
INSERT INTO combo_order_legs (leg_id, parent_order_id, instrument_symbol, side, weight,
                             ratio_basis, p_low, p_high, q_rate, q_max, filled_cum, status,
                             venue_preferences)
VALUES ($1::uuid,$2::uuid,$3,'buy',0.5,'notional_weight',100,200,100,10,$4::numeric,'active',
        ARRAY[$5]::text[])
ON CONFLICT (leg_id) DO NOTHING
)SQL", lid, pid, sym, fill, venue);
      };
      ins_leg(p2_ext, p2, "ETHUSDT", "binance", "0");
      ins_leg(p2_int1, p2, "BTCUSDT", "internal", "4");
      ins_leg(p2_int2, p2, "SOLUSDT", "internal", "1.5");
      ins_leg(p3_ext, p3, "ETHUSDT", "binance", "0");
      tx.commit();
    }
    // p2: Σ внутренних (4 + 1.5) = 5.5, внешняя p2_ext исключена.
    const Decimal sum2 = repo.SumInternalFilledQty(p2, p2_ext);
    ok = expect(Decimal::cmp(sum2, Decimal{55, 1}) == 0,
                "SumInternalFilledQty: internal 4 + 1.5 = 5.5 (external excluded)") && ok;
    // p3: внутренних ног с fill нет → 0 → гейт не пишет компенсацию.
    const Decimal sum3 = repo.SumInternalFilledQty(p3, p3_ext);
    ok = expect(Decimal::cmp(sum3, Decimal::zero()) == 0,
                "SumInternalFilledQty: no internal fill → 0 (gate skips compensation)") && ok;

    {  // execution_groups FK не cascade → удаляем первым (на случай live-matching).
      pqxx::work tx{conn};
      for (const auto& pid : {parent_id, p2, p3}) {
        tx.exec_params("DELETE FROM execution_groups WHERE parent_order_id=$1::uuid", pid);
        tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id=$1::uuid", pid);
      }
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) { std::cout << "postgres_combo_compensation_repository_test: ALL PASSED\n"; return 0; }
  return 1;
}
