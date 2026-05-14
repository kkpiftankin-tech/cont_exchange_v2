# Архитектура: обзор

Биржа FOB реализована как набор stateless-ish C++ микросервисов, обменивающихся:
- **синхронно через gRPC** для запрос-ответных операций (создание ордера, резерв средств);
- **асинхронно через Kafka/Redpanda** для потоковых данных (рыночные данные, события батча).

## Высокоуровневая схема

```
                ┌─────────────────────┐
   HTTP/JSON    │                     │
  ───────────▶ │    cpp/gateway      │
                │  (Crow, HTTP edge)  │
                └──────────┬──────────┘
                           │ gRPC
                           ▼
                ┌─────────────────────┐
                │   cpp/order_flow    │
                │ (валидация, оркестр)│
                └──┬───────┬───────┬──┘
            gRPC   │       │       │ Kafka publish
                   ▼       ▼       ▼  (orders.normalized)
            ┌──────────┐ ┌──────────────┐
            │ cpp/risk │ │  cpp/ledger  │       ┌────────────────────────┐
            │(pretrade)│ │(balances,    │       │      cpp/matching      │
            └──────────┘ │ reservations)│       │  (батч-клиринг, solver)│
                         └──────────────┘       └────────────┬───────────┘
                                                              │ Kafka publish
                                                              ▼ (batch.outputs)
                                          ┌───────────────────┴──────────┐
                                          │                              │
                                          ▼                              ▼
                                  cpp/ledger consumer            cpp/observability
                                  (применяет fills)             (агрегирует метрики)

   Внешний рынок ───▶ cpp/venues ──Kafka──▶ cpp/market_data ──gRPC──▶ потребители
                    (LOB → FOB)    (marketdata.raw)            (tickers)
```

## Сервисы

| Сервис | Каталог | Транспорт | Назначение |
|---|---|---|---|
| gateway | [cpp/gateway/](../../cpp/gateway/) | HTTP вход, gRPC выход | REST/JSON ↔ gRPC/proto, edge-точка |
| order_flow | [cpp/order_flow/](../../cpp/order_flow/) | gRPC сервер, Kafka producer | Валидация, риск-чек, резерв, публикация `orders.normalized` |
| risk | [cpp/risk/](../../cpp/risk/) | gRPC сервер, Kafka producer | Pre-trade checks, kill switch, `risk.alerts` |
| ledger | [cpp/ledger/](../../cpp/ledger/) | gRPC сервер, Kafka consumer | Балансы, резервы, применение fills из `batch.outputs` |
| matching | [cpp/matching/](../../cpp/matching/) | Kafka in/out | Solver батч-клиринга, `orders.normalized` → `batch.outputs` |
| market_data | [cpp/market_data/](../../cpp/market_data/) | gRPC сервер, Kafka consumer | Кэш последних тикеров из `marketdata.raw` |
| venues | [cpp/venues/](../../cpp/venues/) | Kafka in/out | Адаптеры к внешним площадкам (MVP-симулятор) |
| observability | [cpp/observability/](../../cpp/observability/) | Kafka consumer | Аггрегация и логирование событий |

## Топики Kafka

Создаются скриптом [infra/kafka/create_topics.sh](../../infra/kafka/create_topics.sh):

| Топик | Producer | Consumer | Retention |
|---|---|---|---|
| `marketdata.raw` | venues | market_data | 1 ч |
| `orders.normalized` | order_flow | matching | 7 дней |
| `batch.outputs` | matching | ledger, observability | 7 дней |
| `execution.intents` | (планируется matching) | venues | 1 день |
| `execution.reports` | venues | ledger, observability | 7 дней |
| `risk.alerts` | risk | observability | 30 дней |

## gRPC порты

| Сервис | Порт | env-переменная |
|---|---|---|
| gateway (HTTP) | 8080 | `GATEWAY_HTTP_PORT` |
| order_flow | 50051 | `ORDER_FLOW_GRPC_LISTEN` |
| risk | 50052 | `RISK_GRPC_LISTEN` |
| ledger | 50053 | `LEDGER_GRPC_LISTEN` |
| market_data | 50054 | `MARKET_DATA_GRPC_LISTEN` |

## Общая библиотека

[cpp/common/](../../cpp/common/) — линкуется во все сервисы:
- `Decimal` — арифметика фиксированной точки;
- `KafkaProducer`/`KafkaConsumer` — тонкие обёртки над librdkafka;
- `log_json` — структурированное логирование в stdout;
- `Env` — чтение env-переменных;
- `uuid_v4`, `now_ts` — утилиты для correlation_id и timestamp.

## Что НЕ реализовано (как было запланировано)

- Persistence в PostgreSQL/ClickHouse (state — in-memory).
- mTLS между сервисами (используется `InsecureChannelCredentials`).
- Настоящий многомерный солвер LP/QP (matching использует наивный per-order симулятор).
- Хедж-учёт ExecutionReport в ledger (только лог).
- Полноценный observability стек (только JSON в stdout).
