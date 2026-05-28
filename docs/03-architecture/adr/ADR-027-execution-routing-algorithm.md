---
id: ADR-027
status: accepted
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/03-architecture/adr/ADR-013-execution-planning-placement.md
  - docs/03-architecture/adr/ADR-024-latency-aware-venue-curve.md
  - docs/04-domain/business-rules.md
  - cpp/matching/src/app/execution_planner.cpp
---

# ADR-027: Execution routing algorithm strategy

## Контекст

[ADR-013](ADR-013-execution-planning-placement.md) явно выносит выбор
routing-алгоритма в отдельный ADR. Execution Planning распределяет targetQty
по venue на основе `venue.liquidity.fob` + `venue.health`. В коде уже
реализовано **пропорциональное** распределение (`execution_planner.cpp`,
`BuildMultiVenuePlan` — формула в `business-rules.md#routing-plan`). Нужно
зафиксировать стратегию и roadmap, не закрывая дверь для cost-минимизации/RL.

## Решение

Фазовая стратегия выбора алгоритма:

| Фаза | Алгоритм |
| --- | --- |
| **MVP (текущая)** | Proportional allocation по доступной ликвидности: `qty[v] = L(v) / ΣL × targetQty`. Детерминирован, объясним. |
| **v1** | Cost minimization: минимизация `expected_slippage + fees + venue_health_penalty + latency_penalty` (latency — из [ADR-024](ADR-024-latency-aware-venue-curve.md)). |
| **future** | ILP / convex optimization / RL-based (policy из `agent.logs`). |

### Acceptance-критерии для любого алгоритма

- **Детерминированный** выход для replay (тот же вход → тот же RoutingPlan).
- **Explainability** обязательна (почему именно такое распределение).
- **Нет black-box** routing в проде без replay-валидации.

## Альтернативы

- **Сразу ILP/RL** — отклонено для MVP: сложно, недетерминированный риск, нет данных для калибровки.
- **Always best-venue (greedy на одну площадку)** — отклонено: концентрирует риск/impact на одной venue.

## Последствия

- **Плюс:** простой объяснимый MVP; путь к cost-минимизации без переписывания границ (алгоритм заменяем через DI, [ADR-013](ADR-013-execution-planning-placement.md)).
- **Минус:** proportional не оптимизирует стоимость; до v1 возможен субоптимальный хедж.

## Обратимость

Высокая по интерфейсу (algorithm заменяем через DI). Низкая после деплоя нового алгоритма без replay parity — требует валидации против исторических данных.
