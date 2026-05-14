---
id: DOC-API-TOPICS
phase: 06-api
status: draft
owner: core-team
related:
  - infra/kafka/create_topics.sh
  - docs/03-architecture/communication.md
---

# Kafka Topics

Источник истины — скрипт [../../../infra/kafka/create_topics.sh](../../../infra/kafka/create_topics.sh).
Схемы — proto-файлы в `contracts/proto/`.

| Topic | Producer | Consumer(s) | Message type | Retention | Partition key |
| --- | --- | --- | --- | --- | --- |
| `marketdata.raw` | venues | market-data | `fob.marketdata.v1.MarketDataRaw` | 1 h | venue\|symbol |
| `orders.normalized` | order-flow | matching | `fob.orders.v1.OrdersNormalized` | 7 d | symbol |
| `batch.outputs` | matching | ledger, observability | `fob.matching.v1.BatchResult` | 7 d | batch_id |
| `execution.intents` | matching (planned) | venues | `fob.execution.v1.ExecutionIntent` | 1 d | intent_id |
| `execution.reports` | venues | ledger, observability | `fob.execution.v1.ExecutionReport` | 7 d | intent_id |
| `risk.alerts` | risk | observability | `fob.risk.v1.RiskAlert` | 30 d | user_id или alert_id |

## Принципы

- **At-least-once.** `enable.auto.commit=false`, ручной `commitSync`.
- **Idempotency.** Через `event_id` или специальный ключ.
- **Partition stability.** Один логический ключ всегда в одну партицию.

## Per-topic документы

- [orders-normalized.md](orders-normalized.md)
- [batch-outputs.md](batch-outputs.md)
- [marketdata-raw.md](marketdata-raw.md)
- [execution-intents.md](execution-intents.md)
- [execution-reports.md](execution-reports.md)
- [risk-alerts.md](risk-alerts.md)
- (planned) `agent-logs.md`
- (planned, см. Conflict Note C-1 ниже) `fills.md`

## Conflict Notes

### C-1. Раздельные `batch.outputs` и `fills` (IN-003)

**Источник:** IN-003 §4.4.3 предполагает два отдельных Kafka топика:

- `batch.outputs` — только `BatchResult`;
- `fills` — только `FillEvent`.

**Текущее состояние (MVP):** один топик `batch.outputs` несёт оба типа сообщений (BatchResult с встроенными fills).

**Резолюция:** оставить single-topic как MVP-схему. Разделение — отдельный future task [T-F04-008](../../implementation-plan/F-04-batch-clearing.tasks.md#t-f04-008-conflict-resolution-split-batchoutputs-→-batchoutputs--fills-future) (post-MVP):

- партиционирование `fills` по `order_id` для idempotency;
- партиционирование `batch.outputs` по `batch_id`;
- миграция consumer'ов Ledger / Risk / Observability.

После выполнения T-F04-008 — обновить эту таблицу, удалить conflict note, добавить per-topic документ `fills.md`.
