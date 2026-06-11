<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F09-01. Создать combo / multi-leg order

## Feature

- [F-09. Batch, Combo and Multi-leg Orders](../../features/F-09-batch-combo-orders/)

## Primary Actor

Trader / Market Maker / API Client

## Supporting Actors

- order_flow (Provider-Client Backend)
- Risk Manager
- PostgreSQL

## Preconditions

- Пользователь аутентифицирован.
- Все ноги имеют торгуемые инструменты.
- Выбран `executionMode` (`orchestration_only` или `multileg_vector_solver`) и
  `atomicityPolicy`; для жёстких weights/ratio/spread/factor/budget/leverage/
  margin/risk режим обязан быть `multileg_vector_solver` ([ADR-031](../../../03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md)).

## Trigger

Trader отправляет `CreateComboOrder` (или `CreateBatchOrder`) с несколькими
legs, constraints и graph links.

## Main Flow

1. Пользователь задаёт legs (instrument, side, ratio/weight, price band, qMax,
   qRate, venuePreferences) и constraints (ratio/spread/factor/budget/risk).
2. API Gateway маршрутизирует команду в `order_flow`.
3. `order_flow` нормализует parent order, legs, constraints и graph links
   (строит child graph для OCO/bracket/conditional).
4. `order_flow` вызывает `RiskService/PreTradeCheckGroup`: по каждой ноге, по
   группе, margin impact, total notional, max legs, max external legs,
   допустимость atomicity scope.
5. При approve `order_flow` сохраняет parent/legs/constraints/graph в PostgreSQL
   и публикует grouped `orders.normalized`.
6. Parent order получает статус `active` или `waiting_for_trigger`.
7. (Опционально) `PreviewComboOrder` возвращает **grouped** прогноз: expected
   execution scale, expected leg fills, combined VWAP/IS, ratio deviation,
   binding leg/constraints, external execution risk (IN-011 §11.2).

## Alternative Flows

- **A1.** Risk reject — parent order `rejected`, ни одна нога не активируется.
- **A2.** Risk resize/throttle — parent `throttled`, параметры ограничены.
- **A3.** `orchestration_only` — система показывает предупреждение, что веса/
  ratio/spread могут отклониться; ноги живут как независимые FlowOrder.

## Postconditions

- Записи в `batch_orders`/`combo_orders`/`combo_order_legs`/`combo_constraints`/
  `conditional_links` с единым `parentOrderId`.
- Событие в `orders.normalized` с parent+legs+constraints+graph+mode+policy.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F09-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F09-UC-F09-01-services.md)

## Related Contracts

- [OrderFlowService/CreateComboOrder|CreateBatchOrder|PreviewComboOrder](../../../06-api/grpc/order-flow-create-combo-order.md)
- [orders.normalized](../../../06-api/messaging/orders-normalized.md)

## Related Components

- [order-flow](../../../05-components/order-flow/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [ledger](../../../05-components/ledger/overview.md)

## Related Data

- [batch_orders, combo_orders, combo_order_legs, combo_constraints, conditional_links](../../../07-data/oltp-schema.md)

## Acceptance Criteria

- [AC-F09-001](../../features/F-09-batch-combo-orders/acceptance-criteria.md#ac-f09-001-настоящий-multi-leg-solver),
  [AC-F09-002](../../features/F-09-batch-combo-orders/acceptance-criteria.md#ac-f09-002-защита-от-перекоса-portfolio-skew)

## Source

- IN-011 §7 UC-F09-001, §11.1, §11.2
