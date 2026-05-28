---
id: ADR-012
status: accepted
date: 2026-05-20
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-12-execution-hedge/
  - docs/05-components/venue-execution-adapter/overview.md
  - docs/05-components/external-venues/overview.md
  - docs/03-architecture/adr/ADR-014-venues-binary-vs-components.md
  - incoming-docs/2026-05-20-F-12-execution-hedge-v1.md
---

# ADR-012: Venue Execution Adapter — отдельный сервис vs слой внутри `cpp/venues/`

## Контекст

Спецификация F-12 (IN-005) выделяет **Venue Execution Adapter** как самостоятельный компонент, ответственный за:

- consume `execution.intents` из Kafka;
- creating HedgeFlow + child_orders в PostgreSQL;
- делегирование исполнения External Venues Connector (EVC);
- нормализацию raw execution events в `ExecutionReport`;
- publish ExecutionReport в Kafka `execution.venue`;
- timeout watchdog, overfill guard, reconciliation, idempotency.

В текущем коде эти обязанности физически живут внутри `cpp/venues/src/`:

- [`cpp/venues/src/app/execute_on_venue.cpp`](../../cpp/venues/src/app/execute_on_venue.cpp);
- [`cpp/venues/src/infra/execution_intents_consumer.cpp`](../../cpp/venues/src/infra/execution_intents_consumer.cpp);
- [`cpp/venues/src/infra/execution_report_producer.cpp`](../../cpp/venues/src/infra/execution_report_producer.cpp);
- venue adapters (CexWsRest, DexAmmRpc, VenueSim) — это и есть EVC.

Таким образом, `cpp/venues/` совмещает две роли:
1. **External Venues Connector** (EVC) — низкоуровневая интеграция с CEX/DEX/AMM.
2. **Venue Execution Adapter** — высокоуровневый state machine хеджа.

Вопрос: разделять ли эти роли в отдельные сервисы / бинарники, или оставить в общем `cpp/venues/`?

## Решение

**Принять, что Venue Execution Adapter — отдельный логический компонент**, но **в первой итерации (F-12 MVP) реализовать его внутри `cpp/venues/` как отдельный модуль `cpp/venues/src/app/`**, без выделения в отдельный бинарник.

Условия для будущего split-а в отдельный сервис (revisit ADR):

1. Throughput хеджей превышает throughput market-data ingestion в `venues`, и они начинают конкурировать за ресурсы.
2. Хочется деплоить Adapter с отличной от EVC scaling-стратегией.
3. EVC переходит в другую кодовую базу (например, отдельный bridge-сервис для DEX/AMM).

> **Scope vs ADR-014.** Этот ADR решает только **логическую/модульную** границу
> Venue Execution Adapter. Решение о runtime-упаковке (единый бинарь `cpp/venues`
> vs отдельный сервис) подчинено [ADR-014](ADR-014-venues-binary-vs-components.md),
> который фиксирует, что Adapter физически живёт в процессе `cpp/venues`.

## Альтернативы

### 1. Сразу выделить в `cpp/venue_execution_adapter/` сервис

**Pros:** чёткие границы; независимый deploy; легче тестировать isolated; легче скейлить.
**Cons:** дублирование Kafka producer/consumer setup; дополнительный network hop EVC ↔ Adapter (если EVC остаётся в `cpp/venues/`); ещё один бинарник в Docker compose.

Отклонено: MVP не нуждается в отдельной шкале, а сложность инфра увеличится.

### 2. Полностью внутри `cpp/venues/` без разделения модулей

**Pros:** минимум кода; быстрее в коде.
**Cons:** Adapter logic смешивается с EVC, тестирование затрудняется, нарушается single responsibility внутри сервиса.

Отклонено.

### 3. (Выбрано) Модульное разделение внутри `cpp/venues/` (app/ vs infra/)

`cpp/venues/src/app/` хостит Adapter use cases (HedgeFlow state machine, reconciliation, overfill guard, timeout watchdog). `cpp/venues/src/infra/` хостит EVC implementations (REST/WS/RPC adapters) + Kafka I/O.

В docs логически разделяем как два компонента (`venue-execution-adapter`, `external-venues`), физически — один сервис.

## Последствия

### Положительные

- Чёткая логическая декомпозиция в docs (соответствие IN-005).
- Возможность future split без переписки бизнес-логики.
- Common Kafka clients и shared utilities.
- Один deployment-юнит, проще в dev.

### Отрицательные

- Failure isolation слабая: ошибка в EVC может уронить Adapter.
- Скейлинг общий: Adapter и EVC скалируются вместе.
- Нарушение CLAUDE.md §10.3 «venues не принимает бизнес-решения о хеджировании, а исполняет ExecutionIntent» — Adapter принимает бизнес-решения (overfill, reconciliation, retry). Acceptable trade-off для MVP с явной модульной границей.

### Обратимость

Высокая. Внутренние модули `app/` и `infra/` уже разделены; для выноса в отдельный сервис достаточно создать `cpp/venue_execution_adapter/` с теми же файлами + дополнительный Dockerfile.

## Open Questions

См. [F-12 open-questions §1](../../02-system/features/F-12-execution-hedge/open-questions.md).

## Status

Accepted (2026-05-28). Решение реализовано в F-12 MVP: Venue Execution Adapter — модуль `cpp/venues/src/app/`, без отдельного бинарника. Runtime-упаковка зафиксирована в [ADR-014](ADR-014-venues-binary-vs-components.md).
