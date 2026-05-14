---
id: DOC-DATA-OLTP
phase: 07-data
status: draft
owner: core-team
source:
  - IN-001 «БД 1: PostgreSQL (OLTP — оперативные данные)»
related:
  - docs/07-data/data-overview.md
  - docs/07-data/olap-schema.md
  - docs/04-domain/entities.md
---

# OLTP Schema (PostgreSQL)

PostgreSQL — **источник истины** для оперативных данных: пользователи, сессии, счета, активные заявки, позиции, лимиты, маржа, конфиги solver.

ACID-гарантии обязательны: ledger, risk_limits, flow_orders изменяются транзакционно.

## Таблица `users`

Учётные записи пользователей.

| Поле | Тип | Описание |
| --- | --- | --- |
| `user_id` | `UUID PRIMARY KEY` | Уникальный идентификатор |
| `email` | `VARCHAR UNIQUE NOT NULL` | Email для логина и уведомлений |
| `password_hash` | `VARCHAR NOT NULL` | bcrypt / argon2 |
| `display_name` | `VARCHAR` | Отображаемое имя |
| `role` | `ENUM('demo','client','provider','operator','admin')` | Роль |
| `kyc_status` | `ENUM('none','pending','verified','rejected')` | Статус KYC |
| `created_at` | `TIMESTAMPTZ NOT NULL` | Создание |
| `updated_at` | `TIMESTAMPTZ NOT NULL` | Обновление профиля |
| `is_active` | `BOOLEAN NOT NULL DEFAULT TRUE` | Активность |

**Сервисы-потребители:**

- **Auth & Identity** (R/W) — регистрация, логин, смена ролей.
- **API Gateway** (R) — авторизация запросов.
- **Risk Manager** (R) — определение набора лимитов по `role` и `kyc_status`.

## Таблица `sessions`

Активные сессии.

| Поле | Тип | Описание |
| --- | --- | --- |
| `session_id` | `UUID PRIMARY KEY` | |
| `user_id` | `UUID NOT NULL REFERENCES users` | |
| `token_hash` | `VARCHAR NOT NULL` | Хэш токена |
| `ip_address` | `INET` | IP создания |
| `created_at` | `TIMESTAMPTZ NOT NULL` | |
| `expires_at` | `TIMESTAMPTZ NOT NULL` | |
| `is_revoked` | `BOOLEAN NOT NULL DEFAULT FALSE` | |

**Сервисы-потребители:**

- **Auth & Identity** (R/W).
- **API Gateway** (R) — проверка токена.

## Таблица `accounts`

Счета клиентов (по одному на пару user + asset).

| Поле | Тип | Описание |
| --- | --- | --- |
| `account_id` | `UUID PRIMARY KEY` | |
| `user_id` | `UUID NOT NULL REFERENCES users` | |
| `asset` | `VARCHAR NOT NULL` | BTC, ETH, USDT, ... |
| `free_balance` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | Доступно для торговли |
| `reserved_balance` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | Зарезервировано под заявки |
| `venue_allocated` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | Размещено на внешних venue |
| `pending_transfer` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | В процессе перевода |
| `updated_at` | `TIMESTAMPTZ NOT NULL` | |
| | `UNIQUE (user_id, asset)` | |

**Сервисы-потребители:**

- **Collateral & Ledger** (R/W) — единственный writer.
- **Risk Manager** (R) — расчёт margin и pre-trade проверки.
- **API Gateway** (R) — отображение балансов клиенту.

## Таблица `flow_orders`

Потоковые заявки — основная бизнес-сущность.

| Поле | Тип | Описание |
| --- | --- | --- |
| `order_id` | `UUID PRIMARY KEY` | |
| `user_id` | `UUID NOT NULL REFERENCES users` | |
| `provider_type` | `ENUM('ui','api','provider','internal') NOT NULL` | |
| `provider_id` | `VARCHAR` | NULL для UI |
| `symbol` | `VARCHAR NOT NULL` | |
| `side` | `ENUM('buy','sell') NOT NULL` | |
| `portfolio_weights` | `JSONB` | NULL для одно-активных |
| `p_low` | `NUMERIC(38,18) NOT NULL` | |
| `p_high` | `NUMERIC(38,18) NOT NULL` | |
| `q_rate` | `NUMERIC(38,18) NOT NULL` | Скорость, units/sec |
| `q_max` | `NUMERIC(38,18) NOT NULL` | Максимальный объём |
| `filled_cum` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | Кумулятивный fill |
| `time_in_force` | `ENUM('GTC','GTD','IOC') NOT NULL` | |
| `window_start` | `TIMESTAMPTZ NOT NULL` | |
| `window_end` | `TIMESTAMPTZ` | NULL для GTC |
| `status` | `ENUM('new','active','partially_filled','filled','cancelled','expired','liquidated') NOT NULL` | |
| `created_at` | `TIMESTAMPTZ NOT NULL` | |
| `updated_at` | `TIMESTAMPTZ NOT NULL` | |

Индексы: `(user_id, status)`, `(symbol, status)`, `(status) WHERE status IN ('active','partially_filled')`.

**Сервисы-потребители:**

- **Matching Backend** (R/W) — основной writer (`filled_cum`, `status`).
- **Order Flow** (R/W) — создание/изменение/отмена.
- **Risk Manager** (R) — pre-trade и post-trade анализ.
- **Collateral & Ledger** (R) — расчёт резерва.
- **API Gateway** (R) — отображение клиенту.

## Таблица `positions`

Текущие позиции по инструментам.

| Поле | Тип | Описание |
| --- | --- | --- |
| `position_id` | `UUID PRIMARY KEY` | |
| `user_id` | `UUID NOT NULL REFERENCES users` | |
| `symbol` | `VARCHAR NOT NULL` | |
| `side` | `ENUM('long','short','flat') NOT NULL` | |
| `quantity` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | |
| `avg_entry_price` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | |
| `unrealized_pnl` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | Mark-to-market |
| `realized_pnl` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | |
| `updated_at` | `TIMESTAMPTZ NOT NULL` | |
| | `UNIQUE (user_id, symbol)` | |

**Сервисы-потребители:**

- **Collateral & Ledger** (R/W).
- **Risk Manager** (R) — margin, VAR, liquidation.
- **API Gateway** (R) — отображение.

## Таблица `risk_limits`

Лимиты по пользователям / ролям / символам.

| Поле | Тип | Описание |
| --- | --- | --- |
| `limit_id` | `UUID PRIMARY KEY` | |
| `entity_type` | `ENUM('user','role','symbol','global') NOT NULL` | |
| `entity_id` | `VARCHAR NOT NULL` | user_id / role / symbol / `'global'` |
| `max_notional` | `NUMERIC(38,18)` | |
| `max_position` | `NUMERIC(38,18)` | |
| `max_leverage` | `NUMERIC(10,4)` | |
| `max_order_rate` | `INT` | заявок/мин |
| `asset_whitelist` | `TEXT[]` | |
| `kill_switch` | `BOOLEAN NOT NULL DEFAULT FALSE` | |
| `updated_at` | `TIMESTAMPTZ NOT NULL` | |
| `updated_by` | `UUID REFERENCES users` | Кто изменил |

**Сервисы-потребители:**

- **Risk Manager** (R/W) — основной.
- **Matching Backend** (R) — kill_switch проверка перед batch.
- **API Gateway / Web UI** (R) — отображение operator'у.

## Таблица `risk_snapshots`

Снимки маржинального состояния (после каждого batch и важных событий).

| Поле | Тип | Описание |
| --- | --- | --- |
| `snapshot_id` | `UUID PRIMARY KEY` | |
| `entity_id` | `VARCHAR NOT NULL` | user_id или venue_id |
| `free_collateral` | `NUMERIC(38,18) NOT NULL` | |
| `reserved_collateral` | `NUMERIC(38,18) NOT NULL` | |
| `initial_margin` | `NUMERIC(38,18) NOT NULL` | |
| `maintenance_margin` | `NUMERIC(38,18) NOT NULL` | |
| `risk_flags` | `JSONB` | margin_call, liquidation, throttled |
| `timestamp` | `TIMESTAMPTZ NOT NULL` | |

Индекс: `(entity_id, timestamp DESC)`.

**Сервисы-потребители:**

- **Risk Manager** (W).
- **Collateral & Ledger** (R) — решения о liquidation / rebalance.
- **Observability & Reporting** (R) — dashboards.

## Таблица `collateral_transfers`

Операции перемещения средств.

| Поле | Тип | Описание |
| --- | --- | --- |
| `request_id` | `UUID PRIMARY KEY` | |
| `user_id` | `UUID NOT NULL REFERENCES users` | |
| `from_venue` | `VARCHAR NOT NULL` | internal / binance / onchain / ... |
| `to_venue` | `VARCHAR NOT NULL` | |
| `asset` | `VARCHAR NOT NULL` | |
| `amount` | `NUMERIC(38,18) NOT NULL` | |
| `reason` | `ENUM('deposit','withdrawal','rebalance','liquidation') NOT NULL` | |
| `priority` | `ENUM('low','normal','high','urgent') NOT NULL DEFAULT 'normal'` | |
| `status` | `ENUM('pending','processing','confirmed','failed','cancelled') NOT NULL` | |
| `created_at` | `TIMESTAMPTZ NOT NULL` | |
| `confirmed_at` | `TIMESTAMPTZ` | |

**Сервисы-потребители:**

- **Collateral & Ledger** (R/W).
- **Blockchain / Custody Adapter** (R/W) — обновление статуса.
- **Risk Manager** (R) — учёт `pending_transfer`.

## Таблица `solver_config`

Параметры работы solver matching.

| Поле | Тип | Описание |
| --- | --- | --- |
| `config_id` | `UUID PRIMARY KEY` | |
| `version` | `INT NOT NULL` | |
| `batch_interval_ms` | `INT NOT NULL` | |
| `max_iterations` | `INT NOT NULL` | |
| `epsilon_liquidity` | `NUMERIC(38,18) NOT NULL` | epsilon для роли «MM последней инстанции» |
| `tolerance` | `NUMERIC(38,18) NOT NULL` | |
| `fee_model` | `JSONB NOT NULL` | maker/taker, speed-dependent |
| `is_active` | `BOOLEAN NOT NULL DEFAULT FALSE` | |
| `updated_at` | `TIMESTAMPTZ NOT NULL` | |

Constraint: ровно один `is_active = TRUE` одновременно.

**Сервисы-потребители:**

- **Matching Backend** (R) — чтение active config перед каждым batch.
- **Backtest & Replay** (R) — подстановка альтернативных конфигов.

## Связанные документы

- [data-overview.md](data-overview.md) — карта хранилищ.
- [olap-schema.md](olap-schema.md) — ClickHouse.
- [data-flow.md](data-flow.md) — Kafka → DB flows.
- [../04-domain/entities.md](../04-domain/entities.md) — доменные сущности.

## Source Fragments

- IN-001-FR-029
