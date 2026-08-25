# effective_spreads — ClickHouse

## Owner

`market-data` (Market Data Service)

## Purpose

Хранение effective spread для каждого FillEvent — используется для post-trade отчётности (F-13),
оценки качества исполнения и расчёта Implementation Shortfall.

## Feature

[F-05 Live Market Data](../02-system/features/F-05-live-market-data/)

## Engine

ClickHouse `MergeTree`

## DDL

```sql
CREATE TABLE IF NOT EXISTS effective_spreads (
    fill_id              UUID,
    asset                String,
    exec_price           Decimal(38, 18),
    mid_at_exec          Decimal(38, 18),
    effective_spread     Decimal(38, 18),
    effective_spread_bps Decimal(38, 8),
    batch_id             String,
    timestamp            DateTime64(3, 'UTC')
)
ENGINE = MergeTree
ORDER BY (asset, timestamp)
TTL toDateTime(timestamp) + INTERVAL 365 DAY
SETTINGS index_granularity = 8192;
```

## Schema

| Поле | Тип | Описание |
|------|-----|----------|
| `fill_id` | UUID | Ссылка на FillEvent.fill_id |
| `asset` | String | Инструмент |
| `exec_price` | Decimal(38,18) | Цена исполнения из FillEvent |
| `mid_at_exec` | Decimal(38,18) | Mid-price на момент исполнения (из кэша) |
| `effective_spread` | Decimal(38,18) | 2 × \|execPrice − midAtExec\| |
| `effective_spread_bps` | Decimal(38,8) | effective_spread / mid × 10000 |
| `batch_id` | String | Связь с BatchResult |
| `timestamp` | DateTime64(3) | UTC метка FillEvent |

## Формула

```
effectiveSpread    = 2 * |execPrice - midAtExec|
effectiveSpreadBps = effectiveSpread / midAtExec * 10000
```

## Retention

TTL 365 дней.

## Producers

- `market-data` — после получения FillEvent из Kafka `fills`

## Consumers

- `observability` — агрегирует avg/min/max для дашборда
- `F-13 post-trade report` — использует для отчётов о качестве исполнения

## Related

- [marketdata-snapshots.md](marketdata-snapshots.md)
- [../02-system/features/F-05-live-market-data/acceptance-criteria.md](../02-system/features/F-05-live-market-data/acceptance-criteria.md)
- [../02-system/features/F-13-posttrade-report/](../02-system/features/F-13-posttrade-report/)
