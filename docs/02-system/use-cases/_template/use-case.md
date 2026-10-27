---
id: UC-FXX-NN
title: "{Имя use case}"
level: sea
feature: F-XX
system-sequence: sequences/SEQ-UC-FXX-NN-system.md
service-sequence: ../../../05-components/sequences/SEQ-FXX-UC-FXX-NN-services.md
status: draft  # draft | active | superseded | deprecated
---

# UC-FXX-NN. {Имя use case}

> **Template.** При копировании замените `{slug}`, `XX`, `NN` на конкретные
> значения, заполни реальный frontmatter (не HTML-комментарием — он должен
> быть настоящим YAML, читаемым парсером; см. миграцию 2026-10-27,
> [ADR-086](../../../03-architecture/adr/ADR-086-two-axis-reply-routing.md)
> Этап 1). Пути с плейсхолдерами ниже в тексте — шаблонные, не клики.
>
> **IN-013**: `level: sea` 🌊. L0 system sequence — `sequences/SEQ-UC-FXX-NN-system.md`; L1 service sequence — `../../../05-components/sequences/SEQ-FXX-UC-FXX-NN-services.md`. L2 component-internals — в `docs/05-components/{component}/sequences/`.

## Feature

- `../../features/F-XX-{slug}/`

## Primary Actor

{Trader / Market Maker / Provider / Operator}

## Supporting Actors

- {другие участники}

## Preconditions

- {что должно быть истинно до запуска}

## Trigger

{какое действие или событие запускает сценарий}

## Main Flow

1. {Шаг 1}
2. {Шаг 2}
3. ...

## Alternative Flows

### A1. {Имя альтернативного потока}

1. {шаг}

## Postconditions

- {какие изменения зафиксированы в системе}

## Related Sequence Diagrams

- System sequence: `sequences/SEQ-UC-FXX-NN-system.md`
- Service sequence: `../../../05-components/sequences/SEQ-FXX-UC-FXX-NN-services.md`

## Related Contracts

- {ссылки на docs/06-api/...}

## Related Components

- {ссылки на docs/05-components/.../overview.md}

## Related Data

- {ссылки на docs/07-data/...}
