// ============================================================================
// grpc_compensation_service_test.cpp — F-09 MVP-6 slice 3b (T-F09-065, ADR-040).
// In-process тест GrpcCompensationService поверх реального PostgreSQL
// (TEST_PG_DSN; иначе SKIP). Методы вызываются напрямую (без сетевого канала):
// List/Get/Resolve + идемпотентность + валидация action.
// ============================================================================

#include <cstdlib>
#include <iostream>
#include <string>

#include <pqxx/pqxx>

#include "transport/grpc_compensation_service.hpp"

namespace {
bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}
}  // namespace

int main() {
  const char* dsn = std::getenv("TEST_PG_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) {
    std::cout << "grpc_compensation_service_test: SKIPPED (no TEST_PG_DSN)\n";
    return 0;
  }

  using cex::common::Decimal;
  namespace v1 = fob::matching::v1;
  bool ok = true;
  const std::string parent_id = "cccccccc-1111-2222-3333-444444444444";
  const std::string leg_id = "cccccccc-0000-0000-0000-0000000000bb";

  try {
    pqxx::connection conn{std::string(dsn)};
    {  // combo как 'filled' → live matching loop её пропускает (test-isolation lesson).
      pqxx::work tx{conn};
      tx.exec_params(R"SQL(
INSERT INTO combo_orders (combo_order_id, combo_type, execution_mode, status, ratio_basis,
                          atomicity_policy, atomicity_scope, fallback_policy)
VALUES ($1::uuid,'basket','multileg_vector_solver','filled','notional_weight',
        'scalable_atomic','external_compensating','compensate')
ON CONFLICT (combo_order_id) DO NOTHING
)SQL",
                     parent_id);
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
    c.report_id = "rep-svc-1";
    c.reason = "rejected";
    c.internal_filled_qty = Decimal{5, 0};
    ok = expect(repo.RecordPending(c), "seed pending compensation") && ok;

    cex::matching::transport::GrpcCompensationService svc{&repo};

    // --- ListPendingCompensations (фильтр по parent) ---
    std::string comp_id;
    {
      v1::ListPendingCompensationsRequest req;
      req.set_parent_order_id(parent_id);
      v1::ListPendingCompensationsResponse resp;
      const auto st = svc.ListPendingCompensations(nullptr, &req, &resp);
      ok = expect(st.ok(), "List status OK") && ok;
      ok = expect(resp.compensations_size() == 1, "List → 1 pending for parent") && ok;
      if (resp.compensations_size() == 1) {
        comp_id = resp.compensations(0).compensation_id();
        ok = expect(resp.compensations(0).internal_filled_qty().units() == 5,
                    "List Decimal qty preserved (units=5)") && ok;
      }
    }

    // --- GetPendingCompensation (found + not-found) ---
    {
      v1::GetPendingCompensationRequest req;
      req.set_compensation_id(comp_id);
      v1::GetPendingCompensationResponse resp;
      const auto st = svc.GetPendingCompensation(nullptr, &req, &resp);
      ok = expect(st.ok() && resp.found(), "Get → found") && ok;
      ok = expect(resp.compensation().leg_id() == leg_id, "Get leg_id matches") && ok;

      v1::GetPendingCompensationRequest miss;
      miss.set_compensation_id("cccccccc-9999-9999-9999-999999999999");
      v1::GetPendingCompensationResponse miss_resp;
      svc.GetPendingCompensation(nullptr, &miss, &miss_resp);
      ok = expect(!miss_resp.found(), "Get unknown → found=false") && ok;
    }

    // --- ResolvePending: invalid action → applied=false + error ---
    {
      v1::ResolvePendingRequest req;
      req.set_compensation_id(comp_id);
      req.set_action("bogus");
      req.set_operator_id("op-1");
      v1::ResolvePendingResponse resp;
      svc.ResolvePending(nullptr, &req, &resp);
      ok = expect(!resp.applied() && resp.error().code() == "INVALID_ACTION",
                  "bogus action → INVALID_ACTION, not applied") && ok;
    }

    // --- ResolvePending: valid → applied; repeat → idempotent no-op ---
    {
      v1::ResolvePendingRequest req;
      req.set_compensation_id(comp_id);
      req.set_action("reverse_internal");
      req.set_operator_id("op-1");
      req.set_resolving_ref("fo-svc-99");
      v1::ResolvePendingResponse resp;
      svc.ResolvePending(nullptr, &req, &resp);
      ok = expect(resp.applied() && resp.error().code().empty(),
                  "valid resolve → applied, no error") && ok;

      v1::ResolvePendingResponse resp2;
      svc.ResolvePending(nullptr, &req, &resp2);
      ok = expect(!resp2.applied(), "re-resolve → idempotent applied=false") && ok;
    }

    ok = expect(repo.CountPending(parent_id) == 0, "no pending after resolve") && ok;

    {  // cleanup: execution_groups FK не cascade → удаляем первым.
      pqxx::work tx{conn};
      tx.exec_params("DELETE FROM execution_groups WHERE parent_order_id=$1::uuid", parent_id);
      tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id=$1::uuid", parent_id);
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) { std::cout << "grpc_compensation_service_test: ALL PASSED\n"; return 0; }
  return 1;
}
