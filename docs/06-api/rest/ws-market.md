# WebSocket: /api/v1/market

## Purpose

Live-канал рыночных данных (F-05). Клиент подписывается на инструмент и получает
полный `MarketDataSnapshot` при подписке + обновления на каждый пересчёт.

## Endpoint

| | |
| --- | --- |
| URL | `ws://<gateway>/api/v1/market` |
| Component | [gateway](../../05-components/gateway/overview.md) → [market-data](../../05-components/market-data/overview.md) (gRPC `SubscribeMarketData`) |

## Protocol

| Направление | Сообщение |
| --- | --- |
| Client → Server | `{"action":"subscribe","asset":"BTCUSDT"}` |
| Client → Server | `{"action":"unsubscribe","asset":"BTCUSDT"}` |
| Server → Client | `{"type":"snapshot", ...}` (полный снимок: mid, best_bid/ask, spread, depth, source) |
| Server → Client | `{"type":"update", ...}` |

## Schema

Полезная нагрузка — поля `fob.marketdata.v1.MarketDataSnapshot`. См.
[contracts/proto/fob/marketdata/v1/marketdata_service.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_service.proto).

## Related

- Feature: F-05 Live Market Data
- Sequence: [SEQ-F05-UC-F05-01-services.md](../../05-components/sequences/SEQ-F05-UC-F05-01-services.md)
