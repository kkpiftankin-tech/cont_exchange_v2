// ============================================================================
// postgres_child_graph_repository_test.cpp — F-09 (MVP-4, ADR-035).
// Интеграционный тест с реальным PostgreSQL (TEST_PG_DSN; иначе SKIP).
// OCO: A filled → cancel sibling B. Bracket: A filled → resize exit C (q_max,
// active). Идемпотентность по idempotency_key. group_id = execution_group_id.
// ============================================================================

#include <cstdlib>
#include <iostream>
#include <string>

#include <pqxx/pqxx>

#include "domain/child_graph_transitions.hpp"
#include "infra/postgres_child_graph_repository.hpp"

namespace {
bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}
}  // namespace

int main() {
  const char* dsn = std::getenv("TEST_PG_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) {
    std::cout << "postgres_child_graph_repository_test: SKIPPED (no TEST_PG_DSN)\n";
    return 0;
  }

  namespace d = cex::matching::domain;
  bool ok = true;
  const std::string parent_id = "cccccccc-1111-2222-3333-444444444444";
  const std::string eg_id = "cccccccc-8888-7777-6666-555555555555";
  const std::string leg_a = "cccccccc-0000-0000-0000-0000000000aa";  // entry/source (filled)
  const std::string leg_b = "cccccccc-0000-0000-0000-0000000000bb";  // OCO sibling
  const std::string leg_c = "cccccccc-0000-0000-0000-0000000000cc";  // bracket exit (waiting)

  try {
    pqxx::connection conn{std::string(dsn)};
    {
      pqxx::work tx{conn};
      tx.exec_params(R"SQL(
INSERT INTO combo_orders (combo_order_id, combo_type, execution_mode, status, ratio_basis,
                          atomicity_policy, atomicity_scope, fallback_policy)
VALUES ($1::uuid,'bracket','multileg_vector_solver','partially_filled','notional_weight',
        'scalable_atomic','internal_batch','scale_down')
ON CONFLICT (combo_order_id) DO NOTHING
)SQL",
                     parent_id);
      auto leg = [&](const std::string& id, const std::string& status, int q_max, int filled) {
        tx.exec_params(R"SQL(
INSERT INTO combo_order_legs (leg_id, parent_order_id, instrument_symbol, side, weight,
                             ratio_basis, p_low, p_high, q_rate, q_max, filled_cum, status)
VALUES ($1::uuid,$2::uuid,'BTCUSDT','buy',0.6,'notional_weight',100,200,1,$3,$4,$5)
ON CONFLICT (leg_id) DO NOTHING
)SQL",
                       id, parent_id, q_max, filled, status);
      };
      leg(leg_a, "filled", 10, 10);              // entry filled
      leg(leg_b, "active", 10, 0);               // OCO sibling
      leg(leg_c, "waiting_for_trigger", 1, 0);   // bracket exit
      auto link = [&](const std::string& from, const std::string& to, const std::string& type) {
        tx.exec_params(R"SQL(
INSERT INTO conditional_links (parent_order_id, from_leg_id, to_leg_id, link_type)
VALUES ($1::uuid,$2::uuid,$3::uuid,$4)
)SQL",
                       parent_id, from, to, type);
      };
      link(leg_a, leg_b, "oco");
      link(leg_a, leg_c, "bracket");
      tx.exec_params(R"SQL(
INSERT INTO execution_groups (execution_group_id, batch_id, parent_order_id, execution_mode,
                             group_status, execution_scale, atomicity_policy, atomicity_scope)
VALUES ($1::uuid,'batch-1',$2::uuid,'multileg_vector_solver','partial',1,'scalable_atomic','internal_batch')
ON CONFLICT (execution_group_id) DO NOTHING
)SQL",
                     eg_id, parent_id);
      tx.commit();
    }

    cex::matching::infra::PostgresChildGraphRepository repo{std::string(dsn)};

    // Load → apply OCO + bracket → persist.
    auto state = repo.LoadComboGroupState(parent_id);
    ok = expect(state.legs.size() == 3, "loaded 3 legs (incl. waiting exit)") && ok;
    ok = expect(state.edges.size() == 2, "loaded 2 edges (oco + bracket)") && ok;

    auto transitions = d::ApplyOCOTransitions(state);
    auto bracket = d::ResizeBracketExits(state);
    transitions.insert(transitions.end(), bracket.begin(), bracket.end());
    ok = expect(transitions.size() == 2, "2 transitions (cancel B + resize C)") && ok;

    repo.PersistChildGraphTransitions(eg_id, "batch-1", state, transitions);
    repo.PersistChildGraphTransitions(eg_id, "batch-1", state, transitions);  // идемпотентно

    {
      pqxx::work tx{conn};
      const std::string b_status = tx.exec_params(
          "SELECT status FROM combo_order_legs WHERE leg_id=$1::uuid", leg_b)[0][0].as<std::string>();
      const bool c_resized = tx.exec_params(
          "SELECT q_max = 10 AND status='active' FROM combo_order_legs WHERE leg_id=$1::uuid",
          leg_c)[0][0].as<bool>();
      const int tr_rows = tx.exec_params(
          "SELECT count(*) FROM group_state_transitions WHERE group_id=$1::uuid", eg_id)[0][0].as<int>();
      ok = expect(b_status == "cancelled", "OCO: sibling B cancelled") && ok;
      ok = expect(c_resized, "bracket: exit C q_max=10 + active") && ok;
      ok = expect(tr_rows == 2, "2 transitions persisted (idempotent on repeat)") && ok;
      tx.commit();
    }

    {  // cleanup (cascade: links, transitions, legs via FK).
      pqxx::work tx{conn};
      tx.exec_params("DELETE FROM execution_groups WHERE parent_order_id=$1::uuid", parent_id);
      tx.exec_params("DELETE FROM combo_orders WHERE combo_order_id=$1::uuid", parent_id);
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (ok) { std::cout << "postgres_child_graph_repository_test: ALL PASSED\n"; return 0; }
  return 1;
}
