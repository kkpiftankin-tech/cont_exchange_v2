# marketdata_snapshots — ClickHouse

## Owner

`market-data` (Market Data Service)

## Purpose

Хранение исторических снимков рыночных данных после каждого batch-клиринга и обновлений от внешних источников.
Используется для REST history API, post-trade аналитики (F-13) и replay (F-15).

## Feature

[F-05 Live Market Data](../02-system/features/F-05-live-market-data/)

## Engine

ClickHouse `MergeTree`

## DDL

```sql
CREATE TABLE IF NOT EXISTS marketdata_snapshots (
    snapshot_id    UUID,
    asset          String,
    mid            Decimal(38, 18),
    best_bid       Decimal(38, 18),
    best_ask       Decimal(38, 18),
    spread         Decimal(38, 18),
    spread_bps     Decimal(38, 8),
    volume_24h     Decimal(38, 18),
    volume_quote_24h Decimal(38, 18),
    bid_depth      String,   -- JSON: [{price, qty}, ...]
    ask_depth      String,   -- JSON: [{price, qty}, ...]
    clear_price    Nullable(Decimal(38, 18)),
    executed_rate  Nullable(Decimal(38, 18)),
    source         Enum8('internal'=1, 'cex'=2, 'dex'=3, 'composite'=4),
    stale          UInt8 DEFAULT 0,
    batch_id       String,
    timestamp      DateTime64(3, 'UTC')
)
ENGINE = MergeTree
ORDER BY (asset, timestamp)
TTL toDateTime(timestamp) + INTERVAL 90 DAY
SETTINGS index_granularity = 8192;
```

## Schema

| Поле | Тип | Описание |
|------|-----|----------|
| `snapshot_id` | UUID | Уникальный идентификатор снимка |
| `asset` | String | Инструмент, напр. `BTCUSDT` |
| `mid` | Decimal(38,18) | Mid-price = (bestBid + bestAsk) / 2 |
| `best_bid` | Decimal(38,18) | Лучшая цена покупки |
| `best_ask` | Decimal(38,18) | Лучшая цена продажи |
| `spread` | Decimal(38,18) | bestAsk − bestBid |
| `spread_bps` | Decimal(38,8) | Спред в базисных пунктах |
| `volume_24h` | Decimal(38,18) | Суточный объём в базовой валюте |
| `volume_quote_24h` | Decimal(38,18) | Суточный объём в котировочной валюте |
| `bid_depth` | String (JSON) | Массив `[{price, qty}]` по уровням bid |
| `ask_depth` | String (JSON) | Массив `[{price, qty}]` по уровням ask |
| `clear_price` | Decimal(38,18) | Последняя цена клиринга из BatchResult |
| `executed_rate` | Decimal(38,18) | Скорость исполнения из последнего батча |
| `source` | Enum8 | `internal` / `cex` / `dex` / `composite` |
| `stale` | UInt8 | 1 если данные устаревшие (fallback) |
| `batch_id` | String | Связь с BatchResult |
| `timestamp` | DateTime64(3) | UTC метка снимка |

## Retention

TTL 90 дней.

## Indexes / Ordering

`ORDER BY (asset, timestamp)` — оптимизирует запросы по инструменту и диапазону времени.

## Producers

- `market-data` — после `ComputeMarketData` и `updateComposite`

## Consumers

- `market-data` — REST `/api/v1/marketdata/{asset}/history`
- `market-data` — `GetReferencePrices` (fallback path)
- `observability` / `F-13` — post-trade аналитика

## Related

- [effective-spreads.md](effective-spreads.md)
- [marketdata-config.md](marketdata-config.md)
- [../06-api/grpc/marketdata-get-reference-prices.md](../06-api/grpc/marketdata-get-reference-prices.md)
