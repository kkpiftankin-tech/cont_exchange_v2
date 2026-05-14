# gRPC Method: LedgerService/ApplyExecutionReport

## Status

draft (proto-defined)

## Purpose

Применить отчёт об исполнении на внешнем venue к hedge-балансу биржи. Используется как синхронная альтернатива потреблению Kafka `execution.reports`. Обновляет позицию биржи на CEX/DEX/AMM, не балансы пользователей напрямую.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc ApplyExecutionReport(ApplyExecutionReportRequest) returns (google.protobuf.Empty);
```

## Caller

- [external-venues](../../05-components/external-venues/overview.md) (опционально, sync-режим)
- replay / тесты

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

См. [contracts/proto/fob/ledger/v1/ledger.proto](../../../contracts/proto/fob/ledger/v1/ledger.proto).

```proto
message ApplyExecutionReportRequest {
  fob.common.v1.EventMeta meta = 1;
  fob.execution.v1.ExecutionReport report = 2;
}
```

## Idempotency

По ключу `(intent_id, external_order_id, execution_id)`. Дубликат отчёта не должен повторно сдвигать hedge-баланс.

## Used In Features

- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/)

## Used In Use Cases

- [UC-F12-01. Execute Hedge](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F12-UC-F12-01-services](../../05-components/sequences/SEQ-F12-UC-F12-01-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md)
- [external-venues](../../05-components/external-venues/overview.md)

## Related Data Objects

- `accounts` (биржевой hedge-аккаунт)
- ClickHouse `execution_reports`
- Kafka `execution.reports`

## Source Fragments

- IN-001-FR-022
