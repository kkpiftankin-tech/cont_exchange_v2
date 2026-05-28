---
id: ADR-003
status: accepted
date: 2026-05-13
owners:
  - architecture
related:
  - docs/03-architecture/adr/ADR-001-event-driven-microservices.md
  - docs/03-architecture/adr/ADR-020-event-ordering-idempotency.md
  - docs/06-api/messaging/topics.md
  - infra/kafka/create_topics.sh
  - infra/docker-compose.dev.yml
---

# ADR-003: Kafka-совместимый брокер (Redpanda / Kafka)

## Контекст

Архитектура event-driven ([ADR-001](ADR-001-event-driven-microservices.md))
требует durable event streams. У сервисов разные SLA: market-data — высокий
throughput и короткий retention, audit / risk / fills — длинный retention.
Replay и backtest (F-15) требуют переигрывания event log. Нужен брокер с
партиционированием, consumer groups, конфигурируемым retention и replay.

В dev-стеке (`infra/docker-compose.dev.yml`) уже используется Redpanda;
топики создаются скриптом `infra/kafka/create_topics.sh`. Решение по факту
принято и реализовано — этот ADR его фиксирует.

## Решение

Использовать **Kafka-совместимый брокер**. Для dev / MVP — Redpanda (легче,
без ZooKeeper, single-binary). В коде и контрактах обращаться к нему как к
«Kafka-compatible», а **не** жёстко завязываться на Redpanda-специфику, чтобы
prod мог использовать Apache Kafka / MSK / Redpanda Cloud без изменения
кода сервисов.

Ключевые правила потока (детально — [ADR-020](ADR-020-event-ordering-idempotency.md)):

- partition key осмыслен per-topic (`user_id`, `symbol`, `order_id`,
  `batch_id`, `intent_id`);
- delivery — at-least-once + идемпотентные consumers;
- offset коммитится после успешной обработки;
- exactly-once не обещаем;
- retention соответствует назначению топика (market-data short, audit/fills long);
- replay возможен для audit / backtest.

## Альтернативы

- **RabbitMQ** — отклонено: слабее для replay/log-семантики и партиционированного re-read.
- **NATS JetStream** — отклонено: меньшая экосистема инструментов и зрелость для финансового audit log.
- **Redis Streams** — отклонено: durability/retention слабее, не подходит для long-retention audit.
- **Прямой gRPC fan-out** — отклонено: нет durable log, нет replay, теряется при рестарте.
- **PostgreSQL outbox only** — отклонено: не масштабируется на market-data throughput; нет нативных consumer groups.

## Последствия

### Положительные

- Durable, партиционированный event log с replay для audit/backtest.
- Consumer groups для независимого масштабирования читателей.
- Redpanda упрощает dev (один контейнер, Kafka API).

### Отрицательные

- At-least-once требует идемпотентных consumers во всех финансовых путях.
- Операционная сложность брокера (partitions, retention, consumer lag).

## Обратимость

Средняя. Пока код использует только Kafka API (а не Redpanda-специфику),
смена брокера на Apache Kafka обратима без изменения сервисов. Смена на
не-Kafka брокер (RabbitMQ/NATS) — низкая обратимость, потребует переписать
producer/consumer слой.
