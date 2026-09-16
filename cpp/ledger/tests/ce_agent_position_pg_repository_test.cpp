// ============================================================================
// ce_agent_position_pg_repository_test.cpp — F-18 v2 (T-F18-204/205,
// ADR-061 §7).
//
// Интеграционный тест с РЕАЛЬНЫМ PostgreSQL. Требует env TEST_PG_DSN (или
// LEDGER_POSTGRES_DSN как fallback); при отсутствии обоих — ГРАЦИОЗНО
// ПРОПУСКАЕТСЯ (return 0), тот же паттерн, что у order_flow
// (postgres_combo_order_repository_test.cpp), чтобы сборка без поднятого PG
// оставалась зелёной.
//
// Проверяет ЧЕРЕЗ ТУ ЖЕ реализацию, что использует main.cpp
// (MakeLedgerPgPool + PostgresAgentPositionRepository), а не сырой SQL:
//   1. upsert-накопление c←c+f в РЕАЛЬНОМ PG (не in-memory кэше);
//   2. идемпотентность по batch_id (guard в UPSERT ... WHERE ...);
//   3. "позиция переживает рестарт" — НОВЫЙ pool/репозиторий (эмулирует
//      перезапуск процесса ledger — main.cpp создаёт их заново на каждый
//      старт) читает ровно то же накопленное значение из PG.
// ============================================================================
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include <pqxx/pqxx>

#include "cex/common/decimal.hpp"
#include "infra/postgres_repositories.hpp"

using cex::common::Decimal;
using cex::ledger::infra::MakeLedgerPgPool;
using cex::ledger::infra::PostgresAgentPositionRepository;

namespace {

bool expect(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAILED: " << msg << '\n'; return false; }
  return true;
}

}  // namespace

int main() {
  const char* dsn = std::getenv("TEST_PG_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) dsn = std::getenv("LEDGER_POSTGRES_DSN");
  if (dsn == nullptr || std::string(dsn).empty()) {
    std::cout << "ce_agent_position_pg_repository_test: SKIPPED (no TEST_PG_DSN/LEDGER_POSTGRES_DSN)\n";
    return 0;
  }
  const std::string dsn_str(dsn);

  // Фиксированный тестовый ключ — очищаем до и после, как в
  // postgres_combo_order_repository_test.cpp (нет per-test transaction
  // rollback в этой реальной PG).
  const std::string agent_id = "T-F18-204-TEST-AGENT";
  const std::string asset = "BTC";
  const std::string venue = "Binance";

  bool ok = true;
  try {
    {
      pqxx::connection conn{dsn_str};
      pqxx::work tx{conn};
      tx.exec_params(
          "DELETE FROM ce_agent_position WHERE agent_id = $1 AND asset = $2 AND venue = $3",
          agent_id, asset, venue);
      tx.commit();
    }

    {
      // "Процесс 1" — эмулирует живой ledger, применяющий два такта клиринга.
      auto pool = MakeLedgerPgPool(dsn_str, 1);
      ok &= expect(pool != nullptr, "pool created (build must link libpqxx for this test)");
      if (!pool) throw std::runtime_error("no pool — aborting test");

      PostgresAgentPositionRepository repo(pool);

      auto r1 = repo.ApplyDelta(agent_id, "translator", asset, venue, Decimal{500, 2},
                                "pg-it-batch-1", 1000);
      ok &= expect(Decimal::cmp(r1.position, Decimal{500, 2}) == 0,
                  "first delta on a fresh row equals the delta itself (5.00)");

      auto r2 = repo.ApplyDelta(agent_id, "translator", asset, venue, Decimal{-200, 2},
                                "pg-it-batch-2", 2000);
      ok &= expect(Decimal::cmp(r2.position, Decimal{300, 2}) == 0,
                  "accumulation in real PG: c <- c + f = 5.00 - 2.00 = 3.00");

      // Идемпотентность: повтор ТОГО ЖЕ batch_id — guard отклоняет двойное применение.
      auto r3 = repo.ApplyDelta(agent_id, "translator", asset, venue, Decimal{-200, 2},
                                "pg-it-batch-2", 2000);
      ok &= expect(Decimal::cmp(r3.position, Decimal{300, 2}) == 0,
                  "duplicate batch_id delivery is a no-op in real PG (UPSERT guard)");
    }  // pool/repo уничтожены здесь — данные живут только в PG.

    {
      // "Процесс 2" (после рестарта) — НОВЫЙ pool, НОВЫЙ репозиторий, ничего
      // общего в памяти с "процессом 1" выше. Значение обязано прийти из PG.
      auto pool2 = MakeLedgerPgPool(dsn_str, 1);
      PostgresAgentPositionRepository repo2(pool2);
      const auto rows = repo2.GetPositions({agent_id}, {}, {});
      ok &= expect(rows.size() == 1, "exactly one row for the test agent found after restart");
      if (rows.size() == 1) {
        ok &= expect(Decimal::cmp(rows[0].position, Decimal{300, 2}) == 0,
                    "position SURVIVES restart: read back 3.00 from PG, not from memory");
        ok &= expect(rows[0].agent_kind == "translator", "agent_kind persisted across restart");
        ok &= expect(rows[0].last_batch_id == "pg-it-batch-2", "last_batch_id persisted across restart");
        ok &= expect(rows[0].venue == venue, "venue is part of the persisted key (LIVE BUG closed)");
      }
    }

    // Cleanup.
    {
      pqxx::connection conn{dsn_str};
      pqxx::work tx{conn};
      tx.exec_params(
          "DELETE FROM ce_agent_position WHERE agent_id = $1 AND asset = $2 AND venue = $3",
          agent_id, asset, venue);
      tx.commit();
    }
  } catch (const std::exception& e) {
    std::cerr << "FAILED: exception: " << e.what() << '\n';
    ok = false;
  }

  if (!ok) {
    std::cout << "ce_agent_position_pg_repository_test: FAILED\n";
    return 1;
  }
  std::cout << "ce_agent_position_pg_repository_test: ALL PASSED\n";
  return 0;
}
