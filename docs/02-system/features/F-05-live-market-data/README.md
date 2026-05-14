# F-05 — Live Market Data

## Описание

Потоковые рыночные данные: тикеры с внешних venues, кэшированные и доступные через gRPC.

## Поток

```
venues → Kafka(marketdata.raw) → market_data → cache → gRPC GetLastTicker
```

## Файлы

- [cpp/venues/src/app/venues_loop.cpp](../../../../cpp/venues/src/app/venues_loop.cpp) — синтетический поток (MVP)
- [cpp/market_data/src/app/market_data_uc.cpp](../../../../cpp/market_data/src/app/market_data_uc.cpp) — кэш
- [cpp/market_data/src/transport/grpc_market_data_service.cpp](../../../../cpp/market_data/src/transport/grpc_market_data_service.cpp) — API

## Связанные

- F-04 (Batch Clearing) — потребитель reference prices
- F-11 (External Venues LOB → FOB) — расширение источников

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна показывать пользователю актуальные цены, объёмы, скорости исполнения и общую картину ликвидности по каждому торгуемому инструменту.

- Источник внутренних данных — `batch.outputs` (clearing prices, executed rates).
- Источник внешних — `marketdata.raw` от CEX/DEX (см. F-11).
- Streaming по WebSocket с приемлемой latency (NFR-EXEC).
- Ticker cache в Redis для быстрого ответа.

Источник: IN-001 §6 FR-MD-001, §5 продуктовая модель.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
