# Компонент: venues

Адаптеры к внешним торговым площадкам. В MVP — симулятор: генерирует синтетические тикеры в `marketdata.raw` и отвечает на `execution.intents` мгновенным "filled" в `execution.reports`.

## Код

- [cpp/venues/](../../../cpp/venues/) — каталог
- [src/main.cpp](../../../cpp/venues/src/main.cpp) — entrypoint
- [src/app/venues_loop.cpp](../../../cpp/venues/src/app/venues_loop.cpp) — два фоновых потока: MD publisher + exec consumer

## Что делает (MVP)

1. **MD publisher** ([venues_loop.cpp:47-78](../../../cpp/venues/src/app/venues_loop.cpp#L47-L78)) — раз в 500 мс публикует Ticker по `binance|BTC/USDT` с осциллирующей ценой около 100.00.
2. **Exec consumer** ([venues_loop.cpp:80-130](../../../cpp/venues/src/app/venues_loop.cpp#L80-L130)) — на каждый `ExecutionIntent` сразу публикует `ExecutionReport` со статусом `FILLED`.

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `KAFKA_BROKERS` | redpanda:9092 | Kafka |

## Что должно быть в проде

- Реальные адаптеры к Binance/Coinbase/... (через CCXT или нативные REST/WS).
- Преобразование внешнего LOB в FOB-формат (F-11).
- Учёт rate-limits, аутентификации, реконнектов.

## Связанные фичи

- F-11 (External Venues LOB → FOB)
- F-12 (Execution Hedge)
- F-05 (Live Market Data) — поставщик `marketdata.raw`

## Participates In Features

- [F-05](../../02-system/features/F-05-live-market-data/), [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/), [F-12](../../02-system/features/F-12-execution-hedge/)

## Participates In Use Cases

- [UC-F05-01](../../02-system/use-cases/UC-F05-01-stream-market-data/use-case.md), [UC-F11-01](../../02-system/use-cases/UC-F11-01-ingest-external-marketdata/use-case.md), [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F05-UC-F05-01-services](../sequences/SEQ-F05-UC-F05-01-services.md), [SEQ-F11-UC-F11-01-services](../sequences/SEQ-F11-UC-F11-01-services.md), [SEQ-F12-UC-F12-01-services](../sequences/SEQ-F12-UC-F12-01-services.md), [SEQ-F16-UC-F16-01-services](../sequences/SEQ-F16-UC-F16-01-services.md)

## Owned Contracts

- venue-specific REST/WS clients (не публичные внутренние)

## Produced Events

- [marketdata.raw](../../06-api/messaging/marketdata-raw.md)
- [execution.reports](../../06-api/messaging/execution-reports.md)

## Consumed Events

- [execution.intents](../../06-api/messaging/execution-intents.md)

## Data Access

- (planned) `marketdata` (ClickHouse), `execution_reports` (ClickHouse) — [../../07-data/data-overview.md](../../07-data/data-overview.md)
