# Kafka Topic: batch.outputs

## Purpose

Результаты каждого batch clearing цикла: clearing prices, fills, order updates, diagnostics.

## Producer

- [matching-fob-core](../../05-components/matching-fob-core/overview.md)

## Consumers

- [ledger](../../05-components/ledger/overview.md) — applies fills to balances/positions
- [risk-manager](../../05-components/risk-manager/overview.md) — post-trade risk
- [observability-reporting](../../05-components/observability-reporting/overview.md) — diagnostics
- (planned) WebSocket gateway → UI (fill events)

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней (целевая 30 для audit) |
| Partition key | `batch_id` |
| Delivery | at-least-once, idempotent consumer |
| Schema | `fob.matching.v1.BatchResult` (Protobuf) |

## Message schema

См. [contracts/proto/fob/matching/v1/batch.proto](../../../contracts/proto/fob/matching/v1/batch.proto).

Ключевые сущности: `BatchResult`, `Fill`, `OrderUpdate`, `BatchDiagnostics`.

## Used In Features

- [F-04. Batch Clearing](../../02-system/features/F-04-batch-clearing/)
- [F-06. Positions](../../02-system/features/F-06-positions-pnl-margin/)
- [F-08. Liquidations](../../02-system/features/F-08-posttrade-risk-and-liquidations/)
- [F-13. Post-Trade Report](../../02-system/features/F-13-posttrade-report/)
- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/)
- [F-17. Monitoring](../../02-system/features/F-17-monitoring-and-alerts/)

## Used In Use Cases

- [UC-F04-01](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md)
- [UC-F08-01](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md)
- [UC-F13-01](../../02-system/use-cases/UC-F13-01-generate-posttrade-report/use-case.md)
- [UC-F15-01](../../02-system/use-cases/UC-F15-01-replay-historical-batch/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F04-UC-F04-01-services](../../05-components/sequences/SEQ-F04-UC-F04-01-services.md)
- [SEQ-F08-UC-F08-01-services](../../05-components/sequences/SEQ-F08-UC-F08-01-services.md)
- [SEQ-F13-UC-F13-01-services](../../05-components/sequences/SEQ-F13-UC-F13-01-services.md)
- [SEQ-F15-UC-F15-01-services](../../05-components/sequences/SEQ-F15-UC-F15-01-services.md)
- [SEQ-F17-UC-F17-01-services](../../05-components/sequences/SEQ-F17-UC-F17-01-services.md)
