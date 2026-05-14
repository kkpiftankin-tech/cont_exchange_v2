# Event-Driven Architecture

Асинхронное взаимодействие между сервисами идёт через Kafka/Redpanda. Топики создаются скриптом [infra/kafka/create_topics.sh](../../infra/kafka/create_topics.sh).

## Топики и контракты

| Топик | Producer | Consumer(s) | Proto-тип сообщения | Partition key |
|---|---|---|---|---|
| `marketdata.raw` | [cpp/venues](../../cpp/venues/) | [cpp/market_data](../../cpp/market_data/) | `fob.marketdata.v1.MarketDataRaw` | `venue\|symbol` |
| `orders.normalized` | [cpp/order_flow](../../cpp/order_flow/) | [cpp/matching](../../cpp/matching/) | `fob.orders.v1.OrdersNormalized` | `symbol` |
| `batch.outputs` | [cpp/matching](../../cpp/matching/) | [cpp/ledger](../../cpp/ledger/), [cpp/observability](../../cpp/observability/) | `fob.matching.v1.BatchResult` | `batch_id` |
| `execution.intents` | (планируется matching) | [cpp/venues](../../cpp/venues/) | `fob.execution.v1.ExecutionIntent` | `intent_id` |
| `execution.reports` | [cpp/venues](../../cpp/venues/) | [cpp/ledger](../../cpp/ledger/), [cpp/observability](../../cpp/observability/) | `fob.execution.v1.ExecutionReport` | `intent_id` |
| `risk.alerts` | [cpp/risk](../../cpp/risk/) | [cpp/observability](../../cpp/observability/) | `fob.risk.v1.RiskAlert` | `user_id` или `alert_id` |

## Consumer groups

| group_id | Сервис | Топики |
|---|---|---|
| `marketdata` | market_data | marketdata.raw |
| `matching` | matching | orders.normalized |
| `ledger-batch` | ledger | batch.outputs |
| `ledger-exec` | ledger | execution.reports |
| `venues` | venues | execution.intents |
| `observability` | observability | risk.alerts, batch.outputs, execution.reports |

## Принципы

1. **At-least-once.** `enable.auto.commit=false`, ручной `commitSync` после успешной обработки. Идемпотентность достигается через идемпотентные операции (`reservation_id` в ledger) или дедупликацию по `event_id`.
2. **Партиционирование.** Ключ выбирается так, чтобы все события одной логической сущности (одного ордера, одной пары) попадали в одну партицию — это даёт упорядоченность.
3. **`auto.offset.reset=earliest`** — при первом старте в dev читаем с начала. В проде нужно явно решать.
4. **Никаких бизнес-данных в protobuf, не находящихся в `contracts/proto/fob/**`**. Все payloadы — это proto, сериализованные через `cex::common::to_bytes`.

## Где это в коде

- Обёртки над librdkafka: [cpp/common/include/cex/common/kafka.hpp](../../cpp/common/include/cex/common/kafka.hpp), [cpp/common/src/kafka.cpp](../../cpp/common/src/kafka.cpp).
- Сериализация: [cpp/common/include/cex/common/proto.hpp](../../cpp/common/include/cex/common/proto.hpp).
