---
id: DOC-DATA-VENUE-SNAPSHOTS
phase: 07-data
status: draft
owner: core-team
source:
  - IN-004 §«ClickHouse: venue_snapshots»
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/06-api/messaging/venue-topics.md
  - cpp/venues/src/infra/snapshot_clickhouse_writer.cpp
---

# ClickHouse: `venue_snapshots`

История нормализованных снимков ордербуков. Retention ≥ 90 дней (NFR из IN-004).

## Owner / Access

- **Writer:** [cpp/venues / SnapshotClickHouseWriter](../../cpp/venues/src/infra/snapshot_clickhouse_writer.cpp).
- **Readers:**
  - Backtest/Replay (F-15).
  - Operator UI (F-16) через aggregations.
  - Quality assurance (`Testing/f11_test4_quality.sh`).
- **Ingestion path:** прямой HTTP-insert из `cpp/venues` (writer). Альтернативная стратегия — Kafka engine MV (см. T-F11-110, аналогично F-04 ingestion).

## DDL (canonical)

```sql
CREATE TABLE IF NOT EXISTS venue_snapshots (
  snapshotid    UUID,
  venueid       String,
  symbol        String,
  timestamp     DateTime64(3, 'UTC'),
  midprice      Float64,
  bestbid       Float64,
  bestask       Float64,
  spread        Float64,
  volume24h     Float64,
  biddepth      String,                       -- JSON array of {price, qty}
  askdepth      String,                       -- JSON array of {price, qty}
  fees          String,                       -- JSON {maker, taker}
  ticksize      Float64,
  lotsize       Float64,
  status        Enum8('connected'=1, 'stale'=2, 'disconnected'=3, 'empty'=4),
  INDEX idx_venue_symbol (venueid, symbol) TYPE minmax GRANULARITY 1
) ENGINE = MergeTree
  PARTITION BY toYYYYMMDD(timestamp)
  ORDER BY (venueid, symbol, timestamp)
  TTL toDateTime(timestamp) + INTERVAL 90 DAY DELETE;
```

## Поля

| Поле          | Тип             | Описание                                                |
| ------------- | --------------- | ------------------------------------------------------- |
| `snapshotid`  | `UUID`          | Уникальный ID снимка (FK из `synthetic_orders.snapshotid`) |
| `venueid`    | `String`        | `binance`, `coinbase`, `uniswap_v3`, ...                |
| `symbol`      | `String`        | Торговый инструмент в нотации venue                     |
| `timestamp`   | `DateTime64(3)` | UTC; partition key                                      |
| `midprice`    | `Float64`       | $(bestBid + bestAsk) / 2$                               |
| `bestbid`     | `Float64`       | Лучшая цена покупки                                     |
| `bestask`     | `Float64`       | Лучшая цена продажи                                     |
| `spread`      | `Float64`       | $bestAsk - bestBid$                                     |
| `volume24h`   | `Float64`       | Объём 24h (base units)                                  |
| `biddepth`    | `String`        | JSON массив `[{price, qty}, ...]`                       |
| `askdepth`    | `String`        | JSON массив `[{price, qty}, ...]`                       |
| `fees`        | `String`        | JSON `{maker, taker}`                                   |
| `ticksize`    | `Float64`       | Шаг цены                                                |
| `lotsize`     | `Float64`       | Шаг объёма                                              |
| `status`      | `Enum8`         | `connected`/`stale`/`disconnected`/`empty`              |

> **Note about Float64 for money.** В отличие от OLTP, где используется `NUMERIC(24,8)`, ClickHouse-OLAP допускает `Float64` для аналитики/диагностики. См. §9 CLAUDE.md — `double` допустим для OLAP, но **не** для ledger/settlement.

## Retention

- 90 дней через `TTL` clause (`VENUES_CLICKHOUSE_RETENTION_DAYS=90`).
- Партиционирование по дню (`toYYYYMMDD`) — упрощает drop старых партиций.

## Запросы

- Last snapshot per venue/symbol:

  ```sql
  SELECT * FROM venue_snapshots
  WHERE venueid = 'binance' AND symbol = 'BTCUSDT'
  ORDER BY timestamp DESC LIMIT 1;
  ```

- Stale-rate за 5 минут:

  ```sql
  SELECT venueid, countIf(status='stale') / count() AS stale_rate
  FROM venue_snapshots
  WHERE timestamp >= now() - INTERVAL 5 MINUTE
  GROUP BY venueid;
  ```

- VWAP-приближение из `biddepth`/`askdepth` (через `JSONExtract`) — поддерживается; пример см. Testing/f11_test4_quality.sh.

## Связанные таблицы

- [venue_liquidity_curves](venue-liquidity-curves.md) — `curveid → snapshotid` (трассировка).
- [synthetic_orders (PostgreSQL)](synthetic-orders.md) — `snapshotid`.

## Известные несоответствия

- **DDL отсутствует в `infra/clickhouse/init.sql`** (там пока только F-04 `batchresults`, `fills`). Writer `EnsureSchema()` пытается создать таблицу при старте — приемлемо для dev, но для prod нужна явная миграция (T-F11-110).

## Used In

- [F-11 acceptance criteria AC-2, AC-4](../02-system/features/F-11-external-venues-lob-to-fob/acceptance-criteria.md)
- [UC-F11-02 Publish VenueSnapshot](../02-system/use-cases/UC-F11-02-publish-snapshot/use-case.md)
- [Kafka venue.snapshots](../06-api/messaging/venue-topics.md#venue-snapshots)
