# gRPC Method: RiskService/GetRiskSnapshot

## Status

Implemented (контракт добавлен в `contracts/proto/fob/risk/v1/risk.proto`, F-06)

## Purpose

Получить агрегированный snapshot риска пользователя: текущие лимиты, использование лимитов, маржинальный коэффициент, near-liquidation flag. Используется UI/Gateway для отображения panel «риск/маржа» и operator-консолью.

## Transport

gRPC

## Service

`fob.risk.v1.RiskService`

## Method

```proto
rpc GetRiskSnapshot(GetRiskSnapshotRequest) returns (GetRiskSnapshotResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/risk/snapshot` → gRPC)

## Callee

- [risk-manager](../../05-components/risk-manager/overview.md)

## Schema

Финальный контракт (см. `contracts/proto/fob/risk/v1/risk.proto`). Monetary
values используют `fob.common.v1.Decimal` (стиль `BalanceSnapshot`). Snapshot
переиспользует уже существующие `BalanceSnapshot` / `PositionSnapshot`, чтобы
показать collateral и позиции, из которых выводятся margin-числа.

```proto
message RiskSnapshot {
  string entity_id = 1;                              // user_id or account_id
  fob.common.v1.Decimal free_collateral = 2;         // collateral available for new risk
  fob.common.v1.Decimal reserved_collateral = 3;     // collateral locked by open exposure
  fob.common.v1.Decimal initial_margin = 4;          // required initial margin
  fob.common.v1.Decimal maintenance_margin = 5;      // required maintenance margin
  bool margin_call = 6;                              // equity below maintenance margin
  bool liquidation = 7;                              // liquidation triggered
  google.protobuf.Timestamp timestamp = 8;          // snapshot time
  repeated BalanceSnapshot balances = 10;            // underlying collateral balances
  repeated PositionSnapshot positions = 11;          // underlying open positions
}

message GetRiskSnapshotRequest {
  string entity_id = 1;
}

message GetRiskSnapshotResponse {
  RiskSnapshot snapshot = 1;
}
```

## Used In Features

- [F-06. Positions / PnL / Margin](../../02-system/features/F-06-positions-pnl-margin/)
- [F-08. Post-Trade Risk and Liquidations](../../02-system/features/F-08-posttrade-risk-and-liquidations/)

## Used In Use Cases

- [UC-F06-01](../../02-system/use-cases/UC-F06-01-show-positions/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F06-UC-F06-01-services](../../05-components/sequences/SEQ-F06-UC-F06-01-services.md)

## Related Components

- [risk-manager](../../05-components/risk-manager/overview.md)
- [gateway](../../05-components/gateway/overview.md)

## Related Data Objects

- [`risk_limits`](../../07-data/oltp-schema.md#таблица-risk_limits)
- [`risk_snapshots`](../../07-data/oltp-schema.md#таблица-risk_snapshots)

## Source Fragments

- IN-001-FR-016 (FR-RISK-004 snapshot, derived)
- IN-001-FR-022
