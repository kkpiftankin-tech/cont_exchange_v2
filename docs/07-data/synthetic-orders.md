---
id: DOC-DATA-SYNTHETIC-ORDERS
phase: 07-data
status: draft
owner: core-team
source:
  - IN-004 §«PostgreSQL: synthetic_orders»
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - cpp/venues/src/infra/postgres_synthetic_order_repository.cpp
  - docs/06-api/messaging/venue-topics.md
---

# PostgreSQL: `synthetic_orders`

Lifecycle производных `SyntheticFlowOrder`, которые материализуются из `VenueLiquidityCurve` для совместимости с matching v1 (которому нужен `FlowOrder`-вход, не FOB-кривая).

## Owner / Access

- **Сервис-владелец:** [cpp/venues](../05-components/venue-liquidity-curve-builder/overview.md) — R/W через [PostgresSyntheticOrderRepository](../../cpp/venues/src/infra/postgres_synthetic_order_repository.cpp).
- Создаются Liquidity Curve Builder'ом при `venue_config.synthetic_enabled=true`.
- Читаются Admin API: `GET /api/v1/venues/{venueId}/synthetics`.

## DDL (canonical)

```sql
CREATE TYPE side_enum AS ENUM ('buy', 'sell');
CREATE TYPE synthetic_status_enum AS ENUM ('active', 'expired', 'used');

CREATE TABLE synthetic_orders (
  syntheticid    UUID PRIMARY KEY,
  venueid        VARCHAR(32) REFERENCES venue_config(venueid),
  symbol         VARCHAR(32) NOT NULL,
  side           side_enum NOT NULL,
  pl             NUMERIC(24,8) NOT NULL,  -- price low
  ph             NUMERIC(24,8) NOT NULL,  -- price high
  qrate          NUMERIC(24,8) NOT NULL,  -- max speed (base units / sec)
  qmax           NUMERIC(24,8) NOT NULL,  -- total notional cap
  curveid        UUID NOT NULL,           -- FK to ClickHouse venue_liquidity_curves
  snapshotid     UUID NOT NULL,           -- FK to ClickHouse venue_snapshots
  createdat      TIMESTAMPTZ NOT NULL DEFAULT now(),
  expiresat      TIMESTAMPTZ NOT NULL,
  status         synthetic_status_enum NOT NULL DEFAULT 'active'
);

CREATE INDEX IF NOT EXISTS synthetic_orders_venue_symbol_idx
  ON synthetic_orders (venueid, symbol, status);
CREATE INDEX IF NOT EXISTS synthetic_orders_curveid_idx
  ON synthetic_orders (curveid);
CREATE INDEX IF NOT EXISTS synthetic_orders_active_expiry_idx
  ON synthetic_orders (expiresat) WHERE status = 'active';
```

## Lifecycle

```text
   active  -- TTL expiry -->  expired
     |
     | matched in batch
     v
    used
```

- `active` — заявка свежая, доступна matching solver'у.
- `expired` — TTL истёк; cleanup-job (pending, см. open-questions.md §12) должен переводить `active → expired`.
- `used` — solver matching применил эту заявку (полностью).

Инварианты:

- `pl > 0`, `ph > 0`, `pl <= ph`, `qrate > 0`, `qmax > 0`.
- `expiresat > createdat`.
- Один `curveid` может породить ≥ 1 synthetic (отдельные по side, по price-bucket).

## Schema vs Runtime (mismatch, см. open-questions.md §5)

Runtime использует поля типа `synthetic_id`, `venue_id`, `p_l`, `p_h`, `q_rate`, `q_max`, `curve_id`, `snapshot_id`, `created_at`, `expires_at`, `status`, `order_id`, `client_order_id`, `liquidity_source`. Snake_case в proto/code vs camelcase/concat в DDL. Перед T-F11-100 нужно решить нейминг (предпочтительно — snake_case с подчёркиваниями).

## Retention / backup

- **Retention:** активные — бессрочно (пока `active`), `expired` и `used` — 30 дней TTL (`DELETE WHERE status<>'active' AND createdat < now() - interval '30 days'`). См. cleanup-job (pending).
- **Backup:** общий PostgreSQL backup.

## Связанные таблицы

- [venue_config](venue-config.md) — FK по `venueid`.
- [venue_liquidity_curves](venue-liquidity-curves.md) — FK по `curveid` (cross-store, ClickHouse).
- [venue_snapshots](venue-snapshots.md) — FK по `snapshotid` (cross-store).

## Used In

- [F-11 acceptance criteria AC-11](../02-system/features/F-11-external-venues-lob-to-fob/acceptance-criteria.md)
- [UC-F11-03 Build VenueLiquidityCurve](../02-system/use-cases/UC-F11-03-build-liquidity-curve/use-case.md)
- [Kafka venue.synthetic](../06-api/messaging/venue-topics.md#venue-synthetic)
