# Kafka Topic: execution.intents

## Purpose

Намерения внешнего хеджирования (child orders) — что нужно исполнить на внешней площадке.

## Producer

- [risk-manager](../../05-components/risk-manager/overview.md) (planned)
- [matching-fob-core](../../05-components/matching-fob-core/overview.md) (planned execution policy)

## Consumers

- [external-venues](../../05-components/external-venues/overview.md)

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 1 день |
| Partition key | `intent_id` |
| Delivery | at-least-once, idempotent consumer |
| Schema | `fob.execution.v1.ExecutionIntent` (Protobuf) |

## Message schema

См. [contracts/proto/fob/execution/v1/execution.proto](../../../contracts/proto/fob/execution/v1/execution.proto).

## Used In Features

- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/)

## Used In Use Cases

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F12-UC-F12-01-services](../../05-components/sequences/SEQ-F12-UC-F12-01-services.md)
