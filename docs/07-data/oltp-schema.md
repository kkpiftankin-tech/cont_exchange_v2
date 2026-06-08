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

Потоковые заявки — основная бизнес-сущность. Канонический DDL — в [`infra/postgres/init.sql`](../../infra/postgres/init.sql); поля ниже отражают то, что реально читает/обновляет [PostgresFlowOrderRepository](../../cpp/matching/src/infra/postgres/postgres_flow_order_repository.cpp).

| Поле | Тип | Описание |
| --- | --- | --- |
| `order_id` | `UUID PRIMARY KEY DEFAULT gen_random_uuid()` | |
| `user_id` | `TEXT NOT NULL` | |
| `p_low` | `NUMERIC(38,18) NOT NULL` | |
| `p_high` | `NUMERIC(38,18) NOT NULL` | |
| `q_rate` | `NUMERIC(38,18) NOT NULL` | Скорость, units/sec |
| `q_max` | `NUMERIC(38,18) NOT NULL` | Максимальный объём |
| `filled_cum` | `NUMERIC(38,18) NOT NULL DEFAULT 0` | Кумулятивный fill |
| `time_in_force` | `TEXT NOT NULL` | `'GTC' \| 'GTD' \| 'IOC'` |
| `status` | `TEXT NOT NULL` | `'new' \| 'active' \| 'partially_filled' \| 'filled' \| 'cancelled' \| 'rejected' \| 'expired'` |
| `window_start` | `TIMESTAMPTZ NOT NULL` | |
| `window_end` | `TIMESTAMPTZ` | NULL для GTC |
| `created_at` | `TIMESTAMPTZ NOT NULL DEFAULT NOW()` | |
| `updated_at` | `TIMESTAMPTZ NOT NULL DEFAULT NOW()` | |

CHECK-ограничения: `q_max > 0`, `q_rate > 0`, `filled_cum BETWEEN 0 AND q_max`, `p_low > 0 AND p_high >= p_low`, `window_end IS NULL OR window_end > window_start`.

Индексы: `idx_flow_orders_active_window` (partial: `status, window_start, window_end WHERE status IN ('active','partially_filled')`), `idx_flow_orders_user_id`.

### Таблица `flow_order_legs`

Per-asset веса для портфельных / multi-leg FlowOrder. Single-asset ордер имеет 1 строку с `weight = 1`.

| Поле | Тип | Описание |
| --- | --- | --- |
| `order_id` | `UUID NOT NULL REFERENCES flow_orders(order_id) ON DELETE CASCADE` | |
| `instrument_symbol` | `TEXT NOT NULL` | напр. `BTCUSDT` |
| `weight` | `NUMERIC(38,18) NOT NULL` | |
| | `PRIMARY KEY (order_id, instrument_symbol)` | |

Индекс: `idx_flow_order_legs_symbol (instrument_symbol)`.

**Conflict Note (C-FO-LEGS).** Ранняя doc-версия секции `flow_orders` упоминала колонку `portfolio_weights JSONB` для портфельных ордеров. Реализованная в dev-ветке схема (импортирована в этом PR) хранит legs в отдельной таблице `flow_order_legs` — нормализованный вариант, удобнее для индексирования по `instrument_symbol` и для конкурентных UPDATE.

**Сервисы-потребители:**

- **Matching Backend** (R/W) — основной writer (`filled_cum`, `status`).
- **Order Flow** (R/W) — создание/изменение/отмена (writer ещё не подключён в main; T-F04-002).
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

Параметры работы solver matching. Канонический DDL — в [`infra/postgres/init.sql`](../../infra/postgres/init.sql); поля ниже отражают то, что реально читает [PostgresSolverConfigRepository](../../cpp/matching/src/infra/postgres/postgres_solver_config_repository.cpp).

| Поле | Тип | Описание |
| --- | --- | --- |
| `version` | `INTEGER PRIMARY KEY` | |
| `batchintervalms` | `INTEGER NOT NULL` | период batch-clearing |
| `maxiterations` | `INTEGER NOT NULL` | стоп solver'а |
| `epsilonliquidity` | `DOUBLE PRECISION NOT NULL` | epsilon для роли «MM последней инстанции» |
| `tolerance` | `DOUBLE PRECISION NOT NULL` | порог `residualNorm` |
| `feemodel` | `JSONB NOT NULL DEFAULT '{}'::jsonb` | maker/taker, speed-dependent |
| `isactive` | `BOOLEAN NOT NULL DEFAULT FALSE` | |
| `created_at` | `TIMESTAMPTZ NOT NULL DEFAULT NOW()` | |

Constraint: ровно одна строка с `isactive = TRUE` (партиальный unique-индекс `solver_config_one_active`).

Seed (init.sql): version=1 со значениями `batchintervalms=1000, maxiterations=128, epsilonliquidity=0.00001, tolerance=0.0001, feemodel={"makerfeerate":0.0002,"takerfeerate":0.0005}, isactive=TRUE`.

**Conflict Note (C-SC-NAMING).** Ранняя doc-версия использовала snake_case (`batch_interval_ms`, `is_active`, `max_iterations`). Реализованная в dev-ветке схема использует слитное написание (`batchintervalms`, `isactive`, `maxiterations`) — так SQL-запросы в репозитории не зависят от снейк-кейса. Doc выровнен под код.

**Сервисы-потребители:**

- **Matching Backend** (R) — чтение active config перед каждым batch.
- **Backtest & Replay** (R) — подстановка альтернативных конфигов (через replay_solver_configs из cpp/backtest, future PR).

## F-09: Batch/Combo/Multi-leg Orders (OLTP)

> Source: IN-011 (F-09 v2 corrected §8, §14.1, §15), [ADR-032](../03-architecture/adr/ADR-032-parent-child-order-model.md), [ADR-033](../03-architecture/adr/ADR-033-execution-groups-topic.md).
> Owner-сервис: `order_flow` (создание/нормализация), `matching` (execution_groups, group_state_transitions).
> Полный DDL — в `infra/postgres/init.sql` (блок F-09, пишет devops #14). Здесь — описание полей.

Семь таблиц образуют трёхуровневую иерархию `batch_orders` → `combo_orders` → `combo_order_legs`, плюс поперечные таблицы ограничений, графа состояний и аудита: `combo_constraints`, `conditional_links`, `execution_groups`, `group_state_transitions`.

### Таблица `batch_orders`

Клиентский parent-объект, объединяющий несколько `ComboOrder`/conditional-ветвей/`FlowOrder`. **Не путать** с `Batch` (цикл клиринга F-04).

| Поле | Тип | Описание |
| --- | --- | --- |
| `batch_order_id` | `UUID PK` | Идентификатор BatchOrder |
| `user_id` | `TEXT NOT NULL` | Владелец |
| `account_id` | `TEXT NOT NULL` | Счёт |
| `order_type` | `TEXT` | `batch\|combo\|basket\|spread\|conditional\|oco\|bracket` |
| `execution_mode` | `TEXT` | `orchestration_only\|multileg_vector_solver` |
| `status` | `TEXT` | §15.1: draft/risk_pending/active/waiting_for_trigger/partially_filled/filled/cancelled/expired/degraded/rollback_pending/rolledback/rejected |
| `time_window_start/end` | `TIMESTAMPTZ` | Окно активности (CHECK end>start) |
| `created_at/updated_at` | `TIMESTAMPTZ` | |

Индексы: `(user_id)`; partial `(status, created_at DESC)` для активных. **Writers:** order_flow, matching. **Readers:** order_flow, matching, risk, gateway.

### Таблица `combo_orders`

Многоногая заявка с общим `executionMode`, `atomicityPolicy`, политиками.

| Поле | Тип | Описание |
| --- | --- | --- |
| `combo_order_id` | `UUID PK` | Идентификатор ComboOrder |
| `batch_order_id` | `UUID FK→batch_orders` | Родитель (NULL = автономный) |
| `combo_type` | `TEXT` | pair/basket/spread/conditional/oco/bracket/factor/budget |
| `execution_mode` | `TEXT` | orchestration_only/multileg_vector_solver |
| `status` | `TEXT` | §15.1 |
| `ratio_basis` | `TEXT` | notional_weight/quantity_ratio/NULL |
| `atomicity_policy` | `TEXT` | strict_atomic/scalable_atomic/best_effort/sequential_fallback/external_compensating |
| `atomicity_scope` | `TEXT` | internal_batch/venue_native/external_compensating/none |
| `fallback_policy` | `TEXT` | scale_down/wait_next_batch/cancel/degrade/compensate |
| `min_execution_scale` | `NUMERIC(38,18)` | 0..1, NULL = нет |
| `max_ratio_deviation_bps` | `INTEGER` | NULL = нет |

Индексы: `(batch_order_id)`; partial `(status, updated_at DESC)`. **Writers:** order_flow, matching.

### Таблица `combo_order_legs`

Параметры одной ноги ComboOrder с полным CSLO-профилем. **Отдельная** таблица (не расширение `flow_order_legs`, см. рекомендацию ниже / ADR-032).

| Поле | Тип | Описание |
| --- | --- | --- |
| `leg_id` | `UUID PK` | Идентификатор ноги |
| `parent_order_id` | `UUID FK→combo_orders` | Родительский ComboOrder |
| `instrument_symbol` | `TEXT NOT NULL` | Инструмент |
| `side` | `TEXT` | buy/sell |
| `ratio` / `weight` | `NUMERIC(38,18)` | одно из двух NOT NULL (CHECK) |
| `ratio_basis` | `TEXT` | переопределение базы веса |
| `p_low` / `p_high` | `NUMERIC(38,18)` | CHECK p_low>0, p_high≥p_low |
| `q_rate` / `q_max` | `NUMERIC(38,18)` | CHECK >0 |
| `filled_cum` | `NUMERIC(38,18)` | CHECK 0≤filled_cum≤q_max |
| `venue_preferences` | `TEXT[]` | источники ликвидности |
| `status` | `TEXT` | §15.2: inactive/active/waiting_for_trigger/partially_filled/filled/cancelled/blocked_by_group/blocked_by_atomicity/failed_external/compensated |

Индексы: `(parent_order_id)`, `(instrument_symbol)`, partial `(status)`.

### Таблица `combo_constraints`

Общие ограничения (ratio/spread/budget/factor/margin/risk).

| Поле | Тип | Описание |
| --- | --- | --- |
| `constraint_id` | `UUID PK` | |
| `parent_order_id` | `UUID FK→combo_orders` | |
| `constraint_type` | `TEXT` | max_weight_deviation/max_total_notional/spread_range/factor_neutrality/max_leverage/max_margin/risk_limit/ratio_equality |
| `coefficients` | `JSONB` | `{symbol: coeff}` для spread/factor |
| `lower_bound` / `upper_bound` | `NUMERIC(38,18)` | NULL = ∞ |
| `value` / `value_bps` | `NUMERIC(38,18)` / `INTEGER` | абсолютный лимит / в bps |
| `severity` | `TEXT` | hard/soft |

### Таблица `conditional_links`

Рёбра графа активации/отмены (OCO/bracket/conditional).

| Поле | Тип | Описание |
| --- | --- | --- |
| `link_id` | `UUID PK` | |
| `parent_order_id` | `UUID FK→combo_orders` | |
| `from_leg_id` / `to_leg_id` | `UUID FK→combo_order_legs` | CHECK from≠to |
| `link_type` | `TEXT` | oco/bracket/conditional |
| `condition` | `JSONB` | условие перехода (NULL = безусловная OCO-отмена) |

### Таблица `execution_groups`

Результат grouped solve одного batch cycle для одного ComboOrder. `execution_group_id` — idempotency key для ledger (ADR-033).

| Поле | Тип | Описание |
| --- | --- | --- |
| `execution_group_id` | `UUID PK` | idempotency key |
| `batch_id` | `TEXT NOT NULL` | цикл клиринга |
| `parent_order_id` | `UUID FK→combo_orders` | |
| `execution_mode` | `TEXT` | |
| `group_status` | `TEXT` | §15.3: filled/partial/waiting_next_batch/cancelled_by_atomicity/degraded/compensating/rollback_pending/rolledback/failed |
| `execution_scale` | `NUMERIC(38,18)` | α, CHECK 0..1 |
| `atomicity_policy` / `atomicity_scope` | `TEXT` | применённые в цикле |
| `fallback_action` | `TEXT` | NULL если не применялось |
| `ratio_deviation_bps` | `INTEGER` | фактическое отклонение |
| `leg_results` | `JSONB` | `[{legId, execQty, execPrice, fillId}]` |
| `violated_constraints` | `JSONB` | нарушенные constraint_id |
| `solver_diagnostics` | `JSONB` | `{groupSolveTimeMs, bindingLegs[], bindingConstraints[]}` |

**Writer:** только `matching`. **Readers:** ledger (idempotent by `execution_group_id`), order_flow, risk, observability, backtest.

### Таблица `group_state_transitions`

Аудит переходов состояния + идемпотентность повторных Kafka-событий (ADR-032, ADR-020).

| Поле | Тип | Описание |
| --- | --- | --- |
| `id` | `UUID PK` | |
| `group_id` | `UUID FK→execution_groups` | |
| `from_status` / `to_status` | `TEXT` | |
| `batch_id` | `TEXT` | NULL для ручных переходов |
| `reason` | `TEXT` | код перехода |
| `idempotency_key` | `TEXT UNIQUE` | dedup повторных доставок |
| `created_at` | `TIMESTAMPTZ` | |

### Рекомендация: `combo_order_legs` vs `flow_order_legs`

ADR-032 оставлял open question. **Рекомендация — отдельная таблица `combo_order_legs`** (не расширение `flow_order_legs`):

1. `flow_order_legs` имеет PK `(order_id, instrument_symbol)` и только `weight`; добавление NOT NULL CSLO-полей сломало бы CHECK F-02 и `PostgresFlowOrderRepository`.
2. В `multileg_vector_solver` ноги combo **не** самостоятельные FlowOrder (ADR-031) — FK на `flow_orders` семантически неверен.
3. В `orchestration_only` каждая нога **создаёт** отдельную FlowOrder, и `flow_order_legs` заполняется естественно; `combo_order_legs` хранит исходное намерение для parent-reporting.
4. Допустимое дублирование данных в `orchestration_only` — приемлемая цена изоляции уровней.

## Связанные документы

- [data-overview.md](data-overview.md) — карта хранилищ.
- [olap-schema.md](olap-schema.md) — ClickHouse.
- [data-flow.md](data-flow.md) — Kafka → DB flows.
- [../04-domain/entities.md](../04-domain/entities.md) — доменные сущности.

## Source Fragments

- IN-001-FR-029
- IN-011 §8, §14.1, §15 (F-09 batch/combo/multi-leg tables)
