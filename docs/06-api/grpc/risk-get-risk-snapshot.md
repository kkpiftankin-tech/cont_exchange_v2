# gRPC Method: RiskService/GetRiskSnapshot

## Status

TODO / planned (в текущем `risk.proto` отсутствует)

## Purpose

Получить агрегированный snapshot риска пользователя: текущие лимиты, использование лимитов, маржинальный коэффициент, near-liquidation flag. Используется UI/Gateway для отображения panel «риск/маржа» и operator-консолью.

## Transport

gRPC

## Service

`fob.risk.v1.RiskService` (planned extension)

## Method (planned)

```proto
rpc GetRiskSnapshot(GetRiskSnapshotRequest) returns (GetRiskSnapshotResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/risk/snapshot` → gRPC)

## Callee

- [risk-manager](../../05-components/risk-manager/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message LimitUsage {
  string limit_name = 1;                 // "max_notional", "max_position", ...
  fob.common.v1.Decimal current = 2;
  fob.common.v1.Decimal limit = 3;
  double utilization_pct = 4;
}

message RiskSnapshot {
  fob.common.v1.Decimal initial_margin = 1;
  fob.common.v1.Decimal maintenance_margin = 2;
  fob.common.v1.Decimal margin_ratio = 3;
  bool near_liquidation = 4;
  repeated LimitUsage limits = 10;
}

message GetRiskSnapshotRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
}

message GetRiskSnapshotResponse {
  fob.common.v1.EventMeta meta = 1;
  RiskSnapshot snapshot = 2;
  fob.common.v1.Error error = 3;
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
