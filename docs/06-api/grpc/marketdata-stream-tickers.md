# gRPC Method: MarketDataService/StreamTickers

## Status

TODO / planned (в текущем `marketdata_raw.proto` есть только `GetLastTicker`)

## Purpose

Серверный stream тикеров по подписке: клиент задаёт фильтр `(venue, symbols)`, сервер отправляет каждое обновление по мере появления (push-mode). Используется WebSocket-gateway для realtime-обновлений UI, MM-bot-ами и mid-price-listener-ами.

## Transport

gRPC server-streaming

## Service

`fob.marketdata.v1.MarketDataService` (planned extension)

## Method (planned)

```proto
rpc StreamTickers(StreamTickersRequest) returns (stream Ticker);
```

## Caller

- [gateway](../../05-components/gateway/overview.md) (для WS-bridge)
- MM-bots
- [matching-fob-core](../../05-components/matching-fob-core/overview.md) (опционально, для свежих опорных цен между batch)

## Callee

- [market-data](../../05-components/market-data/overview.md)

## Schema

TODO. Предлагаемая форма:

```proto
message StreamTickersRequest {
  fob.common.v1.EventMeta meta = 1;
  repeated string venues = 2;            // empty = all
  repeated string symbols = 3;           // canonical "BTC/USDT", empty = all
  bool include_snapshot = 4;             // отправить snapshot последних значений до старта стрима
}

// Stream of fob.marketdata.v1.Ticker (already defined in marketdata_raw.proto)
```

## Delivery semantics

- at-most-once в рамках одного stream-а;
- при разрыве клиент сам реконнектится и запрашивает свежий snapshot;
- backpressure через стандартный gRPC flow-control.

## Used In Features

- [F-05. Live Market Data](../../02-system/features/F-05-live-market-data/)

## Used In Use Cases

- [UC-F05-01](../../02-system/use-cases/UC-F05-01-stream-market-data/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F05-UC-F05-01-services](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)

## Related Components

- [market-data](../../05-components/market-data/overview.md)
- [gateway](../../05-components/gateway/overview.md)

## Related Data Objects

- Redis cache last_ticker
- Kafka `marketdata.raw`

## Source Fragments

- IN-001-FR-016 (FR-MD-002 stream, derived)
- IN-001-FR-022
