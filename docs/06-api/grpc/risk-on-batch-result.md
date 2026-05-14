# gRPC Method: RiskService/OnBatchResult

## Status

draft (proto-defined; sync alternative to Kafka consumer)

## Purpose

Передать `BatchResult` в Risk Manager синхронно для post-trade обновления экспозиций, расчёта PnL и потенциального триггера ликвидаций. Используется как альтернатива потреблению Kafka `batch.outputs` — преимущественно в тестах, replay-сценариях или внутренних вызовах. В production-MVP основной канал — Kafka.

## Transport

gRPC

## Service

`fob.risk.v1.RiskService`

## Method

```proto
rpc OnBatchResult(PostTradeUpdateRequest) returns (google.protobuf.Empty);
```

## Caller

- [matching-fob-core](../../05-components/matching-fob-core/overview.md) (опционально, в sync-режиме)
- replay / backtest сервисы

## Callee

- [risk-manager](../../05-components/risk-manager/overview.md)

## Schema

См. [contracts/proto/fob/risk/v1/risk.proto](../../../contracts/proto/fob/risk/v1/risk.proto).

```proto
message PostTradeUpdateRequest {
  fob.common.v1.EventMeta meta = 1;
  fob.matching.v1.BatchResult batch = 2;
}
```

## Idempotency

По `batch_id`. Повторное применение того же batch не должно дублировать post-trade alerts или ликвидации.

## Used In Features

- [F-08. Post-Trade Risk and Liquidations](../../02-system/features/F-08-posttrade-risk-and-liquidations/)
- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/)

## Used In Use Cases

- [UC-F08-01. Liquidate Position](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F08-UC-F08-01-services](../../05-components/sequences/SEQ-F08-UC-F08-01-services.md)

## Related Components

- [risk-manager](../../05-components/risk-manager/overview.md)
- [matching-fob-core](../../05-components/matching-fob-core/overview.md)
- [ledger](../../05-components/ledger/overview.md)

## Related Data Objects

- [`positions`](../../07-data/oltp-schema.md#таблица-positions)
- [`risk_snapshots`](../../07-data/oltp-schema.md#таблица-risk_snapshots)
- Kafka `risk.alerts` (производит `MARGIN_CALL`, `LIQUIDATION_TRIGGERED`)

## Source Fragments

- IN-001-FR-016 (FR-RISK-002 post-trade)
- IN-001-FR-022, IN-001-FR-023
