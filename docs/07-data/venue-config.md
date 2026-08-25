---
id: DOC-DATA-VENUE-CONFIG
phase: 07-data
status: draft
owner: core-team
source:
  - IN-004 §«PostgreSQL: venue_config»
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/06-api/rest/venues.md
  - cpp/venues/src/infra/postgres_venue_config_repository.cpp
---

# PostgreSQL: `venue_config`

Конфигурация подключений к внешним площадкам (CEX/DEX/AMM). Источник истины для External Venues Connector при старте и в hot-reload.

## Owner / Access

- **Сервис-владелец:** [cpp/venues](../05-components/external-venues-connector/overview.md) — R/W через [PostgresVenueConfigRepository](../../cpp/venues/src/infra/postgres_venue_config_repository.cpp).
- Hot reload: `VenuesLoop::UpsertVenueConfig` применяет изменения без рестарта.
- Admin API: [REST /api/v1/venues](../06-api/rest/venues.md).

## DDL (canonical)

```sql
CREATE TYPE venuetype_enum AS ENUM ('cex', 'dex', 'amm');

CREATE TABLE venue_config (
  venueid                     VARCHAR(32) PRIMARY KEY,
  venuetype                   venuetype_enum,            -- 'cex'|'dex'|'amm'
  displayname                 VARCHAR(128),
  apiurl                      TEXT,
  wsurl                       TEXT,
  symbols                     JSONB,                     -- набор торгуемых пар
  pollingintervalms           INT,
  reconnectattempts           INT,
  reconnectdelayms            INT,
  fees                        JSONB,                     -- {maker, taker}
  ticksize                    JSONB,                     -- per-symbol
  lotsize                     JSONB,                     -- per-symbol
  lobtofobmodel               JSONB,                     -- {level, tau_sec, regularization, ...}
  isactive                    BOOLEAN NOT NULL DEFAULT FALSE,
  createdat                   TIMESTAMPTZ NOT NULL DEFAULT now(),
  updatedat                   TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS venue_config_is_active_idx ON venue_config (isactive);
```

## Schema notes (поля из реального cpp/venues)

`PostgresVenueConfigRepository::VenueConfigRow` ([postgres_venue_config_repository.hpp](../../cpp/venues/src/infra/postgres_venue_config_repository.hpp)) использует более плоский набор полей, чем DDL выше:

| Runtime поле                  | DDL аналог                       | Примечание                                        |
| ----------------------------- | -------------------------------- | ------------------------------------------------- |
| `venue_id`                    | `venueid`                        | PK                                                |
| `adapter_mode`                | (часть `lobtofobmodel` JSONB)    | `cex_ws_rest`/`dex_amm_rpc`/`simulator`           |
| `ws_url`                      | `wsurl`                          |                                                   |
| `rest_base_url`               | `apiurl`                         | для CEX                                           |
| `rpc_url`                     | новое поле / в `apiurl`          | для DEX/AMM                                       |
| `chain_id`                    | новое                            | EVM chain id для DEX                              |
| `pool_address`                | новое                            | для AMM                                           |
| `venue_symbol`                | первая запись из `symbols` JSONB | torgovyj instrument                               |
| `depth_levels`                | новое                            | сколько уровней снимать                           |
| `curve_level`                 | `lobtofobmodel.level`            | `L1`/`L2`/`L3`                                    |
| `synthetic_enabled`           | новое                            | публиковать ли `venue.synthetic`                  |
| `stale_threshold_ms`          | новое                            | per-venue override `STALE_THRESHOLD_MS`           |
| `circuit_breaker_enabled`     | новое                            |                                                   |
| `circuit_breaker_errors`      | новое                            | per-venue override `CIRCUIT_BREAKER_ERRORS`       |
| `circuit_breaker_window_ms`   | новое                            | per-venue override                                |
| `circuit_breaker_cooldown_ms` | новое                            | per-venue override                                |
| `is_active`                   | `isactive`                       |                                                   |
| `routing_mode`                | новое (`auto`/`watch`)           | runtime hint для Execution Planning               |
| `updated_at_ms`               | `updatedat`                      |                                                   |

> **Conflict Note:** DDL в IN-004 и runtime row в cpp/venues расходятся — формальная DDL богаче runtime в части JSONB-полей (`fees`, `ticksize`, `lotsize`), но беднее в части per-venue circuit-breaker/stale параметров. Перед T-F11-100 нужно решить:
>
> - либо расширить DDL флагами `*_ms`/`*_enabled` и `routing_mode`;
> - либо хранить эти настройки внутри `lobtofobmodel` JSONB;
> - либо ввести вторую таблицу `venue_runtime_overrides`.

## Retention / backup

- **Retention:** бессрочно (audit + onboarding history).
- **Backup:** PostgreSQL логические бэкапы (см. F-14 / общий план backup в [08-infrastructure](../08-infrastructure/)).
- **Migrations:** DDL — в `infra/postgres/init.sql` (currently не добавлен, см. T-F11-100).

## Связанные таблицы

- [synthetic_orders](synthetic-orders.md) — children по `venueid` (FK `synthetic_orders.venueid → venue_config.venueid`).

## Used In

- [F-11 acceptance criteria AC-19 (hot reload)](../02-system/features/F-11-external-venues-lob-to-fob/acceptance-criteria.md)
- [UC-F11-01 Onboard venue](../02-system/use-cases/UC-F11-01-onboard-venue/use-case.md)
- [REST /api/v1/venues](../06-api/rest/venues.md)
