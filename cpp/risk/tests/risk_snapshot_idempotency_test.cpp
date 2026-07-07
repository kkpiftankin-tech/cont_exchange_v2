// ============================================================================
// risk_snapshot_idempotency_test.cpp — T-F06-073 (ADR-046/ADR-020).
//
// Проверяет idempotency risk_snapshots: повторная доставка одного и того же
// (entity_id, batch_id) — at-least-once / rebalance / replay — НЕ создаёт
// дубль-строку и сигнализируется как kDuplicate, чтобы use case не слал
// повторный margin-call alert.
//
// Тест требует живую PostgreSQL (libpqxx + DSN из env RISK_POSTGRES_DSN), к
// которой применён infra/postgres/init.sql (partial-unique index
// risk_snapshots_entity_batch_unique). Поднятие postgres:16 + apply init.sql
// делается раннером (см. CMake/CI или Docker-команды в PR summary).
//
// Honest degraded mode: если CEX_RISK_HAS_LIBPQXX не задан ИЛИ RISK_POSTGRES_DSN
// пуст — тест ПРОПУСКАЕТСЯ (exit 0 со skip-логом). Idempotency — свойство SQL
// (ON CONFLICT против partial-unique index), поэтому без живой БД проверять
// нечего: fake-репозиторий не воспроизвёл бы реальный constraint.
// ============================================================================

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

#include "cex/common/decimal.hpp"
#include "cex/common/env.hpp"
#include "infra/risk_snapshot_repository.hpp"

namespace {

using cex::common::Decimal;
using cex::risk::infra::InsertResult;
using cex::risk::infra::RiskSnapshotInsert;
using cex::risk::infra::RiskSnapshotRepository;

bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
  }
  return condition;
}

RiskSnapshotInsert MakeSnap(const std::string& entity_id,
                            const std::string& batch_id) {
  RiskSnapshotInsert snap;
  snap.entity_id = entity_id;
  snap.batch_id = batch_id;
  snap.free_collateral = Decimal{1000, 0};
  snap.reserved_collateral = Decimal{500, 0};
  snap.initial_margin = Decimal{300, 0};
  snap.maintenance_margin = Decimal{150, 0};
  snap.throttled = false;
  snap.margin_call = true;   // tripped: первый insert → caller сошлёт alert
  snap.liquidation = false;
  return snap;
}

}  // namespace

int main() {
  const std::string dsn = cex::common::Env::get_string("RISK_POSTGRES_DSN", "");
  if (dsn.empty()) {
    std::cout << "SKIP risk_snapshot_idempotency_test: RISK_POSTGRES_DSN not set "
                 "(needs live postgres:16 with init.sql applied)\n";
    return 0;  // honest skip — idempotency is a SQL-level property.
  }

  RiskSnapshotRepository repo(dsn);
  if (!repo.enabled()) {
    std::cout << "SKIP risk_snapshot_idempotency_test: repository disabled "
                 "(no libpqxx / empty DSN)\n";
    return 0;
  }

  bool ok = true;

  // Уникальный (entity_id, batch_id) на каждый прогон — чтобы тест был
  // повторяемым против persistent БД (не зависит от прежних строк).
  const std::string suffix =
      std::to_string(static_cast<std::int64_t>(std::time(nullptr)));
  const std::string entity_id = "test-user-" + suffix;
  const std::string batch_id = "test-batch-" + suffix;

  const auto snap = MakeSnap(entity_id, batch_id);

  // 1-я доставка → kInserted (новая строка, caller публикует alert).
  const InsertResult r1 = repo.InsertSnapshot(snap);
  ok = Expect(r1 == InsertResult::kInserted,
              "first delivery of (entity_id, batch_id) must be kInserted") &&
       ok;

  // 2-я доставка того же batch (rebalance/replay) → kDuplicate, строка не
  // создаётся, caller НЕ публикует повторный alert.
  const InsertResult r2 = repo.InsertSnapshot(snap);
  ok = Expect(r2 == InsertResult::kDuplicate,
              "redelivery of same (entity_id, batch_id) must be kDuplicate") &&
       ok;

  // 3-я доставка — всё ещё дубликат (несколько повторов).
  const InsertResult r3 = repo.InsertSnapshot(snap);
  ok = Expect(r3 == InsertResult::kDuplicate,
              "further redelivery must remain kDuplicate") &&
       ok;

  // Другой batch для того же пользователя → снова kInserted (новый снапшот).
  auto snap_other = snap;
  snap_other.batch_id = batch_id + "-2";
  const InsertResult r4 = repo.InsertSnapshot(snap_other);
  ok = Expect(r4 == InsertResult::kInserted,
              "different batch_id for same entity must be kInserted") &&
       ok;

  // Снапшот без batch_id (пустой) → NULL batch_id, partial index не дедупит:
  // каждая доставка kInserted (append-only, как legacy-поведение).
  auto snap_nobatch = snap;
  snap_nobatch.batch_id = "";
  const InsertResult r5a = repo.InsertSnapshot(snap_nobatch);
  const InsertResult r5b = repo.InsertSnapshot(snap_nobatch);
  ok = Expect(r5a == InsertResult::kInserted && r5b == InsertResult::kInserted,
              "empty batch_id writes NULL — not deduped (append-only)") &&
       ok;

  if (ok) {
    std::cout << "PASS risk_snapshot_idempotency_test\n";
  }
  return ok ? 0 : 1;
}
