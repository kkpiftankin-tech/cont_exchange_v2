<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F12-03. Partial Fill + Retry

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-12-execution-hedge](../../features/F-12-execution-hedge/) |
| ☁️ L0 system sequence | [SEQ-UC-F12-03-system](sequences/SEQ-UC-F12-03-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F12-03-error-scenarios-services](../../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-12. Execution Hedge](../../features/F-12-execution-hedge/)

## Primary Actor

System (Venue Execution Adapter; Reconciliation loop).

## Supporting Actors

- External Venues Connector
- External Venue
- Execution Planning (retry)
- Risk Manager (PreHedgeCheck для retry)

## Preconditions

- Существует активный HedgeFlow (status=OPEN) с placed child_orders.
- Часть child orders вернула `PARTIALLY_FILLED` со временем (например, LIMIT order на CEX исполнился частично).
- `filledQty < targetQty` и `(targetQty - filledQty) > reconciliationGapThreshold`.

## Trigger

- ExecutionReport(status=PARTIALLY_FILLED) от venue с `filled_qty < remaining_qty`.
- Либо ExecutionReport(status=FILLED) на одном из child orders, после которого аккумулированный fill всё ещё ниже target.

## Main Flow

1. Venue Execution Adapter получает ExecutionReport(PARTIALLY_FILLED) от EVC.
2. Adapter обновляет `child_orders.filled_qty`, `child_orders.avg_price`, `child_orders.fee`, status=PARTIALLY_FILLED.
3. Adapter обновляет агрегаты в `hedgeflows.filled_qty`, `hedgeflows.avg_fill_price` (VWAP по формуле из IN-005 §6).
4. Adapter публикует ExecutionReport в `execution.venue`.
5. Adapter вычисляет `remainingQty = targetQty - filledQty`.
6. Если `remainingQty > reconciliationGapThreshold`:
   1. Adapter запрашивает retry routing у Execution Planning.
   2. Execution Planning исключает venues, у которых `filled_qty < requested_qty * 0.5` (слабая ликвидность), либо повышает urgency на step (LOW→MEDIUM, MEDIUM→HIGH).
   3. Risk Manager `PreHedgeCheck` для retry intent.
   4. Adapter создаёт новые child_orders на новые venues.
   5. EVC исполняет.
7. Цикл повторяется до:
   - `remainingQty <= reconciliationGapThreshold` → status=COMPLETED.
   - `hedgeTimeoutMs` истёк → см. [UC-F12-05](../UC-F12-05-timeout-underfilled-reconciliation/).
   - Все venues исключены / risk rejection → status=UNDERFILLED + risk alert.

## Alternative Flows

### A1. Timeout до полного fill

См. [UC-F12-05](../UC-F12-05-timeout-underfilled-reconciliation/).

### A2. Overfill guard срабатывает на retry

После retry аккумулированный fill превышает targetQty (например, retry race condition).

- См. F12-4: Adapter обнаруживает `accumulatedFilledQty > targetQty + overfillThreshold` → cancel all open child orders + ExecutionReport(OVERFILL_GUARD).

## Postconditions

- `hedgeflows.status = COMPLETED` (при успешном retry).
- Все retry-child_orders сохранены с reference на исходный `hedge_flow_id`.
- Audit trail: timeline ExecutionReport показывает retry attempts.

## Acceptance Criteria

Покрывает F12-5 (reconciliation), F12-4 (overfill guard на retry edge case), F12-12 (rejection fallback при reroute).

## Related Sequence Diagrams

- [System sequence: SEQ-UC-F12-03-system](sequences/SEQ-UC-F12-03-system.md)
- Service-level: частично пересекается с [SEQ-F12-01-auto-hedge-services](../../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md) (retry loop) и [SEQ-F12-03-error-scenarios-services](../../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md).

## Related Contracts

- Kafka: [execution.intents](../../../06-api/messaging/execution-intents.md) (retry intent), [execution.venue](../../../06-api/messaging/execution-venue.md)
- gRPC: [risk-pre-hedge-check](../../../06-api/grpc/risk-pre-hedge-check.md)
- REST: [POST /api/v1/hedge/flows/{id}/retry](../../../06-api/rest/hedgeflows.md#post-apiv1hedgeflowsidretry) — operator manual retry.

## Related Components

- [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md)
- [execution-planning](../../../05-components/execution-planning/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [external-venues](../../../05-components/external-venues/overview.md)

## Related Data

- PostgreSQL: [hedgeflows](../../../07-data/hedgeflows.md), [child_orders](../../../07-data/child-orders.md) (multiple per hedge_flow_id)
- ClickHouse: [execution_reports](../../../07-data/execution-reports.md)

## Source Fragments

- IN-005 §5 «Reconciliation алгоритм»
- IN-005 §6 «remainingQty» и «avgFillPrice (VWAP fills)»
- IN-005 §10.1 «U2: Partial fill + retry», «U7: Reconciliation»
