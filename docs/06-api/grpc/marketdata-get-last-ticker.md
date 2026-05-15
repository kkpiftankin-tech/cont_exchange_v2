# gRPC Method: MarketDataService/GetLastTicker

## Status

draft (proto-defined)

## Purpose

Получить последний известный ticker `(bid, ask, last, ts)` по `(venue, symbol)` из кэша Market Data Service. Используется UI/Gateway для отображения текущей цены, Risk Manager — как `reference_price` в `CheckNewOrder`, внутренними сервисами для notional-калькуляций.

## Transport

gRPC

## Service

`fob.marketdata.v1.MarketDataService`

## Method

```proto
rpc GetLastTicker(GetLastTickerRequest) returns (GetLastTickerResponse);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (REST `GET /v1/marketdata/ticker` → gRPC)
- [risk-manager](../../05-components/risk-manager/overview.md) (reference price)
- [order-flow](../../05-components/order-flow/overview.md) (notional snapshot)

## Callee

- [market-data](../../05-components/market-data/overview.md)

## Schema

См. [contracts/proto/fob/marketdata/v1/marketdata_raw.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_raw.proto).

```proto
message GetLastTickerRequest {
  fob.common.v1.EventMeta meta = 1;
  string venue = 2;
  string symbol = 3;     // canonical "BTC/USDT"
}

message GetLastTickerResponse {
  fob.common.v1.EventMeta meta = 1;
  bool found = 2;
  Ticker ticker = 3;
  fob.common.v1.Error error = 4;
}
```

## Used In Features

- [F-05. Live Market Data](../../02-system/features/F-05-live-market-data/)
- [F-11. External Venues LOB→FOB](../../02-system/features/F-11-external-venues-lob-to-fob/)

## Used In Use Cases

- [UC-F05-01](../../02-system/use-cases/UC-F05-01-stream-market-data/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F05-UC-F05-01-services](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)

## Related Components

- [market-data](../../05-components/market-data/overview.md)
- [external-venues](../../05-components/external-venues/overview.md) (источник `marketdata.raw`)

## Related Data Objects

- Redis cache last_ticker (key: `(venue, symbol)`)
- ClickHouse `marketdata` (historical)
- Kafka `marketdata.raw` (источник)

## Source Fragments

- IN-001-FR-016 (FR-MD-001 ticker)
- IN-001-FR-022
