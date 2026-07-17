# gRPC Service: VectorLiquidityService (F-05A)

## Status

`planned` — proto определён (`contracts/proto/fob/marketdata/v1/vector_liquidity.proto`),
серверная реализация не поставлена (T-F05A-1xx/2xx/3xx). Read-side диагностика vector
clearing. Выделен в **отдельный сервис** (не расширяет `MarketDataService`) для
минимизации blast-radius.

## Service

`fob.marketdata.v1.VectorLiquidityService`

## Transport

gRPC (вызывается gateway для UI; internal-диагностика).

## Methods

```proto
rpc GetVectorizedLiquidity(GetVectorizedLiquidityRequest)
    returns (GetVectorizedLiquidityResponse);
rpc GetVectorClearingDiagnostics(GetVectorClearingDiagnosticsRequest)
    returns (GetVectorClearingDiagnosticsResponse);
```

### GetVectorizedLiquidity

Возвращает `VectorClearingInput` (AssetBasis + VectorFlowSegment[] = столбцы `W`,
`pH`, `dHL`, `q`, source_map) для symbols или captured fixture.

- `GetVectorizedLiquidityRequest`: `repeated string symbols`, `string fixture_id` (опц.).
- `GetVectorizedLiquidityResponse`: `VectorClearingInput input`, `fob.common.v1.Error error`.

### GetVectorClearingDiagnostics

Возвращает `VectorClearingResult` (x, π, residual, residualNorm, solver_status,
surplus, diagnostics) по `execution_group_id` / `batch_id`.

- `GetVectorClearingDiagnosticsRequest`: `string execution_group_id`, `string batch_id`.
- `GetVectorClearingDiagnosticsResponse`: `VectorClearingResult result`, `fob.common.v1.Error error`.

## Money

Все денежные/количественные поля — `fob.common.v1.Decimal` (§9). `residual_norm` —
`double` (диагностика, допустимо §9).

## Backward compatibility

Новый сервис + новые messages — аддитивно, существующие контракты не затрагиваются.

## Used In

- Feature: [F-05A](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use cases: [UC-F05A-01](../../02-system/use-cases/UC-F05A-01-vectorize-external-orderbook/use-case.md), [UC-F05A-02](../../02-system/use-cases/UC-F05A-02-vector-clearing/use-case.md), [UC-F05A-03](../../02-system/use-cases/UC-F05A-03-display-vector-liquidity/use-case.md)
- Sequences: [SEQ-F05A-UC-F05A-01-services](../../05-components/sequences/SEQ-F05A-UC-F05A-01-services.md), [SEQ-F05A-UC-F05A-03-services](../../05-components/sequences/SEQ-F05A-UC-F05A-03-services.md)
- Kafka: [marketdata.vectorized](../messaging/marketdata-vectorized.md) (envelope `VectorizedLiquiditySnapshot`)

## Proto

- `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (proto-map group `vector_liquidity`)

## Source

- IN-014 §7.1 / §10.1 (fragment F-07).
