# Kafka Topic: marketdata.snapshots

## Purpose

Полный `MarketDataSnapshot` по инструменту, публикуемый Market Data Service (F-05)
после каждого пересчёта (BatchResult или внешний venue-апдейт): mid, best_bid/ask,
spread, spread_bps, volume_24h, bid_depth/ask_depth, clear_price, executed_rate, source.

## Producer

- [market-data](../../05-components/market-data/overview.md)

## Consumers

- WebSocket gateway → UI (live стакан F-05)
- (planned) downstream-аналитика / F-13

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней |
| Partition key | `asset` |
| Delivery | at-least-once |
| Schema | `fob.marketdata.v1.MarketDataSnapshot` (Protobuf) |

## Message schema

См. [contracts/proto/fob/marketdata/v1/marketdata_service.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_service.proto).

## Related

- Feature: F-05 Live Market Data
- Sequence: [SEQ-F05-UC-F05-01-services.md](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)
