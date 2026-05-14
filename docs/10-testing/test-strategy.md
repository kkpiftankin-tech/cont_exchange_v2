---
id: DOC-TEST-STRATEGY
phase: 10-testing
status: draft
owner: core-team
related:
  - tests/
  - docs/02-system/features/
---

# Test Strategy

## Пирамида тестов

```
        ▲
        │     E2E / scenario
        │     (mvp_e2e, system smoke)
        │
        │     Integration
        │     (Kafka↔modules, gRPC, DB)
        │
        │     Unit
        │     (matching solver, ledger, risk checks, decimal)
        ▼
```

## Виды тестов

| Уровень | Что покрываем | Инструменты | Где лежат |
| --- | --- | --- | --- |
| Unit | домены, value objects, solver, decimal-арифметика | GoogleTest, Catch2 | `tests/unit/`, `cpp/<module>/tests/` |
| Integration | gRPC контракты, Kafka-топики, DB-доступ | docker-compose стенд | `tests/integration/` |
| Contract | proto-совместимость | `tools/proto-contract-auditor/` | CI |
| Traceability | соответствие Goal → Test | `tools/traceability-checker/` | CI |
| Backtest / Replay | numeric-стабильность solver | `legacy_mvp/`, fixtures | `tests/backtest/` (планируется) |
| Smoke / E2E | пользовательский сценарий целиком | `mvp_e2e`, `make run-mvp` | `tests/e2e/` |

## Принципы

1. **Детерминированность.** Никаких случайных таймеров, фиксированный seed.
2. **Decimal-equality** — никаких сравнений `double` через `==`.
3. **Idempotency-проверки** на всех at-least-once путях.
4. **Acceptance-критерии** — для каждой F-XX фичи (см. [../02-system/features/](../02-system/features/)).

## Acceptance-критерии

Каждая фича в [../02-system/features/](../02-system/features/) содержит блок
`acceptance:` в YAML — это и есть источник истины для приёмки.

## Связанные документы

- [../02-system/features/](../02-system/features/)
- [../../tests/](../../tests/)
- [../11-operations/runbook.md](../11-operations/runbook.md)
