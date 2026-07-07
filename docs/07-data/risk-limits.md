---
id: DOC-DATA-RISK-LIMITS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-001 «БД 1: PostgreSQL (OLTP)» §risk_limits
related:
  - docs/07-data/oltp-schema.md
  - docs/07-data/risk-snapshots.md
  - docs/02-system/features/F-06-positions-pnl-margin/
  - docs/implementation-plan/F-06-positions-pnl-margin.tasks.md
---

# PostgreSQL Table: `risk_limits`

> **Status:** ✅ DDL добавлена в [`infra/postgres/init.sql`](../../infra/postgres/init.sql) (F-06, T-F06-011).
> Канонический per-field разбор — в [`oltp-schema.md` §risk_limits](oltp-schema.md#таблица-risk_limits). Поля `initial_margin_rate` / `maintenance_margin_rate` добавлены этой фичей для расчёта margin (T-F06-031).

| Свойство | Значение |
|---|---|
| База | PostgreSQL |
| Owner-сервис | risk |
| Writers | risk |
| Readers | matching (kill_switch), gateway/UI |
| Создаётся | `infra/postgres/init.sql:739` |

## Назначение

Лимиты по сущностям `user` / `role` / `symbol` / `global`: notional, position, leverage, order rate, whitelist, kill-switch и (F-06) margin-rates. Используется risk для pre-trade проверок (F-07) и расчёта initial/maintenance margin (F-06). matching читает `kill_switch` перед batch.

## DDL (actual — `infra/postgres/init.sql:739`)

```sql
CREATE TABLE IF NOT EXISTS risk_limits (
  limit_id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  entity_type              TEXT NOT NULL
                           CHECK (entity_type IN ('user', 'role', 'symbol', 'global')),
  entity_id                TEXT NOT NULL,
  max_notional             NUMERIC(38, 18),
  max_position             NUMERIC(38, 18),
  max_leverage             NUMERIC(10, 4),
  max_order_rate           INTEGER,
  asset_whitelist          TEXT[],
  kill_switch              BOOLEAN NOT NULL DEFAULT FALSE,
  initial_margin_rate      NUMERIC(10, 4),
  maintenance_margin_rate  NUMERIC(10, 4),
  updated_at               TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_by               TEXT,
  CONSTRAINT risk_limits_entity_unique UNIQUE (entity_type, entity_id)
);
CREATE INDEX IF NOT EXISTS idx_risk_limits_entity ON risk_limits (entity_type, entity_id);
```

## Инварианты

- INV-1: `entity_type IN ('user','role','symbol','global')` — CHECK.
- INV-2: ровно одна строка на `(entity_type, entity_id)` — UNIQUE `risk_limits_entity_unique`.
- INV-3 (app-level): `initial_margin_rate ≈ 1 / max_leverage`; `maintenance_margin_rate < initial_margin_rate`.

## Когда заполняется

| Событие | Writer | Operation |
|---|---|---|
| Seed dev | init.sql | INSERT role demo/client с margin-rates |
| Operator меняет лимит | risk | UPSERT ON CONFLICT (entity_type, entity_id) |

## Когда читается

| Query | Reader | Index used |
|---|---|---|
| limits для entity | risk | `idx_risk_limits_entity` |
| kill_switch перед batch | matching | `idx_risk_limits_entity` |

## Retention

Never deleted в MVP; история изменений — отдельный audit-trail (вне scope F-06).

## Связанные feature.yaml

- [F-06](../02-system/features/F-06-positions-pnl-margin/feature.yaml)
- F-07 (pre-trade risk) переиспользует ту же таблицу — не дублировать миграцию (см. T-F06-011 / T-F07-001).

## Известные ограничения

- `entity_id` / `updated_by` — TEXT без FK на `users` (нет таблицы `users`).
- **Не путать** с `replay_risk_limits` (`id, version, body_json`) — это JSON-конфиг-реестр backtest (F-15), не оперативные лимиты.
