---
id: ADR-034
status: accepted
date: 2026-06-10
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-09-batch-combo-orders/feature.yaml
  - docs/03-architecture/adr/ADR-031-multileg-execution-modes-atomicity.md
  - docs/03-architecture/adr/ADR-005-fixed-point-decimal-money.md
  - docs/03-architecture/adr/ADR-009-shadow-mode-isolation-strategy.md
  - cpp/matching/src/domain/grouped_solver.hpp
  - cpp/matching/src/domain/grouped_solver_bisection.cpp
  - cpp/matching/src/domain/constraint_evaluator.cpp
  - docs/implementation-plan/F-09-batch-combo-orders.tasks.md
source: IN-011 (F-09 v2) §2.3; F-09 v2 system-impact analysis §5 (OQ-1)
---

# ADR-034: Grouped constraint solver для combo (feasibility-gate сейчас, QP позже)

## Контекст

MVP-2 реализовал grouped solver для ratio/basket: масштаб группы
`G = min_ℓ(Q_ℓ / ρ_ℓ)`, вектор `e = G·ρ` (closed-form, файл
`grouped_solver_bisection.cpp`), за интерфейсом `IGroupedSolver`. MVP-3 добавил
**оценку ограничений** (`constraint_evaluator.cpp`): `spread_range`,
`factor_neutrality`, `max_total_notional` проверяются по уже посчитанному `e` —
hard-нарушение блокирует группу, soft → degrade.

Полная постановка F-09 (IN-011 §2.3) — это **оптимизация с ограничениями**:

```text
max α   s.t.   A_g e = b_g α   (ratio/factor равенства)
               G_g e ≤ h_g     (spread/notional неравенства)
               0 ≤ e_i ≤ Q_i   (feasible caps)
```

Это QP/LP-задача. Решать её — значит при нарушении ограничения **не блокировать**
группу, а найти ближайший допустимый вектор `e` (исполнить максимально в пределах
ограничений). Выбор солвера/новой библиотеки требует ADR (CLAUDE.md §3.3); QP над
деньгами затрагивает детерминизм replay (AC-F09-010) и границу double↔Decimal (§9).

## Решение

1. **MVP-3 поставляется в форме feasibility-gate** (реализовано): solver считает
   `e` по ratio, `EvaluateConstraints` проверяет, hard → block, soft → degrade.
   Этого достаточно для корректного поведения (группа не исполняется с нарушением).
   Factor-нейтральность для ratio-locked групп достигается **конструктивно**: если
   `A_g ρ = 0`, то `A_g e = α·A_g ρ = 0` автоматически.

2. **Полный QP re-solve откладывается** до реального спроса (spread/factor combo в
   проде). Когда он будет реализован — по следующим правилам:
   - **Алгоритм за `IGroupedSolver`.** Новая реализация (`GroupedSolverQp`) —
     ещё одна стратегия за тем же интерфейсом; bisection остаётся fallback.
     Переключение — одна привязка в DI (обратимо).
   - **Библиотека: OSQP**, вендоринг через CMake FetchContent (pinned commit) +
     установка в `docker/Dockerfile.service`. Eigen (уже подключён) используется
     для сборки матриц `A_g/G_g`, но сам QP не решает.
   - **Детерминированный режим для replay:** фиксированные `max_iter`, отключённый
     adaptive-rho, фиксированные scaling/tolerances, без таймеров/random — чтобы
     `GroupedSolveResult` был бит-в-бит воспроизводим (ADR-009, AC-F09-010).
   - **Граница денег:** QP считает в double; результат квантуется в Decimal на
     фиксированном scale на выходе солвера, перед ledger (ADR-005, §9). double
     допустим только внутри солвера (diagnostics).

3. **Триггер пересмотра:** появление в проде combo со spread/factor-ограничениями,
   которые feasibility-gate блокирует слишком часто (метрика
   `GROUPED_PRE_TRADE_REJECTED` / `spread_constraint_failed`).

## Альтернативы

| Вариант | Плюсы | Минусы | Вердикт |
| --- | --- | --- | --- |
| **OSQP сейчас** | полный QP, общий | новая зависимость + Docker + numerical/replay hardening до реального спроса | отклонено (преждевременно) |
| **Ручной active-set/KKT на Eigen** | без новой либы (Eigen есть) | свой QP-солвер — сложно, риск ошибок в money-математике | отклонено (риск) |
| **Только feasibility-gate навсегда** | просто | combo со spread/factor часто блокируются вместо частичного исполнения | отклонено (теряем качество исполнения) |
| **Gate сейчас + OSQP за `IGroupedSolver` позже (выбрано)** | поставляем рабочее поведение; QP добавляем по спросу, обратимо | временно нет частичного исполнения при нарушении | **принято** |

## Последствия

- **Сейчас:** ratio/basket/factor-нейтральные combo исполняются (MVP-2/3);
  combo с активным spread/factor-нарушением — блокируются/деградируют (честно).
- **При внедрении QP:** +зависимость OSQP (сборка всех образов), +numerical tests
  (обусловленность, масштаб), +replay-детерминизм harness. Изолировано за
  `IGroupedSolver` — single-leg F-04 и bisection не затрагиваются.
- Контракт `GroupedSolveResult` не меняется (QP заполняет те же поля) →
  downstream (ExecutionGroup/ledger/risk) без изменений.

## Обратимость

Высокая. Алгоритм инкапсулирован в `IGroupedSolver`; добавление/удаление
`GroupedSolverQp` — аддитивно, fallback на bisection всегда доступен. OSQP можно
снять, если не оправдает себя, без изменения контрактов и downstream.
