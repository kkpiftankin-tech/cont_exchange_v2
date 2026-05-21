# UC-F12-01. Auto Hedge After Batch (Happy Path)

## Feature

- [F-12. Execution Hedge](../../features/F-12-execution-hedge/)

## Primary Actor

System (Matching Backend timer; triggered every batchIntervalMs after batch clearing cycle of F-04).

## Supporting Actors

- Execution Planning (routing)
- Risk Manager (pre-hedge check)
- Venue Execution Adapter
- External Venues Connector
- CEX / DEX / AMM (external venue)
- Settlement Ledger (HedgePnL)
- ClickHouse (audit history)

## Preconditions

- Закрыт цикл batch clearing (F-04) и опубликован BatchResult.
- Computed `netQty[symbol]` для каждого провайдера.
- `|netQty| >= hedgeTriggerThreshold[symbol]` или `triggerNotional >= thresholdNotional` (см. `HedgeTriggerPolicy`).
- Активные venues публикуют `venue.liquidity.fob` (F-11) и `venue.health=CONNECTED`.
- `solver_config` / `hedge_policy_config` (target — таблица в PostgreSQL) задаёт `hedgeUrgencyPolicy`, `maxSlippage[urgency]`, `maxNotionalPerHedge`, `hedgeExposureLimit[symbol]`, `hedgeTimeoutMs`, `overfillThreshold`, `reconciliationGapThreshold`.

## Trigger

Matching Backend после успешного batch clearing вызывает `HedgeExecutionIntentsPublisher.publish_intents_for_batch()`, который:
1. Получает `PositionSnapshot` через `PositionSnapshotCalculator`.
2. Применяет `HedgeTriggerPolicy.Evaluate(snapshots)`.
3. Для каждого `triggered=true` snapshot формирует `ExecutionIntent` через `ExecutionIntentBuilder` (см. cpp/matching/src/app/).

## Main Flow

1. Matching Backend публикует `ExecutionIntent` в Kafka `execution.intents` (partitionKey = `hedge_flow_id`).
2. Execution Planning читает `ExecutionIntent`.
3. Execution Planning загружает текущие `venue.liquidity.fob` curves для `instrument.symbol` и `venue.health` для allowed_venues.
4. Execution Planning формирует routing plan: `qty[v] = L(v) / Sum L(v') * targetQty` (формула из IN-005 §6).
5. Execution Planning вызывает `RiskService.PreHedgeCheck` с тремя проверками (`targetNotional <= maxNotionalPerHedge`, `currentHedgeExposure + targetQty <= hedgeExposureLimit`, `expectedSlippage <= maxSlippage[urgency]`).
6. Risk Manager возвращает OK.
7. Execution Planning передаёт `ExecutionIntent + RoutingPlan` в Venue Execution Adapter.
8. Venue Execution Adapter создаёт запись в PostgreSQL `hedgeflows` (status=OPEN).
9. Для каждого venue в routing plan Adapter:
   1. Создаёт `child_orders` запись (status=PENDING).
   2. Вызывает External Venues Connector `PlaceChildOrder` (REST / WS / on-chain tx).
   3. Получает Execution Event от venue.
   4. Нормализует в `ExecutionReport` (status=FILLED/PARTIALLY_FILLED).
   5. Обновляет `child_orders` (filled_qty, avg_price, fee).
   6. Публикует `ExecutionReport` в Kafka `execution.venue` (partitionKey = `hedge_flow_id`).
10. Adapter запускает Reconciliation: если `remainingQty <= reconciliationGapThreshold` → status=COMPLETED.
11. Adapter обновляет `hedgeflows` (filled_qty, avg_fill_price, hedge_pnl, completed_at, status=COMPLETED).
12. Settlement Ledger потребляет `execution.venue`, обновляет позиции и считает HedgePnL по формуле.
13. ClickHouse потребляет `execution.venue` и сохраняет в `execution_reports`.
14. Observability фиксирует latency, slippage, fill rate.

## Alternative Flows

См. отдельные use cases:

- A1. Partial fill + retry — [UC-F12-03](../UC-F12-03-partial-fill-retry/).
- A2. Rejection + fallback — [UC-F12-04](../UC-F12-04-rejection-fallback/).
- A3. Timeout / underfilled — [UC-F12-05](../UC-F12-05-timeout-underfilled-reconciliation/).
- A4. Manual operator override — [UC-F12-02](../UC-F12-02-manual-operator-hedge/).

## Postconditions

- `hedgeflows.status = COMPLETED`, `filled_qty = target_qty`, `avg_fill_price` и `hedge_pnl` рассчитаны.
- Все child_orders в финальном статусе (FILLED).
- Все ExecutionReport опубликованы в `execution.venue` и сохранены в ClickHouse.
- Settlement Ledger обновил positions provider'а.
- Observability метрики: `hedge_latency_ms`, `hedge_fill_rate`, `hedge_slippage_bps` обновлены.

## Acceptance Criteria

Покрывает F12-1, F12-2, F12-3, F12-6, F12-7, F12-8, F12-10, F12-11 из [feature acceptance-criteria](../../features/F-12-execution-hedge/acceptance-criteria.md).

## Related Sequence Diagrams

- [System sequence: SEQ-UC-F12-01-system](sequences/SEQ-UC-F12-01-system.md)
- [Service sequence: SEQ-F12-01-auto-hedge-services](../../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md)

## Related Contracts

- Kafka: [execution.intents](../../../06-api/messaging/execution-intents.md), [execution.venue](../../../06-api/messaging/execution-venue.md)
- gRPC: [risk-pre-hedge-check](../../../06-api/grpc/risk-pre-hedge-check.md)
- Proto: `fob.execution.v1.ExecutionIntent`, `fob.execution.v1.ExecutionReport`, `fob.hedge.v1.HedgeFlow`, `fob.hedge.v1.ChildOrder`

## Related Components

- [matching-fob-core](../../../05-components/matching-fob-core/overview.md) — emits ExecutionIntent
- [execution-planning](../../../05-components/execution-planning/overview.md) — routing plan
- [risk-manager](../../../05-components/risk-manager/overview.md) — PreHedgeCheck
- [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md) — HedgeFlow state machine
- [external-venues](../../../05-components/external-venues/overview.md) — EVC (REST/WS/RPC)
- [ledger](../../../05-components/ledger/overview.md) — HedgePnL
- [observability-reporting](../../../05-components/observability-reporting/overview.md)

## Related Data

- PostgreSQL: [hedgeflows](../../../07-data/hedgeflows.md), [child_orders](../../../07-data/child-orders.md)
- ClickHouse: [execution_reports](../../../07-data/execution-reports.md)

## Source Fragments

- IN-005 §2 «Sequence diagram — основной happy path»
- IN-005 §6 «Формулы расчётов» — Hedge Trigger, targetNotional, Routing Plan, Pre-hedge Risk Check
- IN-005 §7 «AC F12-1..F12-12»
