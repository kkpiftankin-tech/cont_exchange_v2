---
id: DOC-DOMAIN-OVERVIEW
phase: 04-domain
status: draft
owner: core-team
related:
  - docs/04-domain/ubiquitous-language.md
  - docs/04-domain/entities.md
  - contracts/proto/fob/
---

# Domain Overview

## Контекст

Предметная область — **continuous flow trading** в духе Kyle-Lee. Заявка трейдера —
не точка (цена, объём), а поток `v_t` во времени, ограниченный диапазоном цен и
максимальным объёмом.

## Ключевые сущности

См. [entities.md](entities.md). Кратко:

- **FlowOrder** — потоковая заявка.
- **Batch** — единичный цикл клиринга.
- **BatchResult** — результат батча: clearPrices, executedRates, fills, diagnostics.
- **FillEvent** — исполнение конкретной заявки.
- **Position** — текущая позиция клиента.
- **Reservation** — резерв средств под открытый ордер.

## Ключевые события

См. [events/](events/). Все события доменного уровня соответствуют Kafka-сообщениям.

## Принципы

1. **Decimal-арифметика** для всех денежных и количественных величин (см. ADR-005).
2. **Источники истины** — proto-схемы в `contracts/proto/`.
3. **Domain events** идут через Kafka и реплеируемы.
4. **Инварианты** проверяются в коде явно (см. specs/domain/invariants.yaml).
