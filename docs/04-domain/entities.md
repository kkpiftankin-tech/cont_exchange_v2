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

## F-09 — Batch / Combo / Multi-leg Orders

Источники: IN-011 §4, §8, §15; [ADR-031](../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md), [ADR-032](../03-architecture/adr/ADR-032-parent-child-order-model.md), [ADR-033](../03-architecture/adr/ADR-033-execution-groups-topic.md). Деньги — `Decimal`.

### BatchOrder

Клиентский parent object, объединяющий несколько `ComboOrder`/conditional-ветвей/`FlowOrder`. **Не путать** с `Batch` (цикл клиринга F-04). Поля: `batch_order_id`, `user_id`, `account_id`, `type`, `execution_mode`, `status`, `time_window{start,end}`, `child_refs[]`, `correlation_id`. Persistence: [`batch_orders`](../07-data/oltp-schema.md). Инварианты: `len(child_refs)≥1`; `execution_mode` неизменяем после `active`; команды идемпотентны по `batch_order_id`; статус — однонаправленный DAG.

### ComboOrder

Многоногая заявка, ноги связаны общими условиями; нормализуется в `MultiLegVectorOrder`. Поля: `combo_order_id`, `batch_order_id?`, `combo_type` (pair/basket/spread/conditional/oco/bracket), `execution_mode`, `atomicity_policy`, `atomicity_scope`, `fallback_policy`, `min_execution_scale`, `max_ratio_deviation_bps`, `ratio_basis`, `legs[]`, `constraints[]`, `status`. Persistence: [`combo_orders`](../07-data/oltp-schema.md). Инварианты: `len(legs)≥2`; сохранение weights/ratio/spread ⇒ `execution_mode=multileg_vector_solver` (ADR-031); `strict_atomic` на внешних площадках только при `venue_native` или internal_batch.

### Leg

Один инструмент/сторона внутри многоногой заявки. В `multileg_vector_solver` **не** самостоятельная FlowOrder — компонент согласованного вектора \(e_g\). Поля: `leg_id`, `parent_order_id`, `instrument`, `side`, `ratio?`/`weight?`, `ratio_basis`, `p_low`, `p_high`, `q_rate`, `q_max`, `filled_cum`, `venue_preferences[]`, `status`. Persistence: [`combo_order_legs`](../07-data/oltp-schema.md). Инварианты: `0<p_low≤p_high`, `q_max>0`, `q_rate>0`, `0≤filled_cum≤q_max`, ровно одно из `{ratio,weight}` задано.

### MultiLegConstraint

Общее ограничение группы (строки матриц \(A_g\)/\(G_g\)). Поля: `constraint_id`, `parent_order_id`, `type` (ratio_equality/spread_range/max_total_notional/factor_neutrality/max_weight_deviation/max_leverage/margin_limit/risk_limit), `coefficients{symbol→Decimal}`, `lower?`, `upper?`, `value?`, `severity` (hard/soft). Persistence: [`combo_constraints`](../07-data/oltp-schema.md). Инвариант: `hard` блокирует исполнение при нарушении; `soft` фиксируется в `violated_constraints`.

### MultiLegVectorOrder

Техническая нормализованная форма ComboOrder для solver (не хранится отдельно; строится Matching на каждый batch cycle). Содержит `legs`, `ρ_g`, `A_g/b_g`, `G_g/h_g`, `atomicity_policy`, `min_execution_scale`, `feasible_caps`, `reference_prices`, spread `c_g/L_g/U_g`. Детерминизм: одинаковый `group_id`+`config_version`+`reference_prices`+`feasible_caps` ⇒ одинаковый `ExecutionGroup` (F-15 replay, AC-F09-010).

### ExecutionGroup

Результат grouped execution одного batch cycle для одного ComboOrder; единица атомарности. Публикуется в `execution.groups` (key `parent_order_id`). Поля: `execution_group_id` (idempotency key), `batch_id`, `parent_order_id`, `execution_mode`, `group_status`, `execution_scale` \(\alpha_g^*\), `atomicity_policy`, `atomicity_scope`, `fallback_action?`, `violated_constraints[]`, `ratio_deviation_bps`, `leg_results[{leg_id,exec_qty,exec_price,fill_id}]`, `solver_diagnostics{group_solve_time_ms,binding_legs[],binding_constraints[]}`, `created_at`. Persistence: [`execution_groups`](../07-data/oltp-schema.md). Инварианты: `execution_scale∈[0,1]`; strict_atomic+cancelled ⇒ scale=0; повторная доставка по `execution_group_id` не дублирует проводки; фиксируется в `group_state_transitions` до публикации `LegFill`.

### LegFill

FillEvent по ноге, часть grouped execution (не самостоятельное исполнение). Публикуется в `fills` с доп. полями `parent_order_id`, `execution_group_id`, `leg_id`, `group_policy`, `liquidity_source`. Инварианты: `exec_qty>0`; `p_low≤exec_price≤p_high`; существует только при наличии родительского `ExecutionGroup`; Σ`exec_qty` ноги по всем группам ≤ `q_max`.

### Статусы (value objects)

- **ParentOrderStatus** (§15.1): `draft, risk_pending, active, waiting_for_trigger, partially_filled, filled, cancelled, expired, degraded, rollback_pending, rolledback, rejected` — однонаправленный DAG.
- **LegStatus** (§15.2): `inactive, active, waiting_for_trigger, partially_filled, filled, cancelled, blocked_by_group, blocked_by_atomicity, failed_external, compensated`.
- **ExecutionGroupStatus** (§15.3): `filled, partial, waiting_next_batch, cancelled_by_atomicity, degraded, compensating, rollback_pending, rolledback, failed`.

Доменная математика, политики и инварианты — [business-rules.md §F-09](business-rules.md#f-09--batch--combo--multi-leg-orders).

## Source Fragments

- IN-001-FR-008 — структуры данных и сервисы
- IN-001-FR-007 — risk/ledger сущности
- IN-011 §4, §8, §15 — F-09 batch/combo/multi-leg entities
