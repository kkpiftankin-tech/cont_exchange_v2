<!--
---
id: UC-F05A-01
title: "Vectorize External Order Book"
level: sea
parent-feature: F-05A
system-sequence: "sequences/SEQ-UC-F05A-01-system.md"
service-sequence: "../../../05-components/sequences/SEQ-F05A-UC-F05A-01-services.md"
---
-->

# UC-F05A-01. Vectorize External Order Book

## Feature

- [F-05A. Vectorized External Liquidity](../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Primary Actor

Market Data Service / Venue Liquidity Builder

## Supporting Actors

- External Venues Connector (`venues`)
- Kafka (`marketdata.raw`, `venue.liquidity.fob`, `marketdata.vectorized`)
- Matching Backend (потребитель)

## Preconditions

- Список supported assets (asset basis) известен и детерминирован.
- Venue snapshots / нормализованные levels доступны (`marketdata.raw` или F-11 `venue.liquidity.fob`).
- Fees / latency-buffer / slippage-buffer configuration задана.
- `dHL` (smoothing width) policy задана (per-venue / default).

## Trigger

Поступление нового `VenueSnapshot` / нормализованного набора внешних order levels
(или тик batch-таймера market_data).

## Main Flow

1. `venues` публикует `VenueSnapshot` / external order levels в `marketdata.raw`
   (или F-11 публикует нормализованные levels в `venue.liquidity.fob`).
2. `market-data` получает bid/ask levels.
3. Для каждого level система вычисляет **effective price**
   `P_eff = P ± fees ± latency_buffer ± slippage_buffer` и приводит quantity к base units.
4. Система строит **вектор активов** `w_i`: bid `w = e_X − P_eff·e_Y`; ask `w = −e_X + P_eff·e_Y`.
5. Для каждого `w_i` строится **flow-сегмент** `(w_i, p^L=0, p^H=dHL, q=min(Q,rateCap), Q^max=Q)`.
6. Система собирает `VectorClearingInput`: `W = [w_1 … w_I]`, `pH`, `dHL`, `q`,
   `D = diag(dHL/q)`, `source_map` (venue_id / source_order_id / segment_id).
7. Vectorized liquidity публикуется в `marketdata.vectorized` (для Matching / Backtest / Observability)
   и сегменты персистятся в ClickHouse (`vector_flow_segments_history`).

## Alternative Flows

### A1. Невалидный / stale level

1. Неизвестная pair, `quantity ≤ 0` или превышен staleness threshold → level пропускается.
2. Событие staleness учитывается метрикой `stale_external_levels_total` (KI-F05A-004).

### A2. Ошибка размерности

1. `dimensional_guard` обнаруживает несовместные единицы (base/quote/price/rate) → level отклоняется, эмитится diagnostic (KI-F05A-003).

## Postconditions

- Создан набор `VectorFlowSegment` (каждый внешний level — отдельный сегмент, provenance сохранён; **no synthetic pair book**).
- Создан `VectorClearingInput` для batch.
- Все сегменты связаны с исходными venue order levels (source-trace).

## Related Sequence Diagrams

- System sequence: [sequences/SEQ-UC-F05A-01-system.md](sequences/SEQ-UC-F05A-01-system.md)
- Service sequence: [../../../05-components/sequences/SEQ-F05A-UC-F05A-01-services.md](../../../05-components/sequences/SEQ-F05A-UC-F05A-01-services.md)

## Related Contracts

- `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (planned, T-F05A-101)
- Kafka `marketdata.vectorized` (planned, T-F05A-102); consumes `marketdata.raw` / F-11 `venue.liquidity.fob`

## Related Components

- `market-data`, `external-venues` / `venues`, `matching`

## Related Data

- PG `asset_basis`, `vector_flow_segments`, `vectorization_runs` (planned)
- CH `vector_flow_segments_history` (planned)
