# Kafka Topic: execution.reports

## Purpose

Отчёты о результатах исполнения на внешних площадках (CEX/DEX): fill, partial fill, reject, timeout.

## Producer

- [external-venues](../../05-components/external-venues/overview.md)

## Consumers

- [ledger](../../05-components/ledger/overview.md) — apply hedge to internal accounting
- [observability-reporting](../../05-components/observability-reporting/overview.md) — execution lifecycle logging

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней (целевая 30 для audit) |
| Partition key | `intent_id` |
| Delivery | at-least-once, idempotent consumer |
| Schema | `fob.execution.v1.ExecutionReport` (Protobuf) |

## Message schema

См. [contracts/proto/fob/execution/v1/execution.proto](../../../contracts/proto/fob/execution/v1/execution.proto).

## Used In Features

- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/)
- [F-17. Monitoring](../../02-system/features/F-17-monitoring-and-alerts/)

## Used In Use Cases

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md)
- [UC-F17-01](../../02-system/use-cases/UC-F17-01-fire-alert/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F12-UC-F12-01-services](../../05-components/sequences/SEQ-F12-UC-F12-01-services.md)
- [SEQ-F17-UC-F17-01-services](../../05-components/sequences/SEQ-F17-UC-F17-01-services.md)
