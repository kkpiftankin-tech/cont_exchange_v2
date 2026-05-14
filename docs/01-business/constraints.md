---
id: DOC-BUSINESS-CONSTRAINTS
phase: 01-business
status: draft
owner: core-team
---

# Constraints

## Регуляторные

- KYC/AML обязательны для client/provider ролей; demo — может работать без верификации.
- Юрисдикционные ограничения (зависит от target market).
- Аудируемые журналы сделок и решений — `risk_events`, `fills`, `batch_results` в ClickHouse.

## Операционные

- median solveTimeMs ≤ 500 ms, p95 ≤ 1000 ms.
- Восстановимость после сбоев без потери истории сделок.
- Возможность replay для аудита.

## Технические

- Decimal-арифметика для всех денежных величин (см. ADR-005).
- Event-driven архитектура (см. ADR-001).
- Protobuf + gRPC как source of truth (см. ADR-002).
