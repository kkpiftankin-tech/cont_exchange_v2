# UC-F07-01. Pre-trade risk check

## Feature

- [F-07. Pre-trade Risk Control](../../features/F-07-pretrade-risk/)

## Primary Actor

System (Order Flow service)

## Preconditions

- Принята входящая команда CreateFlowOrder.

## Trigger

Order Flow вызывает `PreTradeCheck`.

## Main Flow

1. Order Flow собирает контекст: user, instrument, side, notional, speed.
2. Risk читает позиции, лимиты, free balance.
3. Risk вычисляет RiskDecision.
4. Risk возвращает ACCEPT / REJECT / RESIZE / HALT.

## Alternative Flows

### A1. Kill-switch активен

1. Risk возвращает `HALT`.

### A2. Лимит превышен

1. Risk возвращает `REJECT` или `RESIZE` с новыми параметрами.

## Postconditions

- Решение зафиксировано в `risk.alerts` (для нестандартных кейсов).

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F07-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F07-UC-F07-01-services.md)

## Related Contracts

- `fob.risk.v1.RiskService.PreTradeCheck`
- [risk.alerts](../../../06-api/messaging/risk-alerts.md)

## Related Components

- [order-flow](../../../05-components/order-flow/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [ledger](../../../05-components/ledger/overview.md)

## Related Data

- [risk_limits, accounts, positions](../../../07-data/data-overview.md)
