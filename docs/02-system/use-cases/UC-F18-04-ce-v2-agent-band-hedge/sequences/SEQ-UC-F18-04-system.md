<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: SEQ-UC-F18-04-system level: kite
---
-->

# SEQ-UC-F18-04-system. Такт виртуальных контрагентов и хедж по полосе: system view (v2)

## Type

System Context Sequence

## Feature

- [F-18 (v2)](../../../features/F-18-ce-capital-position-hedge/)

## Use Case

- [UC-F18-04](../use-case.md)

## Purpose

Биржа CE как чёрный ящик прогоняет внутренний такт виртуальных контрагентов (A0–A8). Наружу видно только два факта: клиентская сделка меняет владение биржи НЕМЕДЛЕННО, а внешняя заявка на площадку уходит ТОЛЬКО когда позиция агента вышла за полосу ±q. Пока позиция внутри полосы — наружу не видно ничего (интернализация встречного потока).

## Participants

- Liquidity Trader (Client) — источник встречного потока.
- Continuous Exchange System — чёрный ящик (такт агентов opaque).
- External Venue (CEX: Binance/Kraken) — площадка исполнения.
- Operator — наблюдатель (позиции агентов, house, `Z_a` в UI).

## Diagram

```mermaid
sequenceDiagram
    actor C as Liquidity Trader (Client)
    participant S as Continuous Exchange System
    participant V as External Venue (CEX)
    actor O as Operator

    C->>S: Исполнить FlowOrder (клиентская сделка)
    Note over S: Владение биржи меняется НЕМЕДЛЕННО (точка входа №1)
    S->>S: Внутренний такт виртуальных контрагентов (A0-A8, opaque)
    S->>S: Позиция агента копится с нуля, знаковая (наружу ничего)
    alt позиция агента вышла за полосу ±q
        S->>V: Разместить заявку (излишек сверх полосы)
        V-->>S: Fill / partial fill / timeout
        Note over S: Владение меняется ТОЛЬКО исполнением (точка входа №2)
    else позиция внутри полосы
        Note over S: Наружу ничего; такт меняет только внутреннюю позицию агента
    end
    S-->>O: Обновление панели (позиции агентов, счёт дома, Z_a)
```

## Трассировка

| Артефакт | Ссылка |
| --- | --- |
| Feature | [F-18 (v2)](../../../features/F-18-ce-capital-position-hedge/) |
| Use Case | [UC-F18-04](../use-case.md) |
| Related Service Sequence (L1 🌊) | [SEQ-F18-UC-F18-04-services](../../../../05-components/sequences/SEQ-F18-UC-F18-04-services.md) |
| Related ADR | ADR-061 (planned, supersede ADR-054/057) |

## Source Fragments

- `incoming-docs/2026-09-15-CE_algorithm_v2.md` §A0, §A7 «Полоса», §«Позиция биржи по каждой валюте».
