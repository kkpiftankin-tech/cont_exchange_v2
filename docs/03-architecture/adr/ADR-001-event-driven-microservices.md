---
id: ADR-001
status: planned
date: 2026-05-13
owners:
  - architecture
related:
  - docs/03-architecture/architecture-overview.md
  - docs/03-architecture/communication.md
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

Каждый сервис имеет одну зону ответственности (gateway, order-flow, matching, ledger, risk, market-data, venues, observability).

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

## Follow-up tasks

- TODO: написать ADR-003 о выборе Redpanda.
- TODO: написать ADR-004 о разделении PG OLTP / CH OLAP.
