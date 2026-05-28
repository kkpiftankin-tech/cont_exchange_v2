---
id: ADR-020
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-003-kafka-redpanda-broker.md
  - docs/03-architecture/adr/ADR-026-ledger-accounting-pnl.md
  - docs/06-api/messaging/topics.md
  - CLAUDE.md (§13 правила Kafka)
---

# ADR-020: Event ordering, idempotency и delivery semantics

## Контекст

[ADR-001](ADR-001-event-driven-microservices.md) и
[ADR-003](ADR-003-kafka-redpanda-broker.md) фиксируют at-least-once Kafka, но
конкретные правила упорядочивания, идемпотентности и delivery-семантики
разбросаны по CLAUDE.md §13 и коду consumers. Для финансовых потоков
(fills, execution reports, ledger mutations) это критично и должно быть
зафиксировано как единое архитектурное правило.

## Решение

- **Delivery — at-least-once.** Глобальный exactly-once **не** гарантируется.
- **Все consumers идемпотентны** по стабильному ключу:

  | Поток | Idempotency key |
  | --- | --- |
  | FlowOrder commands | `order_id` (+ `client_order_id` от клиента) |
  | Batch clearing | `batch_id` |
  | Fill events | `fill_id` |
  | Execution reports | `report_id` |
  | Child orders | `client_order_id` |

- **Ordering** — per-partition по ключу; partition key осмыслен per-topic
  (`user_id` / `symbol` / `order_id` / `batch_id` / `intent_id`).
- **Offset** коммитится только после успешной обработки.
- **Replay-safety**: повторная обработка того же события не меняет состояние
  (idempotent apply), что обеспечивает audit/backtest replay.
- **DLQ / retry**: ядовитые сообщения уходят в retry/DLQ-топик, не блокируя
  партицию бесконечно.

## Альтернативы

- **Exactly-once (Kafka transactions)** — отклонено как глобальная гарантия: сложно, дорого, кросс-сервисно ненадёжно; идемпотентность проще и достаточна.
- **Полагаться на порядок без идемпотентности** — отклонено: at-least-once даёт дубликаты, без idempotency это double-apply в ledger.

## Последствия

- **Плюс:** безопасный replay, устойчивость к дубликатам, аудируемость.
- **Минус:** каждый финансовый consumer обязан хранить seen-keys / делать idempotent upsert (стоимость состояния и проверок).

## Обратимость

Низкая. Идемпотентность и ключи зашиты в consumer-логику всех финансовых сервисов; смена семантики — широкое изменение.
