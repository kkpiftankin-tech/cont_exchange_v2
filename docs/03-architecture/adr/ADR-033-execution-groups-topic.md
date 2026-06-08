---
id: ADR-033
status: accepted
date: 2026-06-05
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/03-architecture/adr/ADR-003-kafka-redpanda-broker.md
  - docs/03-architecture/adr/ADR-020-event-ordering-idempotency.md
  - docs/03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md
  - docs/06-api/messaging/execution-groups.md
  - docs/06-api/messaging/topics.md
  - contracts/proto/fob/matching/v1/execution_group.proto
  - infra/kafka/create_topics.sh
  - incoming-docs/IN-011.meta.md
  - CLAUDE.md (§7.3 topics, §13 Kafka rules)
source: IN-011 (F-09 v2 corrected §11.4, §13.2, §13.3)
---

# ADR-033: Kafka-топик `execution.groups` для grouped execution (F-09)

## Контекст

F-04 публикует результат клиринга в `batch.outputs` (`BatchResult` с `fills[]`)
и (planned) `fills`. F-09 grouped execution даёт **новую сущность** —
`ExecutionGroup` с согласованным вектором исполнений и набором `LegFill`,
плюс commit rule «LegFill фиксируется не раньше или одновременно с
ExecutionGroup» (IN-011 §11.4, §20). Запихивать grouped-результат в
`batch.outputs` без схемы и ключа партиционирования нельзя (CLAUDE.md §13,
Conflict CN-IN011-03). Изменение схемы топика требует ADR (CLAUDE.md §3.3).

## Решение

Ввести отдельный топик **`execution.groups`**.

| Параметр | Значение |
| --- | --- |
| Producer | `matching` (Matching Backend) |
| Consumers | `ledger`, `risk` (post-trade), `order_flow` (parent/leg status), `observability`, UI stream, `backtest` (replay) |
| Key (partition) | `parentOrderId` (все группы/ноги одной родительской заявки — в один partition, упорядоченность) |
| Schema | `fob.matching.v1.ExecutionGroup` ([execution_group.proto](../../../contracts/proto/fob/matching/v1/execution_group.proto)) |
| Delivery | at-least-once + идемпотентный consumer по `executionGroupId` (ADR-020) |
| Retention | 7d (как `batch.outputs`) |

`LegFill` несёт `parentOrderId / executionGroupId / legId / groupPolicy /
liquiditySource` и публикуется в существующий `fills` (ключ `order_id`
расширяется `legId` для combo) — для downstream-аналитики; canonical grouped
envelope — `ExecutionGroup` в `execution.groups`.

### Commit / idempotency инварианты

- Нет полного допустимого grouped result для `strict_atomic` ⇒ нет `LegFill`
  (группа `cancelled_by_atomicity`/`waiting_next_batch`).
- Повторная доставка `ExecutionGroup` (тот же `executionGroupId`) не создаёт
  повторных балансовых операций в `ledger` и повторного исполнения переходов
  графа (`group_state_transitions`, [ADR-032](ADR-032-parent-child-order-model.md)).

### Регистрация

- `infra/kafka/create_topics.sh` — добавить `execution.groups` (key
  `parentOrderId`, retention 7d).
- `docs/06-api/messaging/topics.md` — добавить строку топика.
- `docs/06-api/messaging/execution-groups.md` — контракт.

## Альтернативы

- **Переиспользовать `batch.outputs`** — отклонено: иной ключ партиционирования
  (`batch_id` vs `parentOrderId`), иная сущность, ломает упорядоченность по
  parent и idempotency consumers ledger.
- **Только `fills` без grouped envelope** — отклонено: теряется атомарный commit
  «группа + ноги», статусы группы и solver diagnostics; ledger не сможет
  применять parent-level grouped summary.

## Последствия

- **Плюс:** чистая изоляция grouped-потока; естественный idempotency-ключ;
  replay-friendly (один envelope на группу, AC-F09-010).
- **Минус:** +1 топик и +1 consumer-путь в ledger/risk/observability; двойная
  публикация (grouped envelope + per-leg `fills`) требует согласованности.

## Обратимость

Низкая. Имя топика, ключ и схема — часть контракта; ретайр/переименование
требует нового ADR и миграции consumers (как `execution.venue` vs
`execution.reports`, CLAUDE.md §7.3).
