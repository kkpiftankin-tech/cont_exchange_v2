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

## Continuous-Order Market Primitives (IN-012)

> Математическая основа matching engine. Canonical reference:
> [`incoming-docs/2026-04-15-continuous-order-market-academic-v1.md`](../../incoming-docs/2026-04-15-continuous-order-market-academic-v1.md)
> (PDF в чате, 44 страницы). См. также
> [ADR-035](../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md)
> и [solver-foundation.md](../09-implementation/solver-foundation.md).

### State variables (IN-012 §3.1)

| Field | Type | Description |
| --- | --- | --- |
| `Q_i` | `ℝ^n` | Текущая позиция агента `i` по `n` активам. |
| `Q̇_i` | `ℝ^n` | Мгновенная скорость изменения позиции. |
| `P` | `ℝ^n` | Рыночная цена (вектор по активам). |

Знаковая конвенция: линейный член `−aq̇` — **signed external anchor**.
Все формулы invariant к "buy/sell" переименованию при последовательном
использовании одной конвенции.

### Four equivalent curve representations (IN-012 §3.2)

```text
CurveRepresentation = enum {
  V_form  — прямая «цена → скорость»: V_i(Q_i, P) = arg max_v {⟨P, v⟩ − L_i(Q_i, v)}
  P_form  — обратная «скорость → цена»: P ∈ ∂_{Q̇_i} L_i(Q_i, Q̇_i)
  L_form  — лагранжиан L_i(Q_i, Q̇_i)
  H_form  — гамильтониан H_i(Q_i, P) = sup_v {⟨P, v⟩ − L_i(Q_i, v)}
}
```

**Invariant**: при strict convexity `L_i(Q_i, ·)` все четыре формы
эквивалентны (Предложение 3.2 IN-012), `V_i = ∇_P H_i`,
`P_i = ∇_{Q̇_i} L_i`, кривые `V` и `P` взаимно обратны.

### Standard agent classes

Каждый "класс агента" — конкретная параметризация `L_i`. Используется
matching engine при построении aggregated `H(Q, P)`.

#### 1D agents (IN-012 §5.1)

| Class | `L(q̇)` | `H(p)` | Use case |
| --- | --- | --- | --- |
| **StandardAgent** | `(m/2)q̇² + a q̇`, `m > 0` | `(p − a)² / 2m` | Базовый flow. Используется F-04. |
| **InfiniteInertiaAgent** | `I_{0}(q̇)` | `0` | Холдер; m → ∞ предел. |
| **PerfectLiquidityAgent** | `a q̇` | `0 if p=a, +∞ else` | Infinite-depth market-maker; m → 0. |
| **LinearFeeAgent** | `(m/2)q̇² + a q̇ + c\|q̇\|`, `c > 0` | `(max{\|p−a\|−c, 0})² / 2m` | Агент с комиссией → no-trade region `[a−c, a+c]`. |

#### 2D agents (IN-012 §5.2)

Матрица инерции `M_ρ` с корреляцией `ρ ∈ [−1, 1]`:

```text
M_ρ = | m_1                 ρ √(m_1 m_2) |
      | ρ √(m_1 m_2)        m_2          |
```

| Class | Constraint | Use case |
| --- | --- | --- |
| **StandardAgent2D** | `\|ρ\| < 1` | Двумерная книга со взаимной liquidity. |
| **DegenerateInPrice2D** | `m_2 = ∞`, `ρ = 0` | Торгует только активом 1. |
| **DegenerateInSpeed2D** | `m_2 = 0`, `ρ = 0` | Идеально ликвиден по активу 2 при `p_2 = a_2`. |
| **IndependentAgent2D** | `ρ = 0` | Две независимые книги. |
| **AbsoluteComplementAgent2D** | `ρ ↑ 1` | Совместное движение активов. |
| **AbsoluteSubstituteAgent2D** | `ρ ↓ −1` | Маркетит relative price / spread. |
| **LinearFeeAgent2D** | + `c_1\|q̇_1\| + c_2\|q̇_2\|` | No-trade region — прямоугольник. |
| **PortfolioAgent** | `H(P) = (1/2μ)(w^⊤ P − a)²` | **MM по индексу / спреду / корзине** с factor `w`. F-10 canonical. |

### Aggregate market (IN-012 §4)

```text
H(Q, P)  = Σ_i H_i(Q_i, P)                          (canonical sum)
L(Q, Q̇) = inf_{Σ_i v_i = Q̇}  Σ_i L_i(Q_i, v_i)   (infimal convolution)
V(Q, P)  = Σ_i V_i(Q_i, P)
```

**Clearing price** `P*(Q)` — три эквивалентных характеристики:

1. **Flow conservation**: `Σ_i Q̇_i(Q_i, P*) = 0`.
2. **Lagrange multiplier**: `P* ∈ ∂_Q̇ L(Q, 0)`.
3. **Hamiltonian minimum**: `P*(Q) ∈ arg min_P H(Q, P)`.

См. формализацию инвариантов в
[business-rules.md §Clearing Mechanics](business-rules.md#clearing-mechanics-in-012).

### Multi-asset replication theorem (IN-012 §6.3.1)

Любой quadratic market `H(P) = (1/2)(P − A)^⊤ Λ (P − A)`, `Λ ≻ 0`,
реализуется `n + r` агентами:

- `n` одноактивных `StandardAgent`-ов: `H_k(P) = (d_k/2)(p_k − A_k)²`.
- `r` факторных `PortfolioAgent`-ов: `H_{n+ℓ}(P) = (1/2)((u^(ℓ))^⊤(P − A))²`.

через разложение `Λ = D + UU^⊤`, `D = diag(d_1,…,d_n) ⪰ 0`. Минимальный
`r*(Λ) = min rank(Λ − D)`. В 2D `r*(Λ) ≤ 1` ⇒ **3 агента достаточно**.

Conceptual foundation для F-10 (MM curves через `PortfolioAgent`) и
N-leg combo solver F-09.

## Source Fragments

- IN-001-FR-008 — структуры данных и сервисы
- IN-001-FR-007 — risk/ledger сущности
- IN-011 §4, §8, §15 — F-09 batch/combo/multi-leg entities
- IN-012 §3, §5, §6 — Continuous-Order Market Primitives (fragments
  F-01, F-02, F-12..F-17, F-20 в
  [IN-012.fragment-map.md](../../incoming-docs/IN-012.fragment-map.md))
