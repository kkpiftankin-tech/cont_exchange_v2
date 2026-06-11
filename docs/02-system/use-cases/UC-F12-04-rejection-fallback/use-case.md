<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F12-04. Rejection + Fallback

## Feature

- [F-12. Execution Hedge](../../features/F-12-execution-hedge/)

## Primary Actor

System (Venue Execution Adapter; Execution Planning fallback).

## Supporting Actors

- External Venues Connector (Primary venue A, Alternative venue B)
- Execution Planning

## Preconditions

- Активный HedgeFlow с child_orders на venue A.
- Альтернативные venues (B, C, ...) доступны и `venue.health=CONNECTED`.

## Trigger

External Venues Connector получает REJECTED от venue A. Типичные причины: `insufficient_liquidity`, `price_too_aggressive`, `rate_limit_exceeded`, `account_disabled`, `circuit_breaker_open`.

## Main Flow

1. EVC отправляет NewOrder на venue A.
2. Venue A отвечает REJECTED (reason: `insufficient_liquidity`).
3. EVC возвращает REJECTED в Venue Execution Adapter.
4. Adapter обновляет `child_orders.status = REJECTED`, `child_orders.error.message`.
5. Adapter публикует ExecutionReport(status=REJECTED, reason) в Kafka `execution.venue`.
6. Adapter запрашивает fallback routing у Execution Planning, передавая `excluded_venues = [A]`.
7. Execution Planning формирует новый routing plan на venues B, C (исключая A); если urgency был LOW, опционально повышает до MEDIUM.
8. Risk Manager `PreHedgeCheck` для нового routing.
9. Adapter создаёт новые `child_orders` на venue B (status=PENDING).
10. EVC исполняет на venue B.
11. Venue B возвращает FILLED.
12. Adapter обновляет `hedgeflows`, публикует ExecutionReport(FILLED).
13. Reconciliation → COMPLETED.

## Alternative Flows

### A1. Все альтернативные venues тоже rejected или unavailable

Если после исключения venue A нет здоровых venues:
- Execution Planning возвращает empty routing plan.
- Adapter обновляет `hedgeflows.status = REJECTED`.
- Adapter публикует `risk.alerts(type=HEDGE_REJECTED, hedge_flow_id, reason)`.
- Observability фиксирует alert; оператор получает уведомление.

### A2. Risk Manager rejects retry (например, `currentHedgeExposure + targetQty > hedgeExposureLimit` после предыдущих attempts)

- Adapter обновляет `hedgeflows.status = RISK_REJECTED`.
- Publish `risk.alerts(type=HEDGE_REJECTED, reason=EXPOSURE_LIMIT)`.

### A3. Rejection из-за временного circuit breaker

Если venue A reported `circuit_breaker_open` и health endpoint показывает временную недоступность:
- Adapter может marked venue A как `excluded_until=now+backoff_ms`.
- Сразу пробовать venue B без cooldown.

## Postconditions

- При успешном fallback: HedgeFlow COMPLETED с записями child_orders на A (REJECTED) и B (FILLED).
- При полном fail: HedgeFlow=REJECTED + risk.alert.

## Acceptance Criteria

Покрывает F12-12 (rejection fallback), частично F12-3 (multi-venue routing).

## Related Sequence Diagrams

- [System sequence: SEQ-UC-F12-04-system](sequences/SEQ-UC-F12-04-system.md)
- Service-level: [SEQ-F12-02-rejection-fallback-services](../../../05-components/sequences/SEQ-F12-02-rejection-fallback-services.md)

## Related Contracts

- Kafka: [execution.intents](../../../06-api/messaging/execution-intents.md), [execution.venue](../../../06-api/messaging/execution-venue.md), [risk.alerts](../../../06-api/messaging/risk-alerts.md)
- gRPC: [risk-pre-hedge-check](../../../06-api/grpc/risk-pre-hedge-check.md)

## Related Components

- [venue-execution-adapter](../../../05-components/venue-execution-adapter/overview.md)
- [execution-planning](../../../05-components/execution-planning/overview.md)
- [external-venues](../../../05-components/external-venues/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [observability-reporting](../../../05-components/observability-reporting/overview.md)

## Related Data

- PostgreSQL: [hedgeflows](../../../07-data/hedgeflows.md), [child_orders](../../../07-data/child-orders.md)
- ClickHouse: [execution_reports](../../../07-data/execution-reports.md)

## Source Fragments

- IN-005 §3 «Sequence diagram — rejection + fallback»
- IN-005 §10.1 «U4: Rejection + fallback»
