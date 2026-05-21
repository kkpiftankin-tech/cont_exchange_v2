---
id: DOC-DOMAIN-BUSINESS-RULES
phase: 04-domain
status: draft
owner: core-team
source:
  - IN-005 §6 «Формулы расчётов» (F-12 Execution Hedge)
related:
  - docs/02-system/features/F-12-execution-hedge/
  - docs/04-domain/entities.md
  - docs/04-domain/ubiquitous-language.md
---

# Business Rules — Continuous Exchange / Flow Order Book

Единое место для бизнес-правил и формул расчётов. Каждая формула привязана к фиче и проверяется тестами.

## Финансовая точность

Все суммы денег, объёмов, цен — `fob.common.v1.Decimal` (см. [common.proto](../../contracts/proto/fob/common/v1/common.proto)). `double` допустим только для диагностики (residualNorm, solveTimeMs) и метрик. См. [CLAUDE.md §9](../../CLAUDE.md).

---

## F-04 Batch Clearing

См. [features/F-04-batch-clearing/](../02-system/features/F-04-batch-clearing/) и [matching-fob-core/overview.md](../05-components/matching-fob-core/overview.md). Формулы VWAP и инвариант диапазона цен описаны в [CLAUDE.md §8.5 и §8.4](../../CLAUDE.md).

---

## F-12 Execution Hedge

Источник: IN-005 §6.

### Hedge Trigger

После batch clearing (F-04) для каждого инструмента / провайдера:

$$
\text{triggerNotional} = |\text{netQty}| \times \text{clearingPrice}
$$

$$
\text{trigger} = \big(|\text{netQty}| \geq \text{thresholdQty}[\text{symbol}]\big) \; \text{OR} \; \big(\text{triggerNotional} \geq \text{thresholdNotional}\big)
$$

Если `trigger == true`, формируется `ExecutionIntent` с `target_qty = |netQty|`, `side = sign(netQty)` (BUY если netQty > 0, SELL если < 0).

Параметры (`thresholdQty[symbol]`, `thresholdNotional`) задаются в `hedge_policy_config` (PostgreSQL, планируется); fallback из env.

Реализация: [`cpp/matching/src/app/hedge_trigger_policy.cpp`](../../cpp/matching/src/app/hedge_trigger_policy.cpp).

### targetNotional

$$
\text{targetNotional} = \text{targetQty} \times \text{referenceMid}
$$

`referenceMid` берётся из BatchResult clearing price для текущего symbol (cross-link к F-04).

### Routing Plan

Для набора healthy venues (`venue.health.status = CONNECTED` и `latency_ms <= maxLatency[urgency]`):

$$
\text{qty}[v] = \frac{L(v)}{\sum_{v' \in V_{\text{healthy}}} L(v')} \cdot \text{targetQty}
$$

где $L(v)$ — суммарная ликвидность venue $v$ в нужном направлении (`bid` для SELL, `ask` для BUY), извлекаемая из `SideLiquidityCurve` ([fob.venue.v1.SideLiquidityCurve](../../contracts/proto/fob/venue/v1/venue.proto)) от F-11 топика `venue.liquidity.fob`.

Дополнительные ограничения:
- $\text{qty}[v]$ округляется к `lot_size[venue]` через `floor`.
- $\text{qty}[v] \leq \text{maxOrderSize}[venue]$ (берётся из VenueSnapshot).

Реализация: целевая в Execution Planning (см. [05-components/execution-planning/](../05-components/execution-planning/overview.md), интерфейс [`IExecutionPlanningUseCases`](../../cpp/matching/src/app/execution_planning_uc.hpp)).

### Pre-hedge Risk Check (×3)

Перед отправкой child orders Execution Planning вызывает `RiskService.PreHedgeCheck` (см. [06-api/grpc/risk-pre-hedge-check.md](../06-api/grpc/risk-pre-hedge-check.md)). Три проверки:

1. **Notional limit:** $\quad \text{targetNotional} \leq \text{maxNotionalPerHedge}$
2. **Exposure limit:** $\quad \text{currentHedgeExposure}[\text{symbol}] + \text{targetQty} \leq \text{hedgeExposureLimit}[\text{symbol}]$
3. **Slippage limit:** $\quad \text{expectedSlippage} \leq \text{maxSlippage}[\text{urgency}]$

Если хотя бы одна проверка не прошла — `PreHedgeCheckResponse.ok = false`, HedgeFlow помечается `RISK_REJECTED`, publish `risk.alerts(HEDGE_REJECTED, reason)`.

`maxSlippage[urgency]` по умолчанию:

| urgency | max slippage |
| --- | --- |
| LOW | 5 bps |
| MEDIUM | 15 bps |
| HIGH | 30 bps |

### Child Order Price (с tick rounding)

Базовая цена:

$$
\text{rawPrice} = \text{referenceMid} \pm \delta
$$

где $\delta$ — passive offset (`+\delta` для SELL, `-\delta` для BUY) при `urgency = LOW`; для MEDIUM / HIGH $\delta = 0$ (price = referenceMid или market).

Округление к `tick_size`:

$$
\text{price} = \text{round}\!\left(\frac{\text{rawPrice}}{\text{tickSize}}\right) \times \text{tickSize}
$$

`tickSize` берётся из VenueSnapshot для конкретного `(venue, instrument)`.

### Child Order Quantity (с lot rounding)

$$
\text{childQty} = \min\!\left(\left\lfloor \frac{\text{qty}[v]}{\text{lotSize}} \right\rfloor \times \text{lotSize},\; \text{maxOrderSize}\right)
$$

`lotSize`, `maxOrderSize` — из VenueSnapshot.

### slippageBps

Считается per-fill после получения ExecutionReport:

$$
\text{slippageBps} = \frac{|\text{avgPrice} - \text{referenceMid}|}{\text{referenceMid}} \times 10^4
$$

Реализация: входит в payload `ExecutionReport.slippage_bps` (вычисляется Adapter перед публикацией).

### avgFillPrice (VWAP по fills)

Агрегированная цена по всем fills HedgeFlow:

$$
\text{avgFillPrice} = \frac{\sum_i \text{filledQty}_i \times \text{avgPrice}_i}{\sum_i \text{filledQty}_i}
$$

где $i$ — fills внутри `child_orders`, относящиеся к одному `hedge_flow_id`.

Записывается в `hedgeflows.avg_fill_price`.

### remainingQty

$$
\text{remainingQty} = \text{targetQty} - \text{filledQty}
$$

Где `filledQty = sum(child_orders.filled_qty WHERE hedge_flow_id = X)`.

### Overfill Guard

$$
\text{overfill} = \text{accumulatedFilledQty} - \text{targetQty}
$$

$$
\text{guard\_triggered} = \text{overfill} > \text{overfillThreshold}
$$

`overfillThreshold` по умолчанию: `0.005 * targetQty` (0.5%). При срабатывании Adapter:
1. Отменяет все open child orders (`CancelOrder` через EVC).
2. Публикует `ExecutionReport(status=OVERFILL_GUARD, reason=overfill_race_condition)`.
3. HedgeFlow → terminal (status зависит от политики: COMPLETED или REJECTED).

### Reconciliation Gap

$$
\text{gap} = \text{targetQty} - \text{filledQty}
$$

После истечения `hedgeTimeoutMs` или completion всех child_orders:
- $\text{gap} \leq \text{reconciliationGapThreshold}$ → status=COMPLETED.
- $\text{gap} > \text{reconciliationGapThreshold}$ → если есть fallback venue → retry с `urgency = HIGH`; иначе status=UNDERFILLED + `risk.alerts(HEDGE_UNDERFILL)`.

`reconciliationGapThreshold` по умолчанию: `0.0001 * targetQty` (0.01%, см. F12-N3).

### HedgePnL

Realized PnL на момент завершения HedgeFlow.

**SELL** (продали на venue, получили quote):

$$
\text{hedgePnL} = (\text{avgFillPrice} - \text{referenceMid}) \times \text{filledQty} - \text{feesTotal}
$$

**BUY** (купили на venue, потратили quote):

$$
\text{hedgePnL} = (\text{referenceMid} - \text{avgFillPrice}) \times \text{filledQty} - \text{feesTotal}
$$

Интерпретация: положительный hedgePnL значит, что биржа исполнила хедж лучше, чем внутренний clearingPrice (зашли с положительным capture).

Реализация: целевая в [`cpp/ledger/src/app/ledger_uc.cpp`](../../cpp/ledger/src/app/ledger_uc.cpp) `ApplyExecutionReport` (планируется).

### FillRatio

$$
\text{FillRatio} = \frac{\text{filledQty}}{\text{targetQty}} \times 100\%
$$

Используется в Reconciliation Alerts dashboard.

### GapAbs / GapPct

$$
\text{GapAbs} = \text{targetQty} - \text{filledQty}
$$

$$
\text{GapPct} = \frac{\text{GapAbs}}{\text{targetQty}} \times 100\%
$$

### Post-trade Risk Update

После каждого ExecutionReport (FILLED / PARTIALLY_FILLED) Risk Manager обновляет:

$$
\text{currentHedgeExposure}[\text{symbol}] \mathrel{{-}{=}} \text{filledQty} \times \text{sign}(\text{side})
$$

где `sign(BUY) = +1`, `sign(SELL) = -1`. Снижение absolute exposure при правильном хедже.

Источник: IN-005 §6.

---

## Связь с F-04 / F-11 / F-15

- **F-04 → F-12:** `referenceMid` в ExecutionIntent = `BatchResult.clear_prices[symbol]`.
- **F-11 → F-12:** Routing plan использует `SideLiquidityCurve` из топика `venue.liquidity.fob`.
- **F-12 → F-06:** ExecutionReport обновляет `positions` через Settlement Ledger.
- **F-15:** Backtest должен дать identical HedgePnL при тех же входах через VenueSim.

---

## F-15 — Backtest / Replay

См. [features/F-15-backtest-replay/](../02-system/features/F-15-backtest-replay/).
Полная реализация формул — в [replay_step_journal.cpp](../../cpp/backtest/src/app/replay_step_journal.cpp).

### Базовые рыночные величины (per batch)

$$
\text{mid} = \frac{\text{bestBid} + \text{bestAsk}}{2}
$$

$$
\text{spread} = \text{bestAsk} - \text{bestBid}
$$

`mid` используется как `decisionPrice` для IS. При отсутствии полного
quote — fallback на `clear_price` (см. `decision_price_source` в
[replay_summaries](../07-data/replay-summaries.md)).

### Incremental и cumulative PnL

$$
\Delta PnL_i = PnL^{after}_i - PnL^{before}_i, \qquad
\text{totalPnL} = \sum_i \Delta PnL_i
$$

$$
\text{cumPnL}(t) = \sum_{i \le t} \Delta PnL_i
$$

$PnL^{before}_i$ берётся из `AgentState` предыдущего батча через
[RestoreState UC](../05-components/backtest-service/overview.md).

### Implementation Shortfall

Простая форма:

$$
IS = \text{execPrice} - \text{decisionPrice}
$$

Объёмно-взвешенная (текущий MVP, `avgis_rule = 'volume_weighted'`):

$$
IS_i^{buy} = (P^{avg}_i - \text{decisionPrice}_i) \cdot Q^{exec}_i
$$

$$
IS_i^{sell} = (\text{decisionPrice}_i - P^{avg}_i) \cdot Q^{exec}_i
$$

Aggregated:

$$
\text{avgIS} = \frac{\sum_i IS_i}{\sum_i Q^{exec}_i}
$$

### VWAP

$$
\text{VWAP} = \frac{\sum_j Q^{exec}_j \cdot P^{exec}_j}{\sum_j Q^{exec}_j}
$$

### Fill Rate

$$
\text{FillRate} = \frac{\sum_i Q^{exec}_i}{\sum_i Q^{\max}_i} \times 100\%
$$

Граничный случай: при $\sum_i Q^{\max}_i = 0$ полагается `fill_rate = 0`
и устанавливается флаг `no_requested_volume = true` (F15-34).

### Sharpe ratio (per-batch increments)

$$
\text{Sharpe} = \frac{E[\Delta PnL]}{\sigma(\Delta PnL)}
$$

Граничный случай: при $\sigma(\Delta PnL) = 0$ — `Sharpe = 0` (F15-33;
см. [replay_summary_aggregation_test.cpp](../../cpp/backtest/tests/replay_summary_aggregation_test.cpp)).

### Max Drawdown

$$
\text{MaxDD} = \max_t \big(\,\text{peak}(t) - \text{cumPnL}(t)\,\big),
\qquad
\text{peak}(t) = \max_{s \le t} \text{cumPnL}(s)
$$

### Reward modes

- `pnl` — $r_i = \Delta PnL_i$.
- `-is` — $r_i = -IS_i$ (минимизация IS как награда).
- `hybrid` — $r_i = \alpha \cdot \Delta PnL_i + \beta \cdot (-IS_i)$, веса $\alpha, \beta$ из snapshot.

Default `pnl`. Режим фиксируется в `session_config_snapshot` (F15-26).

### AgentState (state JSON в `replay_agentlogs.state_json`)

Snapshot **до** батча.

```json
{
  "session_id": "rpl-abc-123",
  "batch_seq": 42,
  "event_time_ms": 1739361720000,
  "cum_pnl": 152.30,
  "open_positions": [
    {
      "symbol": "BTCUSDT",
      "qty": 0.05,
      "avg_price": 60100.0,
      "unrealized_pnl": 12.5
    }
  ],
  "free_margin": 9800.50,
  "used_margin": 199.50,
  "mid":    {"BTCUSDT": 60150.0},
  "spread": {"BTCUSDT": 1.2},
  "active_flow_orders_count": 3,
  "session_config_snapshot_version": 1
}
```

### Action (action JSON в `replay_agentlogs.action_json`)

Действие политики на этом батче — какие FlowOrder активны и какие
control-events генерируются. Для MVP стратегия пассивна, FlowOrder
создаются один раз при старте replay'а; `events.kind = "noop"`. RL-агент
в будущем будет генерировать `create_order` / `cancel_order` /
`amend_order` events.

```json
{
  "active_orders": [
    {
      "order_id": "ord-001",
      "symbol": "BTCUSDT",
      "side": "buy",
      "pL": 58000,
      "pH": 62000,
      "qrate": 0.5,
      "qmax_remaining": 95.0
    }
  ],
  "events": [{"kind": "noop"}]
}
```

### ShadowPositions (per-session, in-memory)

Структура `cpp/backtest/src/infra/in_memory_shadow_ledger.cpp` под ключом
`shadow:<session_id>:<account_id>`:

```json
{
  "account_id": "acct-001",
  "namespace": "shadow:rpl-abc-123",
  "balances": {
    "USDT": {"free": 9800.50, "reserved": 199.50},
    "BTC":  {"free": 0.05,    "reserved": 0.00}
  },
  "open_positions": [
    {
      "symbol": "BTCUSDT",
      "qty": 0.05,
      "avg_price": 60100.0,
      "fees_paid": 1.50,
      "cum_realized_pnl": 152.30
    }
  ],
  "shadow_only": true,
  "last_batch_seq": 42
}
```

Инвариант: `shadow_only=true` означает что namespace не имеет влияния на
production Collateral Ledger.

### Инварианты F-15

1. $\text{progress\_batches} \le \text{total\_batches}$.
2. При `solver_error_flag=1` или `risk_status='rejected'`: `fills_applied=0`.
3. $\text{processed\_batches} = N - \text{failed\_batches}$ при completed.
4. $\sum_i Q^{exec}_i \le \sum_i Q^{\max}_i$.
5. Для каждой fill: $\min(p_L) \le P^{exec} \le \max(p_H)$ (наследуется от F-04).
6. Детерминизм: одинаковый `session_config_snapshot` + одинаковый
   `random_seed` + одинаковая входная история → одинаковые
   `replay_agentlogs` (F15-16).
7. `partial=true` ⇔ статус сессии в `{cancelled, failed}` и хотя бы один
   батч обработан.

## Source Fragments

- IN-005 §6 «Формулы расчётов» (все 15 формул F-12)
- IN-005 §1 (термины ExecutionIntent, HedgeFlow, ChildOrder, ExecutionReport, Urgency)
- IN-006 § Канонические сущности и термины, Метрики качества исполнения (F-15)
