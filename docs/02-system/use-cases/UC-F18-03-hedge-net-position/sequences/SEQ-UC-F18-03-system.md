<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F18-03-system
level: kite
---
-->

# SEQ-UC-F18-03-system. Хедж чистой позиции: system view

## Type

System Context Sequence

## Feature

- [F-18](../../../features/F-18-ce-capital-position-hedge/)

## Use Case

- [UC-F18-03](../use-case.md)

## Purpose

Хедж эмиттится по чистой позиции актива (flatten к нулю), а не по каждому
потоку сегмента. Внешней площадке виден один агрегированный child order на актив
вместо нескольких встречных.

## Participants

- Continuous Exchange System
- External Venue (CEX / DEX / AMM)
- Operator (наблюдатель — видит хедж-флоу и PnL в UI)

## Diagram

```mermaid
sequenceDiagram
    participant S as Continuous Exchange System
    participant V as External Venue (CEX/DEX/AMM)
    actor O as Operator

    Note over S: Позиция обновлена (UC-F18-02)
    S->>S: Для каждого актива: |pos_a| > θ_a ?
    Note over S: Да — flatten (pos>0 SELL, pos<0 BUY)
    S->>S: Pre-hedge risk check
    S->>V: Place ОДИН net-hedge child order на актив
    V-->>S: Fill / partial / reject
    S->>S: pos_a -= filled; realized PnL/fees → capital
    S-->>O: HedgeFlow + CE Capital/Position update (UI)
```

## Related Service Sequence

- [SEQ-F18-UC-F18-02-services](../../../../05-components/sequences/SEQ-F18-UC-F18-02-services.md)

## Source Fragments

- ADR-054 §4, §5
