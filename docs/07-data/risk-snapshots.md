---
id: DOC-DATA-RISK-SNAPSHOTS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-001 «БД 1: PostgreSQL (OLTP)» §risk_snapshots
related:
  - docs/07-data/oltp-schema.md
  - docs/07-data/risk-limits.md
  - docs/02-system/features/F-06-positions-pnl-margin/
  - docs/implementation-plan/F-06-positions-pnl-margin.tasks.md
---

# PostgreSQL Table: `risk_snapshots`

> **Status:** ✅ DDL добавлена в [`infra/postgres/init.sql`](../../infra/postgres/init.sql) (F-06, T-F06-011). `RiskSnapshotRepository` — T-F06-031.
> Канонический per-field разбор — в [`oltp-schema.md` §risk_snapshots](oltp-schema.md#таблица-risk_snapshots).

| Свойство | Значение |
|---|---|
| База | PostgreSQL |
| Owner-сервис | risk |
| Writers | risk |
| Readers | ledger, gateway, observability |
| Создаётся | `infra/postgres/init.sql:768` |

## Назначение

Снимки маржинального состояния сущности (user_id или venue_id), пишутся после каждого batch и важных событий. Содержат free/reserved collateral, initial/maintenance margin и `risk_flags` (margin_call / liquidation / throttled). `GetRiskSnapshot` отдаёт последний снимок gateway; observability строит dashboards.

## DDL (actual — `infra/postgres/init.sql:768`)

```sql
CREATE TABLE IF NOT EXISTS risk_snapshots (
  snapshot_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  entity_id            TEXT NOT NULL,
  batch_id             TEXT,                          -- T-F06-073 idempotency key
  free_collateral      NUMERIC(38, 18) NOT NULL,
  reserved_collateral  NUMERIC(38, 18) NOT NULL,
  initial_margin       NUMERIC(38, 18) NOT NULL,
  maintenance_margin   NUMERIC(38, 18) NOT NULL,
  risk_flags           JSONB,
  "timestamp"          TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
ALTER TABLE risk_snapshots ADD COLUMN IF NOT EXISTS batch_id TEXT;

CREATE INDEX IF NOT EXISTS idx_risk_snapshots_entity_timestamp
  ON risk_snapshots (entity_id, "timestamp" DESC);

-- T-F06-073: дедуп повторной доставки batch.outputs (at-least-once / rebalance
-- / replay). Партиал WHERE batch_id IS NOT NULL — старые/event-driven строки
-- без batch_id не конфликтуют.
CREATE UNIQUE INDEX IF NOT EXISTS risk_snapshots_entity_batch_unique
  ON risk_snapshots (entity_id, batch_id)
  WHERE batch_id IS NOT NULL;

-- C1 (ADR-046): entity_id не пустой (идемпотентно через DO $$ ... IF NOT EXISTS).
ALTER TABLE risk_snapshots
  ADD CONSTRAINT risk_snapshots_entity_id_nonempty CHECK (entity_id <> '');
```

## Идемпотентность (T-F06-073 / ADR-046, ADR-020)

`batch.outputs` доставляется at-least-once; rebalance consumer-group и replay
повторяют те же сообщения. Раньше повтор давал дубль-строку в `risk_snapshots`
и повторный margin-call alert (ложные алерты). Теперь:

- `(entity_id, batch_id)` — idempotency-ключ через partial-unique index
  `risk_snapshots_entity_batch_unique`;
- `RiskSnapshotRepository::InsertSnapshot` пишет с `ON CONFLICT (entity_id,
  batch_id) WHERE batch_id IS NOT NULL DO NOTHING` (предикат обязателен — index
  партиальный) и возвращает `InsertResult` (`kInserted` / `kDuplicate` /
  `kError`);
- `buildRiskSnapshot` публикует `RiskAlert` (MARGIN_CALL / LIQUIDATION) ТОЛЬКО
  при `kInserted`; на `kDuplicate` обработка считается успешной (offset
  коммитится), но alert не шлётся повторно;
- снапшоты без `batch_id` (NULL) не дедупятся — append-only как раньше.

## Инварианты

- INV-1: append-only — каждый уникальный (`entity_id`, `batch_id`) вставляет
  одну строку; повтор того же ключа — no-op (не UPDATE, не дубль).
- INV-2 (app-level): `margin_call` флаг при `free_collateral < maintenance_margin`.
- INV-3 (T-F06-073): margin-call alert эмитится не более одного раза на
  (`entity_id`, `batch_id`).

## Когда заполняется

| Событие | Writer | Operation |
|---|---|---|
| buildRiskSnapshot (после batch.outputs) | risk | INSERT |

## Когда читается

| Query | Reader | Index used |
|---|---|---|
| GetRiskSnapshot(user_id) — latest | risk/gateway | `idx_risk_snapshots_entity_timestamp` |
| история маржи | observability | `idx_risk_snapshots_entity_timestamp` |

## Retention

Append-only; периодический перенос/обрезка в OLAP — отдельный поток аналитики (вне scope F-06).

## Связанные feature.yaml

- [F-06](../02-system/features/F-06-positions-pnl-margin/feature.yaml)

## Известные ограничения

- Колонка `timestamp` — зарезервированное слово, в DDL и запросах квотируется как `"timestamp"`.
- `entity_id` — TEXT без FK (может быть user_id или venue_id).
- Append-only рост: без OLAP-выгрузки таблица растёт неограниченно.
