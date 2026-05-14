# UC-F08-01. Принудительная ликвидация позиции

## Feature

- [F-08. Post-trade Risk & Liquidations](../../features/F-08-posttrade-risk-and-liquidations/)

## Primary Actor

System (Risk Manager)

## Supporting Actors

- Trader (получатель уведомления)
- Operator (manual override)

## Preconditions

- Маржинальное покрытие пользователя ниже maintenance margin.

## Trigger

Post-trade risk check после `batch.outputs` обнаруживает margin breach.

## Main Flow

1. Risk Manager регистрирует `RiskAlert(margin_call)`.
2. Risk создаёт ликвидационный ордер на закрытие позиции.
3. Order Flow обрабатывает как обычную команду CreateFlowOrder.
4. Matching включает в следующий batch.
5. После исполнения Ledger обновляет позицию.
6. Risk публикует `RiskAlert(liquidation_complete)`.

## Alternative Flows

### A1. Operator override

1. Operator вручную отменяет ликвидацию через console.

## Postconditions

- Позиция закрыта.
- Audit trail.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F08-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F08-UC-F08-01-services.md)

## Related Contracts

- [risk.alerts](../../../06-api/messaging/risk-alerts.md)
- `fob.orders.v1.OrderFlowService.CreateFlowOrder` (системный)

## Related Components

- [risk-manager](../../../05-components/risk-manager/overview.md)
- [ledger](../../../05-components/ledger/overview.md)
- [order-flow](../../../05-components/order-flow/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)

## Related Data

- [risk_snapshots](../../../07-data/data-overview.md)
- [positions](../../../07-data/data-overview.md)
