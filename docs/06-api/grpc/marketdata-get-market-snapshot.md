# gRPC: MarketDataService / GetMarketDataSnapshot

## Purpose

Возвращает текущий `MarketDataSnapshot` по инструменту из in-memory кэша Market
Data Service (fast path, p95 < 50 ms). Используется gateway для REST
`GET /api/v1/marketdata/{asset}` и WebSocket-стрима.

## Service

- [market-data](../../05-components/market-data/overview.md)

## Method

| | |
| --- | --- |
| Service | `fob.marketdata.v1.MarketDataService` |
| Method | `GetMarketDataSnapshot` |
| Request | `GetMarketDataSnapshotRequest { asset }` |
| Response | `GetMarketDataSnapshotResponse { found, snapshot, error }` |

## Schema

См. [contracts/proto/fob/marketdata/v1/marketdata_service.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_service.proto).

## Related

- Feature: F-05 Live Market Data
- Sequence: [SEQ-F05-UC-F05-01-services.md](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)
