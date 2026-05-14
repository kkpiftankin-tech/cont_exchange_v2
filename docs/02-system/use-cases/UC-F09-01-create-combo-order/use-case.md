# UC-F09-01. Создать combo / portfolio order

## Feature

- [F-09. Combo & Portfolio Orders](../../features/F-09-batch-combo-orders/)

## Primary Actor

Trader / Market Maker

## Preconditions

- Пользователь аутентифицирован.
- Все legs combo-заявки имеют торгуемые инструменты.

## Trigger

Trader посылает `CreateComboOrder` с несколькими legs.

## Main Flow

1. Trader задаёт legs (symbol, side, qty, price range).
2. Order Flow валидирует консистентность combo.
3. Risk проверяет совокупный риск.
4. Ledger резервирует средства по net-position.
5. Order Flow публикует `orders.normalized` с тегом combo.
6. Matching обрабатывает с учётом ограничения «все или ничего».

## Alternative Flows

### A1. Один leg отвергнут — combo отвергается полностью.

## Postconditions

- Записи в `flow_orders` с одинаковым `combo_id`.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F09-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F09-UC-F09-01-services.md)

## Related Contracts

- (планируется) `ComboOrder` в `fob.orders.v1`
- [orders.normalized](../../../06-api/messaging/orders-normalized.md)

## Related Components

- [order-flow](../../../05-components/order-flow/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [ledger](../../../05-components/ledger/overview.md)

## Related Data

- [flow_orders](../../../07-data/data-overview.md)
