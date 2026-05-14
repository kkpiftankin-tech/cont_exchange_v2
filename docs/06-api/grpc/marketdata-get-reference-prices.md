# gRPC Method: MarketDataService/GetReferencePrices

## Status

TODO / planned

## Purpose

Получить набор reference prices (mid / bid / ask) для списка активов на момент `ts_batch`. Используется matching-fob-core перед запуском solver, чтобы зафиксировать опорные цены batch-цикла.

В отличие от `GetLastTicker` (per-symbol), этот метод оптимизирован для batch-чтения нескольких активов одним вызовом и должен гарантировать, что все возвращаемые цены относятся к одному и тому же логическому моменту времени (либо снимок ближайший к `ts_batch`).

## Transport

gRPC

## Service

`fob.marketdata.v1.MarketDataService` (planned extension)

## Method (planned)

```proto
rpc GetReferencePrices(GetReferencePricesRequest) returns (GetReferencePricesResponse);
```

## Caller

- [matching-fob-core](../../05-components/matching-fob-core/overview.md) — внутренний `MarketDataClient` оркестратора `RunBatch`

## Callee

- [market-data](../../05-components/market-data/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message GetReferencePricesRequest {
  fob.common.v1.EventMeta meta = 1;
  repeated string assets = 2;           // e.g. ["BTC", "ETH", "USDT"]
  google.protobuf.Timestamp ts_batch = 3;
}

message ReferencePrice {
  string asset = 1;
  fob.common.v1.Decimal mid = 2;
  fob.common.v1.Decimal bid = 3;
  fob.common.v1.Decimal ask = 4;
  google.protobuf.Timestamp as_of = 5;
}

message GetReferencePricesResponse {
  fob.common.v1.EventMeta meta = 1;
  repeated ReferencePrice prices = 2;
  fob.common.v1.Error error = 3;
}
```

## Used In Features

- [F-04. Batch Clearing](../../02-system/features/F-04-batch-clearing/)

## Used In Use Cases

- [UC-F04-01. Run Batch Clearing](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md)

## Used In Sequence Diagrams

- [SEQ-MATCHING-001-solver-cycle](../../05-components/matching-fob-core/sequences/SEQ-MATCHING-001-solver-cycle.md)
- [SEQ-F04-UC-F04-01-services](../../05-components/sequences/SEQ-F04-UC-F04-01-services.md)

## Related Components

- [market-data](../../05-components/market-data/overview.md)
- [matching-fob-core](../../05-components/matching-fob-core/overview.md)

## Related Data Objects

- `marketdata` (ClickHouse) — историческая последовательность тиков
- Redis cache last_ticker (current state)

## Source Fragments

- IN-003-FR-017 (Scheduler → MB → MDS)
- IN-003-FR-019 (входы solver: reference prices)
- IN-003-FR-021 (RunBatch signature)
