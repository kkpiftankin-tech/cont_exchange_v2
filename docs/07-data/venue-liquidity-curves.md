---
id: DOC-DATA-VENUE-LIQUIDITY-CURVES
phase: 07-data
status: draft
owner: core-team
source:
  - IN-004 §«ClickHouse: venue_liquidity_curves»
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/06-api/messaging/venue-topics.md
  - cpp/venues/src/app/liquidity_curve_producer.cpp
---

# ClickHouse: `venue_liquidity_curves`

История построенных VenueLiquidityCurve (LOB → FOB). Используется backtest/replay (F-15), quality dashboard (F-17), и для офлайн-калибровки L3.

## Owner / Access

- **Producer:** [cpp/venues / LiquidityCurveProducer](../../cpp/venues/src/app/liquidity_curve_producer.cpp) публикует в Kafka `venue.liquidity.fob`.
- **Ingestion path:** Kafka engine MV (planned T-F11-110, аналогично F-04 `fills`/`batchresults`).
- **Readers:** backtest (F-15), quality reports, ML calibration.

## DDL (canonical)

```sql
CREATE TYPE venue_curve_side AS ENUM ('buy', 'sell');         -- логически; в CH см. Enum8
CREATE TYPE venue_curve_level AS ENUM ('L1', 'L2', 'L3');

CREATE TABLE IF NOT EXISTS venue_liquidity_curves (
  curveid         UUID,
  venueid         String,
  symbol          String,
  side            Enum8('buy'=1, 'sell'=2),
  level           Enum8('L1'=1, 'L2'=2, 'L3'=3),
  tau_sec         Float64,
  q_grid          String,                  -- JSON array<Float64>
  p_of_q          String,                  -- JSON array<Float64>
  s_of_q          String,                  -- JSON array<Float64>
  l_of_v          String,                  -- JSON array<Float64>
  epsilon1        Float64,
  epsilon2        Float64,
  epsilon3        Float64,
  confidence      Float64,
  snapshotid      UUID,                    -- линк на venue_snapshots
  schema_version  UInt32,
  producer_version String,
  createdat       DateTime64(3, 'UTC'),
  INDEX idx_venue_symbol (venueid, symbol) TYPE minmax GRANULARITY 1
) ENGINE = MergeTree
  PARTITION BY toYYYYMMDD(createdat)
  ORDER BY (venueid, symbol, side, createdat)
  TTL toDateTime(createdat) + INTERVAL 90 DAY DELETE;
```

## Поля

| Поле               | Тип             | Описание                                                  |
| ------------------ | --------------- | --------------------------------------------------------- |
| `curveid`          | `UUID`          | Уникальный ID кривой                                      |
| `venueid`          | `String`        |                                                           |
| `symbol`           | `String`        |                                                           |
| `side`             | `Enum8`         | `buy`/`sell`                                              |
| `level`            | `Enum8`         | `L1`/`L2`/`L3` (effective; может отличаться от requested) |
| `tau_sec`          | `Float64`       | Временной масштаб $\tau$                                  |
| `q_grid`           | `String (JSON)` | Сетка объёмов                                             |
| `p_of_q`           | `String (JSON)` | $p(q)$ — предельная цена                                  |
| `s_of_q`           | `String (JSON)` | $S(q)$ — интегральная стоимость                            |
| `l_of_v`           | `String (JSON)` | FOB-кривая по скорости                                     |
| `epsilon1`         | `Float64`       | Ошибка стоимости                                          |
| `epsilon2`         | `Float64`       | Ошибка монотонности                                       |
| `epsilon3`         | `Float64`       | Ошибка исполнения (L3)                                     |
| `confidence`       | `Float64`       | Доверие к кривой ∈ [0, 1]                                  |
| `snapshotid`       | `UUID`          | FK на `venue_snapshots`                                   |
| `schema_version`   | `UInt32`        | Версия payload-схемы (для F-15)                            |
| `producer_version` | `String`        | Версия producer'а (`cpp-venues/0.1`)                       |
| `createdat`        | `DateTime64(3)` | Время публикации                                          |

## Retention

- 90 дней через `TTL` (см. NFR из IN-004).

## Запросы

- Quality drift по venue (epsilon1 поверх 24h окна):

  ```sql
  SELECT venueid, symbol, avg(epsilon1) AS avg_eps1, quantileExact(0.95)(epsilon1) AS p95_eps1
  FROM venue_liquidity_curves
  WHERE createdat >= now() - INTERVAL 24 HOUR
  GROUP BY venueid, symbol
  ORDER BY p95_eps1 DESC;
  ```

- Backtest replay по схеме:

  ```sql
  SELECT * FROM venue_liquidity_curves
  WHERE schema_version = 1 AND venueid = 'binance' AND symbol = 'BTCUSDT'
    AND createdat BETWEEN ? AND ?
  ORDER BY createdat;
  ```

## Связанные таблицы

- [venue_snapshots](venue-snapshots.md) — FK по `snapshotid`.
- [synthetic_orders (PostgreSQL)](synthetic-orders.md) — FK по `curveid` (cross-store).

## Известные несоответствия

- **Ingestion ещё не подключён.** Producer пишет в Kafka, но Kafka engine MV для `venue_liquidity_curves` отсутствует — T-F11-110.

## Used In

- [F-11 acceptance criteria AC-10, AC-20, AC-26](../02-system/features/F-11-external-venues-lob-to-fob/acceptance-criteria.md)
- [UC-F11-03 Build VenueLiquidityCurve](../02-system/use-cases/UC-F11-03-build-liquidity-curve/use-case.md)
- [Kafka venue.liquidity.fob](../06-api/messaging/venue-topics.md#venue-liquidity-fob)
