# Kafka Topic: marketdata.raw

## Purpose

Сырые market data события из внешних площадок: тикеры, сделки, обновления стакана.

## Producer

- [external-venues](../../05-components/external-venues/overview.md)

## Consumers

- [market-data](../../05-components/market-data/overview.md) — обновляет ticker cache в Redis
- (planned) [matching-fob-core](../../05-components/matching-fob-core/overview.md) — для FOB conversion features

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 1 час (raw), ClickHouse — для истории |
| Partition key | `venue|symbol` |
| Delivery | at-least-once |
| Schema | `fob.marketdata.v1.MarketDataRaw` (Protobuf) |

## Message schema

См. [contracts/proto/fob/marketdata/v1/marketdata_raw.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_raw.proto).

Ключевые: `Ticker`, `Trade`, `OrderBookL2Update`.

## Used In Features

- [F-05. Live Market Data](../../02-system/features/F-05-live-market-data/)
- [F-11. External Venues](../../02-system/features/F-11-external-venues-lob-to-fob/)

## Used In Use Cases

- [UC-F05-01](../../02-system/use-cases/UC-F05-01-stream-market-data/use-case.md)
- [UC-F11-01](../../02-system/use-cases/UC-F11-01-ingest-external-marketdata/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F05-UC-F05-01-services](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)
- [SEQ-F11-UC-F11-01-services](../../05-components/sequences/SEQ-F11-UC-F11-01-services.md)
