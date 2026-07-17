<!--
---
id: UC-F05A-03
title: "Display Vector Liquidity in UI"
level: sea
parent-feature: F-05A
system-sequence: "sequences/SEQ-UC-F05A-03-system.md"
service-sequence: "../../../05-components/sequences/SEQ-F05A-UC-F05A-03-services.md"
---
-->

# UC-F05A-03. Display Vector Liquidity in UI

## Feature

- [F-05A. Vectorized External Liquidity](../../features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)

## Primary Actor

Trader / Provider / Operator (Web UI)

## Supporting Actors

- gateway (REST/WebSocket), market-data, matching (diagnostics), ClickHouse

## Preconditions

- Vectorization / clearing выполнены (UC-F05A-01/02); диагностика в ClickHouse.
- REST/WS API `vector-liquidity` доступны.

## Trigger

Пользователь открывает Vector Liquidity Explorer / выбирает symbol / execution group.

## Main Flow

1. UI запрашивает через gateway `GET /api/v1/marketdata/vector-liquidity` (или `/{fixtureId}/W`, `/solver-result`, `/execution-groups/{id}/vector-diagnostics`).
2. gateway вызывает `MarketDataService.GetVectorizedLiquidity` / `GetVectorClearingDiagnostics` (или читает CH).
3. UI отображает **доказательные графики**: raw order book depth, W heatmap, execution vector `x_i`, **asset residual `Wx`**, residualNorm-over-time, clearing graph, source-trace, surplus/EXCHANGE_PNL.
4. Клик по сегменту `w_i` / fill открывает source venue order level (venue_id, source_order_id, raw/effective price).
5. WS `/api/market/vector` пушит обновления в пределах F-05 latency budget.

## Alternative Flows

### A1. Residual выше tolerance

1. UI визуально выделяет `residualNorm > tolerance` (warning) и показывает surplus по активам (AC-F05A-UI-006/009).

## Postconditions

- Пользователь видит корректность подхода: `Wx ≈ 0` либо явный surplus; каждый fill трассируется к venue level.

## Related Sequence Diagrams

- System sequence: [sequences/SEQ-UC-F05A-03-system.md](sequences/SEQ-UC-F05A-03-system.md)
- Service sequence: [../../../05-components/sequences/SEQ-F05A-UC-F05A-03-services.md](../../../05-components/sequences/SEQ-F05A-UC-F05A-03-services.md)

## Related Contracts

- REST `/api/v1/marketdata/vector-liquidity*`, WS `/api/market/vector` (planned, T-F05A-103); gRPC `GetVectorizedLiquidity` / `GetVectorClearingDiagnostics`

## Related Components

- `gateway`, `market-data`, `matching`

## Related Data

- CH `vector_clearing_results`, `vector_flow_segments_history` (planned)
