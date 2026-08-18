---
id: DOC-DOMAIN-ENTITIES
phase: 04-domain
status: draft
owner: core-team
related:
  - specs/domain/entities.yaml
  - contracts/proto/fob/
---

# Domain Entities

Сводный обзор сущностей. Подробные поля и связи — в [../../specs/domain/entities.yaml](../../specs/domain/entities.yaml).

## Базовые value objects

- **Decimal** — `units * 10^(-scale)`. Proto: `fob.common.v1.Decimal`.
- **Side** — BUY / SELL.
- **Instrument** — symbol + base + quote.
- **EventMeta** — общие поля события: id, ts, source, correlation_id.

## Ордерный домен

- **FlowOrder** — заявка с диапазоном цен, скоростью, объёмом, окном. Proto: `fob.orders.v1.FlowOrder`.
- **OrdersNormalized** — Kafka-событие команды (create/cancel/amend).

## Matching домен

- **BatchResult** — результат батч-клиринга. Proto: `fob.matching.v1.BatchResult`.
- **Fill** — fill внутри батча. Proto: `fob.matching.v1.Fill`.
- **OrderUpdate** — дельта состояния ордера после батча.
- **BatchDiagnostics** — `residual_norm`, `solve_time_ms`, `num_active_orders`.

## Ledger / Risk

- **Balance** — `available` / `reserved` по паре (user, currency).
- **Reservation** — резерв под ордер, идемпотентен.
- **Position** — текущая позиция (long/short/flat).
- **PreTradeCheckRequest/Response** — gRPC контракты risk.
- **RiskAlert** — Kafka-событие в `risk.alerts`.

## Market data

- **MarketDataRaw** — поток сырых данных (Ticker / Trade / OrderBookL2Update).
- **Ticker** — последний bid/ask/last по паре.

## Execution

- **ExecutionIntent** — намерение хедж-сделки на внешнем venue.
- **ExecutionReport** — отчёт о её исполнении.

См. таблицу полей в [../../specs/domain/entities.yaml](../../specs/domain/entities.yaml).

## Поля сущностей из IN-001 §8

### FlowOrder (бизнес-форма)

`order_id`, `user_id`, `provider_type` (ui/api/provider/internal), `provider_id`, `symbol`, `side` (buy/sell), `portfolio_weights` (для портфельных), `p_low`, `p_high`, `q_rate`, `q_max`, `filled_cum`, `time_in_force` (GTC/GTD/IOC), `window_start`, `window_end`, `status` (new/active/partially_filled/filled/cancelled/expired/liquidated), `created_at`, `updated_at`.

Persistence: [`flow_orders`](../07-data/oltp-schema.md#таблица-flow_orders).

### BatchRequest / BatchResult

`batch_id`, `timestamp`, `clear_prices` (symbol → price), `executed_rates` (order_id → rate), `residual_norm`, `solve_time_ms`, `num_active_orders`, `solver_diagnostics`, `config_version`.

Persistence: [`batch_results`](../07-data/olap-schema.md#таблица-batch_results) в ClickHouse.

### FillEvent

`fill_id`, `batch_id`, `order_id`, `user_id`, `symbol`, `side`, `asset_legs` (для портфельных), `exec_qty`, `exec_price`, `liquidity_source` (internal/cex_hedge/dex_hedge/epsilon_mm), `fees`, `timestamp`.

Persistence: [`fills`](../07-data/olap-schema.md#таблица-fills) в ClickHouse.

### RiskSnapshot

`snapshot_id`, `entity_id` (user/venue), `free_collateral`, `reserved_collateral`, `initial_margin`, `maintenance_margin`, `risk_flags` (margin_call/liquidation/throttled), `timestamp`.

Persistence: [`risk_snapshots`](../07-data/oltp-schema.md#таблица-risk_snapshots) в PostgreSQL.

### AgentLog (state–action–reward)

`log_id`, `agent_id`, `policy_version`, `mode` (live/shadow/replay), `batch_id`, `observation_blob`, `action_blob`, `reward_components`, `outcome_blob`, `timestamp`.

Persistence: [`agent_logs`](../07-data/olap-schema.md#таблица-agent_logs) в ClickHouse.

### CollateralTransfer

`request_id`, `user_id`, `from_venue`, `to_venue`, `asset`, `amount`, `reason` (deposit/withdrawal/rebalance/liquidation), `priority`, `status` (pending/processing/confirmed/failed/cancelled), `created_at`, `confirmed_at`.

Persistence: [`collateral_transfers`](../07-data/oltp-schema.md#таблица-collateral_transfers).

## Live Venue Simulator (F-20, IN-010)

Сущности F-20. Feature — [F-20](../02-system/features/F-20-live-venue-simulator/README.md).
Деньги/цены — `Decimal` (§9).

### VenueSimulator

Ядро симуляции: принимает `ChildOrderRequest`, использует **живой** `VenueSnapshot`
(F-11), делает LEVEL_BY_LEVEL matching и применяет модели, генерирует
`SimExecutionReport`. Параметры: `simSessionId`, `venueId`, `latencyModel`, `impactModel`,
`feeModel`, `rejectionModel`, `staleLobThresholdMs`, `partialFillMode`.

### VenueSimRouter

Маршрутизатор child-ордеров между `VenueSimulator` и реальным EVC. Единственная точка
переключения; режимы `SIM_ONLY` / `LIVE_ONLY` / `SHADOW` (dual-send). Venue Execution
Adapter не знает о режиме.

### SimSession

Конфигурационная сессия (PG `sim_sessions`): `routingMode`, `scopeVenues[]`,
`scopeInstruments[]`, конфиги моделей (JSONB), `staleLobThresholdMs`, `partialFillMode`,
`status` (ACTIVE/PAUSED/COMPLETED/CANCELLED).

### ChildOrderRequest

Стандартный запрос child-ордера (F-12 → Router), режим-независимый: `childOrderId`,
`hedgeFlowId`, `venueId`, `symbol`, `side`, `orderType`, `qty`, `price?`, `timeInForce`,
`clientOrderId`, `simSessionId?`.

### SimExecutionReport

`ExecutionReport` (F-12) + sim-поля: `simMode=true`, `simSessionId`, `lobSnapshotId`,
`lobAge`, `impactBps`, `slippageBps`, `latencySampleMs`; `rejectReason ∈
{SIM_STALE_LOB, SIM_NO_LIQUIDITY, SIM_RANDOM_REJECT, SIM_TIMEOUT}`.

### Модели: LatencyModel / ImpactModel / FeeModel / RejectionModel

- **LatencyModel** — `distribution (LOGNORMAL|UNIFORM|FIXED|EMPIRICAL)`, `p50/p95/p99Ms`, `tailProbability`, `timeoutMs`.
- **ImpactModel** — `modelType (LINEAR|SQRT|POWER_LAW|LEVEL_BY_LEVEL)`, `impactCoeff`, `powerExponent`, `depletionMode`, `depletionDecayMs`.
- **FeeModel** — maker/taker/gas (совместим с форматом F-15).
- **RejectionModel** — `insufficientLiquidityEnabled`, `priceConstraintEnabled`, `randomRejectionRate`, `rateLimitRejectRate`, `minLiquidityThreshold`.

Persistence: PG [`sim_sessions`](../07-data/sim-sessions.md), CH
[`sim_execution_reports`/`sim_divergence_log`](../07-data/sim-execution-reports.md).
Базовый (наивный) симулятор уже реализован — `cpp/venues/src/app/venues_loop.cpp`.

## Source Fragments

- IN-010 — F-20 Live Venue Simulator (VenueSimulator/Router/SimSession/модели)
- IN-001-FR-008 — структуры данных и сервисы
- IN-001-FR-007 — risk/ledger сущности
