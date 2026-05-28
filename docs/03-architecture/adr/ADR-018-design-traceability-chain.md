---
id: ADR-018
status: accepted
date: 2026-05-28
owners:
  - architecture
related:
  - docs/03-architecture/adr/ADR-006-docs-as-code-repository-structure.md
  - docs/03-architecture/adr/ADR-007-feature-traceability-gate.md
  - docs/traceability/feature-traceability.md
  - CLAUDE.md (§0a docs-as-code workflow)
---

# ADR-018: End-to-end design traceability chain

## Контекст

[ADR-006](ADR-006-docs-as-code-repository-structure.md) фиксирует docs-as-code
структуру, [ADR-007](ADR-007-feature-traceability-gate.md) — traceability gate
на уровне feature → code. Но сама **сквозная проектная цепочка** от
бизнес-требования до кода не зафиксирована как нормативная, хотя CLAUDE.md §0a
её уже описывает и проект ей следует.

## Решение

Зафиксировать обязательную сквозную цепочку прослеживаемости:

```text
Business Requirement → Feature → Use Case → System Sequence →
Service Sequence → Contracts → Data Objects → Components → Tests → Code
```

Каждая фича в `docs/02-system/features/F-XX-*/` обязана линковаться на:
requirements, use cases, system-level и service-level sequences, components,
contracts (`docs/06-api/`), data objects (`docs/07-data/`), tests/acceptance.
Machine-readable источник связей — `specs/domain/*.yaml`, markdown
traceability в `docs/traceability/` держится с ним в синхроне.

> **Примечание о структуре.** Замечание-источник предлагало альтернативную
> раскладку `docs/05_features/F-XX/{brd,user-stories,…}.md`. Репозиторий
> использует утверждённую 11-папочную раскладку (см. CLAUDE.md Conflict Note);
> цепочка прослеживаемости накладывается на неё **без** переименования папок.
> Изменение раскладки потребовало бы отдельного ADR.

## CI-инвариант

```text
BR  → Feature        (каждое BR покрыто фичей)
Feature → Use Case
Use Case → System Sequence
System Sequence → Service Sequence
Service Sequence → Contract (REST/gRPC/Kafka/SQL)
Contract → Code path
Domain entity → Proto
Acceptance criteria → Test (или waiver)
```

Нарушение цепочки — fail traceability gate ([ADR-007](ADR-007-feature-traceability-gate.md)).

## Альтернативы

- **Не формализовать цепочку** (полагаться только на ADR-006/007) — отклонено: gate проверяет feature↔code, но не полную цепочку BR→…→Test.
- **Отдельная раскладка `docs/05_features/`** — отклонено: конфликтует с принятой 11-папочной структурой.

## Последствия

- **Плюс:** любое изменение кода прослеживаемо до бизнес-требования и теста.
- **Минус:** дисциплина создания всех звеньев до кода (no-code-before-docs).

## Обратимость

Высокая на уровне CI-правил; низкая по духу (отказ от прослеживаемости противоречит docs-as-code основе проекта).
