# gRPC Method: RiskService/CheckNewOrder

## Status

draft (proto-defined)

## Purpose

Pre-trade risk-проверка для новой или изменённой FlowOrder. Возвращает `RiskDecision`: `ACCEPT`, `REJECT`, `RESIZE` (с рекомендованными параметрами), либо `HALT` (если действует kill-switch).

В сервисных диаграммах часто упоминается как `PreTradeCheck` — это синоним. Канонический proto-метод: `CheckNewOrder`.

## Transport

gRPC

## Service

`fob.risk.v1.RiskService`

## Method

```proto
rpc CheckNewOrder(PreTradeCheckRequest) returns (PreTradeCheckResponse);
```

## Caller

- [order-flow](../../05-components/order-flow/overview.md) — перед резервированием средств и публикацией в Kafka

## Callee

- [risk-manager](../../05-components/risk-manager/overview.md)

## Schema

См. [contracts/proto/fob/risk/v1/risk.proto](../../../contracts/proto/fob/risk/v1/risk.proto).

```proto
message PreTradeCheckRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  fob.orders.v1.FlowOrder order = 3;
  repeated BalanceSnapshot balances = 10;
  repeated PositionSnapshot positions = 11;
  fob.common.v1.Decimal reference_price = 20;
}

message PreTradeCheckResponse {
  fob.common.v1.EventMeta meta = 1;
  RiskDecision decision = 2;          // ACCEPT / REJECT / RESIZE / HALT
  fob.common.v1.Error error = 3;
  fob.orders.v1.FlowOrder resized_order = 10;
  fob.common.v1.Decimal required_initial_margin = 20;
  fob.common.v1.Decimal max_loss_estimate = 21;
}
```

## Used In Features

- [F-02. Create FlowOrder](../../02-system/features/F-02-create-floworder/)
- [F-03. Order Lifecycle](../../02-system/features/F-03-order-lifecycle/) (re-check on amend)
- [F-07. Pre-Trade Risk](../../02-system/features/F-07-pretrade-risk/)

## Used In Use Cases

- [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md)
- [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md)
- [UC-F07-01](../../02-system/use-cases/UC-F07-01-pretrade-risk-check/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F02-UC-F02-01-services](../../05-components/sequences/SEQ-F02-UC-F02-01-services.md)
- [SEQ-F03-UC-F03-01-services](../../05-components/sequences/SEQ-F03-UC-F03-01-services.md)
- [SEQ-F07-UC-F07-01-services](../../05-components/sequences/SEQ-F07-UC-F07-01-services.md)

## Related Components

- [order-flow](../../05-components/order-flow/overview.md)
- [risk-manager](../../05-components/risk-manager/overview.md)
- [ledger](../../05-components/ledger/overview.md) (опционально — для snapshot)

## Related Data Objects

- [`risk_limits`](../../07-data/oltp-schema.md#таблица-risk_limits)
- [`risk_snapshots`](../../07-data/oltp-schema.md#таблица-risk_snapshots)
- Kafka `risk.alerts` (REJECT/HALT → побочный alert)

## Source Fragments

- IN-001-FR-016 (FR-RISK-001 pre-trade)
- IN-001-FR-022, IN-001-FR-023
