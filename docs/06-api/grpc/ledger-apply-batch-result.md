# gRPC Method: LedgerService/ApplyBatchResult

## Status

draft (proto-defined; sync RPC is alternative to Kafka consumer)

## Purpose

Применить результат batch clearing к балансам и позициям пользователей (apply fills). Используется как синхронная альтернатива потреблению Kafka `batch.outputs` — преимущественно в тестах, replay-сценариях или внутренних сервисных вызовах. В production-MVP основным каналом остаётся Kafka.

## Transport

gRPC

## Service

`fob.ledger.v1.LedgerService`

## Method

```proto
rpc ApplyBatchResult(ApplyBatchResultRequest) returns (ApplyBatchResultResponse);
```

## Caller

- [matching-fob-core](../../05-components/matching-fob-core/overview.md) — внутренний `LedgerClient` оркестратора `RunBatch`
- replay / backtest сервисы (планируется)
- интеграционные тесты

## Callee

- [ledger](../../05-components/ledger/overview.md)

## Schema

См. [contracts/proto/fob/ledger/v1/ledger.proto](../../../contracts/proto/fob/ledger/v1/ledger.proto).

```proto
message ApplyBatchResultRequest {
  fob.common.v1.EventMeta meta = 1;
  fob.matching.v1.BatchResult batch = 2;
}

message ApplyBatchResultResponse {
  fob.common.v1.EventMeta meta = 1;
  bool success = 2;
  fob.common.v1.Error error = 3;
}
```

## Idempotency

Идемпотентность по ключу `(batch_id, order_id, fill_id)`. Повторная доставка не должна порождать повторные балансовые операции.

## Used In Features

- [F-04. Batch Clearing](../../02-system/features/F-04-batch-clearing/)
- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/)

## Used In Use Cases

- [UC-F04-01. Run Batch Clearing](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md)

## Used In Sequence Diagrams

- [SEQ-MATCHING-001-solver-cycle](../../05-components/matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md)
- [SEQ-F04-UC-F04-01-services](../../05-components/sequences/SEQ-F04-UC-F04-01-services.md)

## Related Components

- [ledger](../../05-components/ledger/overview.md)
- [matching-fob-core](../../05-components/matching-fob-core/overview.md)

## Related Data Objects

- [`fills`](../../07-data/olap-schema.md#таблица-fills)
- [`batch_results`](../../07-data/olap-schema.md#таблица-batch_results)
- `user_balances`, `positions` (OLTP)

## Source Fragments

- IN-001-FR-022, IN-001-FR-023
- IN-003-FR-017, IN-003-FR-021
