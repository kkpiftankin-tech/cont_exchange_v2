# Topic: marketdata.vectorized

> **Status:** `TODO / planned` — proposed schema sketch (F-05A, IN-014). Материализуется
> на ingress-close (T-F05A-101/102) вместе с `contracts/proto/fob/marketdata/v1/vector_liquidity.proto`.

| Свойство | Значение |
| --- | --- |
| Producer | `market-data` |
| Consumers | `matching`, `backtest`, `observability` |
| Message type | `fob.marketdata.v1.VectorizedLiquiditySnapshot` (planned) — несёт `VectorClearingInput` (W, pH, dHL, q, D, source_map, AssetBasis) |
| Partition key | `symbol` / `batch_id` |
| Delivery | at-least-once + idempotent consumer (по `batch_id`) |
| Retention | short (market data class) |
| Создаётся | `infra/kafka/create_topics.sh` (T-F05A-102) |

## Purpose

Канал передачи vectorized внешней ликвидности от `market-data` к `matching`:
каждый внешний order level представлен вектором активов `w_i` (столбец матрицы `W`),
без синтетической pair-book. Matching решает QP `Wx=0` (см.
[SEQ-F05A-UC-F05A-02-services](../../05-components/sequences/SEQ-F05A-UC-F05A-02-services.md)).

## Proposed schema sketch

```proto
message VectorizedLiquiditySnapshot {
  fob.common.v1.EventMeta meta        = 1;
  string batch_id                      = 2;
  AssetBasis basis                     = 3;   // ordered assets a_1..a_N
  repeated VectorFlowSegment segments  = 4;   // столбцы W (+ source_map)
  // pH, dHL, q, D выводятся из segments; см. vector_liquidity.proto (planned)
}
```

Полные messages (`ExternalOrderLevel`, `AssetBasis`, `VectorFlowSegment`,
`VectorClearingInput`, `VectorClearingResult`, `SurplusEvent`) — в
`contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (T-F05A-101). Денежные/
количественные поля — `fob.common.v1.Decimal` (CLAUDE.md §9).

## Used In

- Feature: [F-05A](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use cases: [UC-F05A-01](../../02-system/use-cases/UC-F05A-01-vectorize-external-orderbook/use-case.md) (producer path), [UC-F05A-02](../../02-system/use-cases/UC-F05A-02-vector-clearing/use-case.md) (consumer path)
- Sequences: [SEQ-F05A-UC-F05A-01-services](../../05-components/sequences/SEQ-F05A-UC-F05A-01-services.md), [SEQ-F05A-UC-F05A-02-services](../../05-components/sequences/SEQ-F05A-UC-F05A-02-services.md)

## Source

- IN-014 §7.2 / §10.2 (fragment F-08). Register topic + proto on ingress-close.
