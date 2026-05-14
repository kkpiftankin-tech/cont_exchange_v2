# UC-F16-01. Активировать kill-switch

## Feature

- [F-16. Operator Console & Kill-Switch](../../features/F-16-operator-console/)

## Primary Actor

Operator

## Preconditions

- Operator аутентифицирован и имеет permission.

## Trigger

Operator принимает решение остановить торговлю (по инструменту / глобально).

## Main Flow

1. Operator посылает `ActivateKillSwitch(scope)` через console.
2. Gateway проверяет permissions.
3. Risk Manager переводит флаг kill-switch в Redis / state.
4. Risk публикует `risk.alerts(KILL_SWITCH)`.
5. Order Flow, Matching и Venues подписаны и переходят в halt-mode.
6. Operator получает подтверждение.

## Alternative Flows

### A1. Deactivate kill-switch — обратный flow.

## Postconditions

- Новые ордера отвергаются.
- Активные ордера могут отменяться (по policy).
- Audit-event записан.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F16-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F16-UC-F16-01-services.md)

## Related Contracts

- (планируется) `POST /v1/admin/kill-switch`
- [risk.alerts](../../../06-api/messaging/risk-alerts.md)

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [order-flow](../../../05-components/order-flow/overview.md)
- [observability-reporting](../../../05-components/observability-reporting/overview.md)

## Related Data

- (планируется) state в Redis / `risk_snapshots` audit
