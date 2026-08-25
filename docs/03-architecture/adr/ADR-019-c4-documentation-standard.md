---
id: ADR-019
status: accepted
date: 2026-05-28
owners:
  - architecture
related:
  - docs/03-architecture/adr/ADR-001-event-driven-microservices.md
  - docs/03-architecture/context-diagram.md
  - docs/03-architecture/container-diagram.md
  - docs/03-architecture/component-map.md
---

# ADR-019: C4 documentation standard

## Контекст

[ADR-001](ADR-001-event-driven-microservices.md) фиксирует микросервисную
event-driven архитектуру, но не задаёт стандарт описания границ. В
`docs/03-architecture/` уже есть `context-diagram.md`, `container-diagram.md`,
`component-map.md` — нужно зафиксировать уровни C4 как нормативные, чтобы
ссылки из sequence-диаграмм и traceability ([ADR-018](ADR-018-design-traceability-chain.md))
указывали на стабильные C4-компоненты.

## Решение

Принять модель **C4** как стандарт архитектурной документации:

- **C1 — System Context**: Continuous Exchange как система + внешние акторы (`context-diagram.md`).
- **C2 — Containers**: gateway, order-flow, matching, ledger, risk, market-data, venues, venue-health, observability, kafka/redpanda, postgres, clickhouse (`container-diagram.md`).
- **C3 — Components**: внутренние компоненты сервисов; для `cpp/venues` — connector, normalizer, curve-builder, execution-adapter; health-routing — в `cpp/venue_health` (`component-map.md`, см. [ADR-014](ADR-014-venues-binary-vs-components.md)).
- **C4 — Code**: диаграммы уровня кода только для критических модулей (solver, LOB→FOB, ledger), по необходимости.

Service-level и system-level sequence-диаграммы (CLAUDE.md §26a) обязаны
ссылаться на C4-компоненты соответствующего уровня.

## Альтернативы

- **arc42 / 4+1 views** — отклонено: C4 проще и уже частично применён в репозитории.
- **Свободные диаграммы без стандарта** — отклонено: ломает прослеживаемость sequence→component.

## Последствия

- **Плюс:** стабильные якоря для ссылок; единый язык границ.
- **Минус:** диаграммы нужно поддерживать при изменении контейнеров/компонентов (drift-policy [ADR-008](ADR-008-code-doc-drift-policy.md)).

## Обратимость

Высокая. Стандарт документации, не код; смена нотации не затрагивает рантайм.
