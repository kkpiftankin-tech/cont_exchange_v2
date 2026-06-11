<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F12-05. Timeout / Underfilled / Reconciliation

## Feature

- [F-12. Execution Hedge](../../features/F-12-execution-hedge/)

## Primary Actor

System (Venue Execution Adapter timeout watcher; Reconciliation loop).

## Supporting Actors

- External Venues Connector (CancelOrder API)
- Risk Manager (publish alert)
- Observability

## Preconditions

- Активный HedgeFlow с placed child_orders.
- Истёк `hedge_flow.timeout_ms` (например, 5000 ms для urgency=HIGH; 60_000 ms для urgency=LOW).
- Часть child orders ещё в статусе NEW / PENDING / PARTIALLY_FILLED.

## Trigger

Внутренний watchdog в Venue Execution Adapter detect-ит `now - hedge_flow.created_at > timeout_ms`.

## Main Flow

1. Watchdog обнаруживает истечение timeoutMs.
2. Adapter формирует список open child orders (`status IN (PENDING, PARTIALLY_FILLED, NEW)`).
3. Для каждого open child order Adapter вызывает EVC `CancelOrder(client_order_id)`.
4. EVC отправляет CancelOrder на venue (REST/WS).
5. Venue подтверждает cancel; EVC возвращает результат.
6. Adapter обновляет `child_orders.status = CANCELLED`.
7. Adapter публикует ExecutionReport(status=CANCELLED, reason=`timeout`) в Kafka `execution.venue`.
8. Adapter вычисляет финальный `filledQty` (агрегат всех FILLED + PARTIALLY_FILLED child orders) и `gap = targetQty - filledQty`.
9. Если `gap <= reconciliationGapThreshold` → status=COMPLETED.
10. Если `gap > reconciliationGapThreshold`:
    1. Adapter обновляет `hedgeflows.status = UNDERFILLED`, `hedgeflows.completed_at = now()`.
    2. Adapter публикует `risk.alerts(type=HEDGE_UNDERFILL, hedge_flow_id, gap, fill_ratio)`.
    3. Observability логирует UNDERFILLED HedgeFlow в Reconciliation Alerts dashboard.
    4. Опционально: если включена политика `auto_retry_on_underfill=true` и количество предыдущих retry < `max_retry`, создаётся новый ExecutionIntent с `urgency=HIGH` для остатка (см. также [UC-F12-03](../UC-F12-03-partial-fill-retry/)).

## Alternative Flows

### A1. Overfill detected во время timeout

В race condition между timeout и late fill, аккумулированный filledQty может превысить targetQty.

- Adapter detect: `accumulatedFilledQty - targetQty > overfillThreshold`.
- Cancel all открытых child orders.
- Publish ExecutionReport(status=OVERFILL_GUARD, reason=`overfill_race_condition`).
- Status=COMPLETED или REJECTED (зависит от политики).

### A2. CancelOrder fails

Venue не отвечает на CancelOrder.

- Adapter ставит retry-cancel с exponential backoff.
- После N попыток — `risk.alerts(type=HEDGE_STUCK, hedge_flow_id)` для ручного разбора.

### A3. Risk Manager rejection во время авто-retry

После UNDERFILLED Adapter создаёт retry intent → PreHedgeCheck возвращает REJECT.

- Дальнейшие retry прерываются.
- `hedgeflows.status = UNDERFILLED` остаётся финальным.

## Postconditions

- `hedgeflows.status = COMPLETED` (если gap в допустимом intervale) или `UNDERFILLED`.
- Все child orders в финальных статусах (FILLED / PARTIALLY_FILLED / CANCELLED / REJECTED).
- При UNDERFILLED — risk alert опубликован, операторский dashboard обновлён.

## Acceptance Criteria

Покрывает F12-5 (reconciliation), F12-4 (overfill guard в race condition), F12-N3 (reconciliation gap NFR).

## Related Sequence Diagrams

- [System sequence: SEQ-UC-F12-05-system](sequences/SEQ-UC-F12-05-system.md)
- Service-level: [SEQ-F12-03-error-scenarios-services](../../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md)

## Related Contracts

- Kafka: [execution.venue](../../../06-api/messaging/execution-venue.md), [risk.alerts](../../../06-api/messaging/risk-alerts.md)
- EVC: CancelOrder API (внутренний)

## Related Components

- [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md)
- [external-venues](../../../05-components/external-venues/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [observability-reporting](../../../05-components/observability-reporting/overview.md)

## Related Data

- PostgreSQL: [hedgeflows](../../../07-data/hedgeflows.md), [child_orders](../../../07-data/child-orders.md)
- ClickHouse: [execution_reports](../../../07-data/execution-reports.md)

## Source Fragments

- IN-005 §4 «Sequence diagram — error scenarios» (Timeout, Overfill guard, Risk rejection)
- IN-005 §5 «Reconciliation алгоритм»
- IN-005 §6 «Reconciliation Gap», «Overfill Guard»
- IN-005 §10.1 «U3: Overfill guard», «U5: Timeout → UNDERFILLED», «U7: Reconciliation»
