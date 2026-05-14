# UC-F02-01. Создать одноактивную потоковую заявку

## Feature

- [F-02. Create FlowOrder](../../features/F-02-create-floworder/)

## Primary Actor

Trader

## Supporting Actors

- Operator
- Provider (external market data)

## Preconditions

- Пользователь аутентифицирован.
- Инструмент торгуется.
- Достаточный свободный баланс.
- Kill-switch по инструменту не активирован.

## Trigger

Trader отправляет команду создания FlowOrder через UI или API.

## Main Flow

1. Trader задаёт `symbol`, `side`, `p_low`, `p_high`, `q_rate`, `q_max`, `window`.
2. Система показывает preview VWAP / IS.
3. Trader подтверждает заявку.
4. Система выполняет pre-trade risk check.
5. Система резервирует средства в Ledger.
6. Система публикует событие `orders.normalized`.
7. Система возвращает `order_id` и статус `active`.

## Alternative Flows

### A1. Недостаточный баланс

1. Risk Manager возвращает `REJECT`.
2. UI показывает причину.

### A2. Превышен лимит скорости

1. Risk Manager возвращает `THROTTLE` со скорректированными параметрами.
2. UI предлагает принять скорректированные параметры.

## Postconditions

- Запись в `flow_orders`.
- Резерв в `ledger.reservations`.
- Событие `orders.normalized` опубликовано.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F02-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F02-UC-F02-01-services.md)

## Related Contracts

- [POST /v1/flow-orders](../../../06-api/rest/) (планируется detail)
- [CreateFlowOrderRequest](../../../06-api/grpc/) — `fob.orders.v1.OrderFlowService`
- [PreTradeCheckRequest](../../../06-api/grpc/) — `fob.risk.v1.RiskService`
- [orders.normalized](../../../06-api/messaging/orders-normalized.md)

## Related Components

- [gateway](../../../05-components/gateway/overview.md)
- [order-flow](../../../05-components/order-flow/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [ledger](../../../05-components/ledger/overview.md)

## Related Data

- [flow_orders](../../../07-data/data-overview.md)
- [accounts](../../../07-data/data-overview.md)
- [risk_limits](../../../07-data/data-overview.md)
