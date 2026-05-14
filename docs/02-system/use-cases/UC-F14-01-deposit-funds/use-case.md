# UC-F14-01. Депозит средств

## Feature

- [F-14. Deposit / Withdraw](../../features/F-14-deposit-withdraw/)

## Primary Actor

Trader

## Supporting Actors

- Custody / Payment Provider

## Preconditions

- Пользователь аутентифицирован.
- Custody adapter сконфигурирован (планируется).

## Trigger

Trader инициирует депозит.

## Main Flow

1. Trader инициирует депозит через UI.
2. Gateway отправляет команду в Ledger.
3. Custody adapter получает on-chain / банковское подтверждение.
4. Ledger пополняет `accounts`.
5. Уведомление пользователю.

## Alternative Flows

### A1. Депозит не подтверждён — статус pending, alert после таймаута.

## Postconditions

- Баланс увеличен.
- Запись в `collateral_transfers`.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F14-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F14-UC-F14-01-services.md)

## Related Contracts

- (планируется) `POST /v1/deposits`
- `fob.ledger.v1.LedgerService.ApplyDeposit`

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [ledger](../../../05-components/ledger/overview.md)
- (планируется) custody-adapter

## Related Data

- (планируется) `collateral_transfers`, `accounts` в PostgreSQL
