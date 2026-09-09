<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F18-02-system
level: kite
---
-->

# SEQ-UC-F18-02-system. Вывод чистой позиции CE: system view

## Type

System Context Sequence

## Feature

- [F-18](../../../features/F-18-ce-capital-position-hedge/)

## Use Case

- [UC-F18-02](../use-case.md)

## Purpose

Чистая позиция CE выводится из результата клиринга (вектор x) как проекция на
активы. Полностью внутренний сценарий; внешним участникам ничего не видно —
только Operator наблюдает позицию в UI.

## Participants

- Continuous Exchange System
- Operator (наблюдатель — видит net-позицию в UI)

## Diagram

```mermaid
sequenceDiagram
    participant S as Continuous Exchange System
    actor O as Operator

    Note over S: Vector clearing завершён (F-05A), converged
    S->>S: Проекция x на активы: ΔposBASE=+x, ΔposQUOTE=-x·P_eff
    S->>S: Неттинг встречных ног по каждому активу
    S->>S: pos_a += Δpos_a (идемпотентно по batch_id)
    S->>S: Append Δposition в аудит-историю
    S-->>O: CE Net Position snapshot (UI)
```

## Related Service Sequence

- [SEQ-F18-UC-F18-02-services](../../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md)

## Source Fragments

- ADR-054 §2, §3
