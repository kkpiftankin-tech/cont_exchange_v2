# gRPC Method: ObservabilityService/GetPostTradeReport

## Status

TODO / planned

## Purpose

Сгенерировать post-trade отчёт для пользователя за период: список fills, VWAP, Implementation Shortfall, спред, fees, agreggated по символам и стратегиям. Источник данных — ClickHouse (`fills`, `batch_results`, `execution_reports`).

## Transport

gRPC

## Service

`fob.observability.v1.ObservabilityService` (planned package; компонент `observability-reporting` существует, но gRPC API не зафиксирован)

## Method (planned)

```proto
rpc GetPostTradeReport(GetPostTradeReportRequest) returns (GetPostTradeReportResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/reports/post-trade` → gRPC)

## Callee

- [observability-reporting](../../05-components/observability-reporting/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message PostTradeReportRow {
  string symbol = 1;
  fob.common.v1.Decimal traded_qty = 2;
  fob.common.v1.Decimal vwap = 3;
  fob.common.v1.Decimal implementation_shortfall = 4;
  fob.common.v1.Decimal fees_paid = 5;
  int64 fills_count = 6;
}

message GetPostTradeReportRequest {
  fob.common.v1.EventMeta meta = 1;
  string user_id = 2;
  google.protobuf.Timestamp from_ts = 3;
  google.protobuf.Timestamp to_ts = 4;
  repeated string symbols = 5;           // optional filter
}

message GetPostTradeReportResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated PostTradeReportRow rows = 2;
  fob.common.v1.Error error = 3;
}
```

## Used In Features

- [F-13. Post-Trade Report](../../02-system/features/F-13-posttrade-report/)

## Used In Use Cases

- [UC-F13-01. Generate Post-Trade Report](../../02-system/use-cases/UC-F13-01-generate-posttrade-report/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F13-UC-F13-01-services](../../05-components/sequences/SEQ-F13-UC-F13-01-services.md)

## Related Components

- [observability-reporting](../../05-components/observability-reporting/overview.md)
- [gateway](../../05-components/gateway/overview.md)

## Related Data Objects

- ClickHouse [`fills`](../../07-data/olap-schema.md#таблица-fills)
- ClickHouse [`batch_results`](../../07-data/olap-schema.md#таблица-batch_results)
- ClickHouse `execution_reports`

## Source Fragments

- IN-001-FR-016 (FR-RPT-001 post-trade, derived)
- IN-001-FR-022
