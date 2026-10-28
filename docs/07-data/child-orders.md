---
id: DOC-DATA-CHILD-ORDERS
phase: 07-data
status: schema-ready-impl-pending
owner: core-team
source:
  - IN-005 §1 «ChildOrder (PostgreSQL child_orders)»
  - IN-008 v2 (PR-F12-2 schema applied 2026-05-23)
related:
  - docs/07-data/hedgeflows.md
  - docs/07-data/execution-reports.md
  - docs/02-system/features/F-12-execution-hedge/
  - contracts/proto/fob/hedge/v1/hedge.proto
---

# PostgreSQL Table: `child_orders`

> **Status:** ✅ DDL добавлена в [`infra/postgres/init.sql`](../../infra/postgres/init.sql) (PR-F12-2, 2026-05-23) с FK на `hedgeflows(hedge_flow_id)` и UNIQUE-индексом по `(hedge_flow_id, client_order_id)` для idempotency. C++ репозиторий-код будет реализован в PR-F12-3.

## Owner

[venue-execution-adapter](../05-components/venue-execution-adapter/overview.md) — единственный writer. Read access: REST gateway (GET `/api/v1/hedge/flows/{id}/child-orders`); frontend-api (BFF) читает состоявшиеся band-хеджи агентов CE (`hedge_flow_id` вида `ce|band|…`) для панели «Живые позиции», вкладки «Граф» и графика «Сделки на внешних биржах по парам» на вкладке «PnL» ([pair-trades-history](../06-api/rest/pair-trades-history.md), запрос по индексу `idx_child_orders_created`).

## Purpose

Конкретный ордер на одном venue в рамках HedgeFlow. Один HedgeFlow может породить несколько child_orders:
- routing plan split: один child на каждый venue;
- retry / fallback: новые child_orders с тем же `hedge_flow_id`;
- partial fill требует доразмещения.

## DDL (actual — `infra/postgres/init.sql:163-194`)

```sql
-- F-12 Execution Hedge: child_orders table.
--
-- Lifecycle: PENDING -> FILLED | PARTIALLY_FILLED | CANCELLED | REJECTED.
--
-- =====================================================================
-- Различия с исходной IN-005 спекой (drift correction 2026-05-27, DoD-18):
--   1. id-колонки TEXT, не UUID — см. hedgeflows.md (composite intent_id
--      от matching's F-04 external_fill пути).
--   2. Decimal precision NUMERIC(38,18), не (24,8).
--   3. ENUM-набор order_type сейчас включает 'IOC' и 'FOK' напрямую как
--      order_type (исторически правильнее это TIF, но venues генерирует
--      такие записи). Перенос в tif — отдельный refactor PR.
--   4. UNIQUE index по (hedge_flow_id, client_order_id), не
--      (venue_id, client_order_id) — потому что matching's composite
--      client_order_id уже включает venue в свой хвост.
-- =====================================================================
CREATE TABLE IF NOT EXISTS child_orders (
  child_order_id   TEXT PRIMARY KEY,                                 -- was UUID
  hedge_flow_id    TEXT NOT NULL REFERENCES hedgeflows(hedge_flow_id) ON DELETE CASCADE,  -- was UUID
  venue_id         TEXT NOT NULL,
  symbol           TEXT NOT NULL,                                    -- venue-mapped symbol (e.g. XBTUSD on Kraken)
  side             TEXT NOT NULL CHECK (side IN ('BUY', 'SELL')),
  order_type       TEXT NOT NULL CHECK (order_type IN ('MARKET', 'LIMIT', 'POST_ONLY', 'IOC', 'FOK')),
  qty              NUMERIC(38, 18) NOT NULL CHECK (qty > 0),         -- was (24,8)
  price            NUMERIC(38, 18),                                  -- nullable for MARKET
  tif              TEXT NOT NULL CHECK (tif IN ('GTC', 'IOC', 'FOK')),
  filled_qty       NUMERIC(38, 18) NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
  avg_price        NUMERIC(38, 18),
  fee              NUMERIC(38, 18) NOT NULL DEFAULT 0,               -- was fee_amount in spec
  fee_currency     TEXT,
  client_order_id  TEXT NOT NULL,
  venue_order_id   TEXT,                                             -- assigned by venue after accept
  status           TEXT NOT NULL CHECK (status IN ('PENDING', 'FILLED', 'PARTIALLY_FILLED', 'CANCELLED', 'REJECTED')),
  error_code       TEXT,
  error_message    TEXT,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  CONSTRAINT child_orders_filled_le_qty CHECK (filled_qty <= qty * 1.01)
);

-- Idempotency: prevent retry duplicates per (hedge_flow_id, client_order_id).
CREATE UNIQUE INDEX IF NOT EXISTS child_orders_idem
  ON child_orders (hedge_flow_id, client_order_id);

CREATE INDEX IF NOT EXISTS idx_child_orders_venue ON child_orders (venue_id, status);
CREATE INDEX IF NOT EXISTS idx_child_orders_hedge_flow ON child_orders (hedge_flow_id);
CREATE INDEX IF NOT EXISTS idx_child_orders_venue_order_id
  ON child_orders (venue_id, venue_order_id) WHERE venue_order_id IS NOT NULL;
```

**Note on `PARTIALLY_FILLED`**: included in the CHECK constraint, but in
dev runtime venues' sim adapter always returns FILLED in a single
ApplyReport. PARTIALLY_FILLED state appears in row history only when
real CEX/DEX adapter returns multi-step fills — not yet observed in
dev. When backtest/replay (F-15) drives venues with historical traces,
PARTIALLY_FILLED becomes a normal transient state.

## Поля

| Поле | Тип | Описание |
| --- | --- | --- |
| `child_order_id` | UUID PK | |
| `hedge_flow_id` | UUID FK | back-ref к `hedgeflows` |
| `venue_id` | VARCHAR(32) | |
| `venue_symbol` | VARCHAR(32) | |
| `symbol` | VARCHAR(32) | internal symbol (denormalized) |
| `side` | enum | BUY/SELL |
| `order_type` | enum | соответствует `ExecutionStrategy` |
| `tif` | enum | TimeInForce |
| `qty` | DECIMAL(24,8) | |
| `price` | DECIMAL(24,8) | для LIMIT/POST_ONLY; NULL для MARKET |
| `client_order_id` | VARCHAR(64) | unique per venue |
| `venue_order_id` | VARCHAR(64) | ID от venue после ack |
| `filled_qty`, `avg_price`, `fee_amount`, `fee_currency` | aggregates | обновляются из ExecutionReport; правила `filled_qty` и `avg_price` — в разделе «Обновление filled_qty и avg_price» |
| `status` | enum | PENDING/FILLED/PARTIALLY_FILLED/CANCELLED/REJECTED |
| `error_code`, `error_message` | при REJECTED | |
| `created_at`, `updated_at` | TIMESTAMPTZ | |

## Lifecycle Constraints

- При INSERT: status='PENDING', filled_qty=0, venue_order_id=NULL.
- После ack от venue: UPDATE venue_order_id, updated_at.
- При первом filled: status='PARTIALLY_FILLED'.
- При complete fill: status='FILLED', filled_qty=qty (с допуском по lot rounding).
- При cancel: status='CANCELLED', error_code='cancelled_by_adapter|cancelled_by_venue|timeout'.
- При reject: status='REJECTED'.

## Обновление filled_qty и avg_price

`ExecutionReport.filled_qty` — накопленный объём, `average_price` — цена прироста объёма этого отчёта (тик-локальный VWAP резидентной заявки; ledger читает его так же); у терминального отчёта без нового fill цены нет. Поэтому `PostgresChildOrderRepository::ApplyReport` ведёт агрегаты так:

- `filled_qty = GREATEST(старое, новое)` — объём не уменьшается от запоздавшего отчёта;
- `avg_price` — накопленный VWAP: $\bar p_{new}=\dfrac{\bar p_{old}\,q_{old}+p\,(q_{new}-q_{old})}{q_{new}}$; первый fill задаёт цену отчёта;
- цена не меняется, если в отчёте её нет или она нулевая, либо объём не вырос (терминальный отчёт без fill, повторная доставка);
- строка без fill получает 0 (как раньше).

Проверка формулы: `cpp/venues/tests/sql/child_orders_apply_report_avg_price.sql` (временная таблица, откат).

**Инцидент 2026-10-22.** С 08:04 UTC (резидентные заявки ADR-067) прежний код записывал `avg_price = 0` при терминальном отчёте без нового fill и хранил цену только последнего тика у заявок с несколькими fill'ами: панель «Живые позиции» (R и U) и вкладка «Граф» читают эти цены и показывали искажённый заработок (ledger и ClickHouse не затронуты). Исправлено в `ApplyReport`; уже записанные строки восстанавливает `tools/child-orders-avgprice-repair/repair.sh` по отчётам из ClickHouse (с резервной копией `child_orders_avgfix_backup_20261022`). История заработка агентов `ce_agent_pnl_history` за период с 08:04 UTC до исправления содержит старые числа.

## Idempotency

- `client_order_id` уникален per venue.
- При повторной обработке того же ExecutionReport (replay or duplicate Kafka): UPDATE based on `(child_order_id, status)` no-op если status уже finalised.

## Retention

- Joined в `hedgeflows` retention: 365 дней.
- При `ON DELETE CASCADE` удаление родительского hedgeflows очищает child_orders.

## Migration

В follow-up PR `feat/f12-postgres` (вместе с hedgeflows):
1. DDL в [`infra/postgres/init.sql`](../../infra/postgres/init.sql);
2. `cpp/venues/src/infra/postgres_child_order_repository.{hpp,cpp}`;
3. wire в `ExecuteOnVenue` (INSERT перед PlaceOrder, UPDATE после ExecutionReport).

## Used In Features

- [F-12. Execution Hedge](../02-system/features/F-12-execution-hedge/) — primary.
- [F-13. Post-Trade Report](../02-system/features/F-13-posttrade-report/) — детализация исполнения.
- [F-16. Operator Console](../02-system/features/F-16-operator-console/) — drill-down per venue.

## Source Fragments

- IN-005 §1 «ChildOrder (PostgreSQL child_orders)»
- `contracts/proto/fob/hedge/v1/hedge.proto` → `ChildOrder`, `ChildOrderStatus`
