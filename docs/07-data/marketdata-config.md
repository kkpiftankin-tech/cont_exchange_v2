# marketdata_config — PostgreSQL

## Owner

`market-data` (Market Data Service)

## Purpose

Конфигурация Market Data Service per инструмент: интервалы публикации, глубина рынка,
окно агрегации объёма, внешние источники и kill-switch.

## Feature

[F-05 Live Market Data](../02-system/features/F-05-live-market-data/)

## Engine

PostgreSQL

## DDL

```sql
CREATE TABLE IF NOT EXISTS marketdata_config (
    asset                  VARCHAR(32) PRIMARY KEY,
    snapshot_interval_ms   INTEGER     NOT NULL DEFAULT 1000,
    depth_levels           INTEGER     NOT NULL DEFAULT 10,
    volume_window_sec      INTEGER     NOT NULL DEFAULT 86400,
    spread_alert_threshold_bps DECIMAL(10,4) NOT NULL DEFAULT 50.0,
    stale_threshold_sec    INTEGER     NOT NULL DEFAULT 30,
    external_sources       JSONB       NOT NULL DEFAULT '[]',
    is_active              BOOLEAN     NOT NULL DEFAULT TRUE,
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
```

## Schema

| Поле | Тип | Описание |
|------|-----|----------|
| `asset` | VARCHAR(32) PK | Инструмент, напр. `BTCUSDT` |
| `snapshot_interval_ms` | INTEGER | Интервал публикации снимков (мс). Default 1000. |
| `depth_levels` | INTEGER | Количество ценовых уровней в bid/ask depth. Default 10. |
| `volume_window_sec` | INTEGER | Окно агрегации объёма в секундах. Default 86400 (24h). |
| `spread_alert_threshold_bps` | DECIMAL | Порог спреда для генерации risk alert. Default 50 bps. |
| `stale_threshold_sec` | INTEGER | Максимальный возраст внутреннего снимка до fallback. Default 30. |
| `external_sources` | JSONB | Список внешних источников `[{venue, priority}]`. |
| `is_active` | BOOLEAN | Kill-switch: false → прекратить публикацию по инструменту. |
| `updated_at` | TIMESTAMPTZ | Метка последнего изменения. |

## Kill-switch

При `is_active = false`:
- Прекратить публикацию MarketDataSnapshot в Kafka и WebSocket.
- WebSocket-клиентам отправить сообщение `{type: "paused", asset: "..."}`.
- REST GET продолжает работать (возвращает последний известный snapshot с флагом `stale: true`).

## Seed data (dev)

```sql
INSERT INTO marketdata_config (asset, snapshot_interval_ms, depth_levels, is_active)
VALUES
  ('BTCUSDT', 500,  10, TRUE),
  ('ETHUSDT', 500,  10, TRUE),
  ('SOLUSDT', 1000, 5,  TRUE)
ON CONFLICT (asset) DO NOTHING;
```

## Related

- [marketdata-snapshots.md](marketdata-snapshots.md)
- [../02-system/features/F-05-live-market-data/feature.yaml](../02-system/features/F-05-live-market-data/feature.yaml)
