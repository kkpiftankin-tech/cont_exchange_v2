# Компонент: market-data

In-memory кэш последних тикеров по парам (venue, symbol). Подписан на `marketdata.raw`, отдаёт gRPC `GetLastTicker`.

## Код

- [cpp/market_data/](../../../cpp/market_data/) — каталог
- [src/main.cpp](../../../cpp/market_data/src/main.cpp) — entrypoint
- [src/app/market_data_uc.cpp](../../../cpp/market_data/src/app/market_data_uc.cpp) — кэш
- [src/transport/grpc_market_data_service.cpp](../../../cpp/market_data/src/transport/grpc_market_data_service.cpp) — gRPC сервер
- [src/infra/kafka_consumer.cpp](../../../cpp/market_data/src/infra/kafka_consumer.cpp) — consumer `marketdata.raw`

## gRPC API

`fob.marketdata.v1.MarketDataService` (см. [marketdata_service.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_service.proto)):

| RPC | Что делает |
|---|---|
| `GetLastTicker` | Вернуть последний Ticker по `(venue, symbol)` или `found=false` |

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `MARKET_DATA_GRPC_LISTEN` | 0.0.0.0:50054 | Адрес gRPC |
| `KAFKA_BROKERS` | redpanda:9092 | Kafka |

## Ограничения MVP

- **Только Ticker**. Trade и OrderBookL2Update пропускаются ([market_data_uc.cpp:20](../../../cpp/market_data/src/app/market_data_uc.cpp#L20)).
- **Нет OHLCV / свечей / истории.**
- **Нет стрим-подписки** для UI.
- **Composite key `venue|symbol`** — без normalization.

## Связанные фичи

- F-05 (Live Market Data)
- F-11 (External Venues LOB → FOB) — потребитель данных от venues

## Participates In Features

- [F-05](../../02-system/features/F-05-live-market-data/), [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/)

## Participates In Use Cases

- [UC-F05-01](../../02-system/use-cases/UC-F05-01-stream-market-data/use-case.md), [UC-F11-01](../../02-system/use-cases/UC-F11-01-ingest-external-marketdata/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F05-UC-F05-01-services](../sequences/SEQ-F05-UC-F05-01-services.md), [SEQ-F11-UC-F11-01-services](../sequences/SEQ-F11-UC-F11-01-services.md)

## Owned Contracts

- `fob.marketdata.v1.MarketDataService` (`GetLastTicker`, planned `StreamTickers`) — [../../06-api/grpc/](../../06-api/grpc/)

## Produced Events

- (none)

## Consumed Events

- [marketdata.raw](../../06-api/messaging/marketdata-raw.md)

## Data Access

- Redis (ticker cache `last_ticker:{symbol}`) — [../../07-data/data-overview.md](../../07-data/data-overview.md)
