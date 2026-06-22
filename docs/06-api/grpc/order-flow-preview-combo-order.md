# gRPC Method: OrderFlowService/PreviewComboOrder

## Status

TODO / planned — описан в [order-flow-create-combo-order.md](order-flow-create-combo-order.md).
Отдельный файл как точка трассировки.

## Purpose

Grouped preview для ComboOrder без создания заявки. Возвращает ожидаемый execution
scale, leg fills, combined VWAP, IS, ratio deviation, binding legs/constraints,
expected margin load, external execution risk.

Preview считает grouped prognosis; **не** независимые прогнозы по ногам.

## Transport

gRPC (unary; в будущем — server-streaming для live preview при изменении цен).

## Service

`fob.orders.v1.OrderFlowService`

## Method (planned)

```proto
rpc PreviewComboOrder(PreviewComboOrderRequest) returns (PreviewComboOrderResponse);
```

## Schema

Полная схема — в [order-flow-create-combo-order.md](order-flow-create-combo-order.md#previewcomboorder).

Ключевые поля ответа: `expected_execution_scale`, `expected_leg_fills`,
`expected_combined_vwap`, `expected_is`, `expected_ratio_deviation_bps`,
`binding_leg_ids`, `binding_constraint_ids`, `has_external_execution_risk`.

## Idempotency

Read-only; идемпотентен по определению.

## Used In Features

- [F-09](../../02-system/features/F-09-batch-combo-orders/)

## Used In Use Cases

- [UC-F09-01](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md)

## Source Fragments

- IN-011 §11.2 (preview semantics)
```

---
