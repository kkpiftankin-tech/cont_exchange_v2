---
id: ADR-001
status: accepted
date: 2026-05-13
owners:
  - architecture
related:
  - docs/03-architecture/architecture-overview.md
  - docs/03-architecture/communication.md
  - docs/03-architecture/adr/ADR-003-kafka-redpanda-broker.md
  - docs/03-architecture/adr/ADR-019-c4-documentation-standard.md
  - docs/03-architecture/adr/ADR-020-event-ordering-idempotency.md
---

# ADR-001: Event-driven microservices

## Контекст

Биржа должна обрабатывать поток событий (orders, market data, fills, risk alerts)
с разной частотой и SLA. Монолит плохо масштабируется по solver, market data
имеет иной throughput, чем gateway.

## Решение

Архитектура — набор C++ микросервисов, обменивающихся:

- **синхронно через gRPC** для запрос-ответных операций;
- **асинхронно через Kafka / Redpanda** для потоковых событий.

Каждый сервис имеет одну зону ответственности (gateway, order-flow, matching, ledger, risk, market-data, venues, venue-health, observability).

### Модель коммуникации

- **gRPC** — команды и запрос-ответ: создание/отмена FlowOrder, pre-trade и pre-hedge risk-чеки, ledger-запросы, market-data read API.
- **Kafka / Redpanda** — потоковые события: `orders.normalized`, `batch.outputs`, `fills`, `marketdata.raw`, `venue.liquidity.fob`, `venue.health`, `execution.intents`, `execution.venue`, `risk.alerts` (полный реестр — `docs/06-api/messaging/topics.md`).

### Модель согласованности

- Kafka delivery — **at-least-once**; все consumers идемпотентны (ключи идемпотентности — см. [ADR-020](ADR-020-event-ordering-idempotency.md)).
- Упорядочивание — per-key внутри партиции; глобальный exactly-once **не** гарантируется.
- Между matching / ledger / risk / market-data — **eventual consistency**; распределённых транзакций нет.

Уровни C4 для этой архитектуры формализованы в [ADR-019](ADR-019-c4-documentation-standard.md); выбор брокера — в [ADR-003](ADR-003-kafka-redpanda-broker.md).

## Альтернативы

- **Монолит C++**. Отклонено: трудно масштабировать matching и market-data независимо.
- **Service mesh с REST.** Отклонено: REST даёт меньшую type-safety и больший overhead, чем proto.

## Последствия

### Положительные

- Независимое масштабирование matching, market-data, gateway.
- Чёткие границы ответственности.
- Event log как audit trail для replay.

### Отрицательные

- Распределённая отладка сложнее.
- Дополнительная сложность с at-least-once семантикой Kafka.

## Обратимость

Низкая. Переход обратно в монолит потребует переписывания.

## Связанные ADR

- [ADR-003](ADR-003-kafka-redpanda-broker.md) — выбор Kafka-совместимого брокера.
- [ADR-004](ADR-004-postgres-clickhouse-data-platform.md) — разделение PostgreSQL OLTP / ClickHouse OLAP.
- [ADR-019](ADR-019-c4-documentation-standard.md) — C4-стандарт документации границ.
- [ADR-020](ADR-020-event-ordering-idempotency.md) — event ordering / idempotency / delivery semantics.
