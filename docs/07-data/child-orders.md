---
id: DOC-DATA-CHILD-ORDERS
phase: 07-data
status: spec-only
owner: core-team
source:
  - IN-005 §1 «ChildOrder (PostgreSQL child_orders)»
related:
  - docs/07-data/hedgeflows.md
  - docs/07-data/execution-reports.md
  - docs/02-system/features/F-12-execution-hedge/
  - contracts/proto/fob/hedge/v1/hedge.proto
---

# PostgreSQL Table: `child_orders`

> **Status:** ❌ DDL отсутствует в [`infra/postgres/init.sql`](../../infra/postgres/init.sql). Создание — задача T-F12-202 в [implementation plan](../implementation-plan/F-12-execution-hedge.tasks.md#t-f12-202).

## Owner

[venue-execution-adapter](../05-components/venue-execution-adapter/overview.md) — единственный writer. Read access: REST gateway (GET `/api/v1/hedge/flows/{id}/child-orders`).

## Purpose

Конкретный ордер на одном venue в рамках HedgeFlow. Один HedgeFlow может породить несколько child_orders:
- routing plan split: один child на каждый venue;
- retry / fallback: новые child_orders с тем же `hedge_flow_id`;
- partial fill требует доразмещения.

## DDL (target)

```sql
-- F-12 Execution Hedge: child_orders table.
--
-- Lifecycle: PENDING -> FILLED | PARTIALLY_FILLED | CANCELLED | REJECTED
--
-- Statuses соответствуют fob.hedge.v1.ChildOrderStatus.
-- order_type соответствует fob.execution.v1.ExecutionStrategy.

CREATE TABLE IF NOT EXISTS child_orders (
    child_order_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    hedge_flow_id    UUID NOT NULL REFERENCES hedgeflows(hedge_flow_id) ON DELETE CASCADE,

    venue_id         VARCHAR(32) NOT NULL,        -- binance / coinbase / uniswap_v3 / venue_sim
    venue_symbol     VARCHAR(32) NOT NULL,        -- venue-specific symbol
    symbol           VARCHAR(32) NOT NULL,        -- internal symbol (denormalized для запросов)
    side             VARCHAR(4) NOT NULL CHECK (side IN ('BUY', 'SELL')),
    order_type       VARCHAR(16) NOT NULL CHECK (order_type IN ('MARKET','LIMIT','TWAP','POST_ONLY','IOC')),
    tif              VARCHAR(8) CHECK (tif IS NULL OR tif IN ('GTC','GTD','IOC','FOK')),

    qty              NUMERIC(24,8) NOT NULL CHECK (qty > 0),
    price            NUMERIC(24,8),               -- NULL для MARKET

    -- идемпотентность на стороне venue
    client_order_id  VARCHAR(64) NOT NULL,
    venue_order_id   VARCHAR(64),                 -- ID после ack от venue

    -- aggregates
    filled_qty       NUMERIC(24,8) NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
    avg_price        NUMERIC(24,8),
    fee_amount       NUMERIC(24,8),
    fee_currency     VARCHAR(16),

    -- lifecycle
    status           VARCHAR(20) NOT NULL DEFAULT 'PENDING'
        CHECK (status IN ('PENDING','FILLED','PARTIALLY_FILLED','CANCELLED','REJECTED')),
    error_code       TEXT,
    error_message    TEXT,

    created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    CONSTRAINT child_orders_filled_le_qty CHECK (filled_qty <= qty * 1.01)
);

-- idempotency на стороне venue: один client_order_id на venue
CREATE UNIQUE INDEX IF NOT EXISTS child_orders_venue_client_order_uniq
    ON child_orders (venue_id, client_order_id);

-- частые запросы
CREATE INDEX IF NOT EXISTS idx_child_orders_hedge_flow ON child_orders (hedge_flow_id);
CREATE INDEX IF NOT EXISTS idx_child_orders_venue_status ON child_orders (venue_id, status);
CREATE INDEX IF NOT EXISTS idx_child_orders_open
    ON child_orders (hedge_flow_id, status)
    WHERE status IN ('PENDING', 'PARTIALLY_FILLED');
CREATE INDEX IF NOT EXISTS idx_child_orders_venue_order_id
    ON child_orders (venue_order_id) WHERE venue_order_id IS NOT NULL;
```

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
| `filled_qty`, `avg_price`, `fee_amount`, `fee_currency` | aggregates | обновляются из ExecutionReport |
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
- [F-16. Operator Console](../02-system/features/F-16-operator-panel-and-kill-switch/) — drill-down per venue.

## Source Fragments

- IN-005 §1 «ChildOrder (PostgreSQL child_orders)»
- `contracts/proto/fob/hedge/v1/hedge.proto` → `ChildOrder`, `ChildOrderStatus`
