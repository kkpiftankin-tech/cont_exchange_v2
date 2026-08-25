---
id: ADR-048
title: QP solver backend (OSQP) для vector clearing F-05A — активация и расширение ADR-034
status: accepted
date: 2026-07-07
accepted: 2026-08-25
level: sea
feature: F-05A
related: [ADR-034, ADR-035, ADR-047, ADR-005, ADR-009]
---

# ADR-048 — QP solver backend (OSQP) для F-05A vector clearing

> **Принято 2026-08-25** (решение владельца): OSQP-backend с детерминированным replay-режимом и `Decimal`-квантованием на границе солвера (§9) утверждён. Разблокирует критический путь QP-реализации F-05A (Phase 3) и общий QP-backend для F-09 combo. Обработка ненулевого остатка (`residual → surplus`) остаётся за **[ADR-047](ADR-047-surplus-exchange-pnl-policy.md)** (`proposed`): money-path/surplus-задачи F-05A не разблокированы до его принятия.

## Контекст

[ADR-034](ADR-034-grouped-constraint-solver.md) уже **выбрал OSQP** как QP-backend за
интерфейсом `IGroupedSolver`, но **отложил** его реализацию до реального спроса
(«feasibility-gate сейчас, QP позже»; триггер — combo со spread/factor-ограничениями
в проде).

**F-05A (IN-014) — этот спрос.** Vector clearing внешней ликвидности решает
полноценную QP:

$$
\max_x \left[ x^\top p^H - \tfrac12 x^\top D x \right]
\quad\text{s.t.}\quad Wx = 0,\ 0 \le x \le q,
$$

где $W \in \mathbb{R}^{N\times I}$ (N активов, I внешних order levels), $D = \operatorname{diag}(dHL/q)$ (SPD),
$p^H = dHL$. Это **настоящая QP** (квадратичная цель + линейное равенство $Wx=0$ +
box), в отличие от ADR-034-постановки $\max \alpha$ (линейная по $\alpha$). Обе — одна семья
(equality + box) и обе решаются одним backend.

В `cpp/matching` сейчас QP-решателя нет (только `grouped_solver_bisection` +
`simulated_solver`). Выбор либы над деньгами требует ADR (CLAUDE.md §3.3), затрагивает
детерминизм F-15 replay (AC-F05A-011) и границу double↔Decimal (§9, ADR-005).

## Решение

1. **Активировать OSQP** (решение ADR-034 п.2), общий backend для двух QP-задач:
   - F-09 combo grouped re-solve (ADR-034: $\max \alpha \text{ s.t. } A_g e = b_g \alpha,\ G_g e \le h_g$);
   - F-05A vector clearing (эта ADR).
   Вендоринг через **CMake FetchContent (pinned commit)** + установка в
   `docker/Dockerfile.service`. **Eigen** (уже подключён) — только сборка/масштабирование
   матриц `W`, `D`; сам QP решает OSQP.

2. **Отображение F-05A-задачи в стандартную форму OSQP**
   ($\min \tfrac12 x^\top P x + q^\top x \text{ s.t. } l \le Ax \le u$):
   ```text
   P = D                         # SPD, diag(dHL/q)
   q = -p^H                      # максимизация → минимизация -pH·x
   A = [ W ; I_I ]               # (N + I) × I
   l = [ 0_N ; 0_I ]
   u = [ 0_N ; q_box ]           # Wx=0 задаётся как 0 ≤ Wx ≤ 0
   ```
   Решатель за интерфейсом (`IVectorClearingSolver`, аналог `IGroupedSolver`);
   bisection/gate не затрагиваются, single-leg F-04 не затрагивается.

3. **Детерминированный режим для replay** (как ADR-034, ADR-009, AC-F05A-011):
   фиксированные `max_iter`, **отключённый adaptive-rho**, фиксированные
   scaling/tolerances, без таймеров/random. `VectorClearingResult` (x, π, residual,
   diagnostics) обязан быть бит-в-бит воспроизводим на тех же входах.

4. **Граница денег** (ADR-005, §9): OSQP считает в `double`; результат `x`
   **квантуется в `Decimal`** на фиксированном scale на выходе солвера, до ledger.
   `double` допустим только внутри солвера и в diagnostics (residualNorm, solveTimeMs).

5. **Residual и surplus:** solver возвращает $r = Wx$ и $\text{residualNorm} = \|r\|$.
   Обработка ненулевого остатка — по [ADR-047](ADR-047-surplus-exchange-pnl-policy.md)
   (`SurplusPolicy`). QP-решатель residual **не прячет** — отдаёт его наружу.

6. **Numerical hardening (обязательно до money-path):** масштабирование `W`/`D` с
   учётом размерностей активов (связка с `dimensional_guard`, KI-F05A-003);
   проверка обусловленности; поведение при $q_i \to 0$, вырожденной `W`,
   несовместных ограничениях (→ `degraded`/`failed`, не молчаливый мусор).

7. **CI/Docker (урок F-09).** OSQP как новая зависимость добавляется **синхронно** в
   `docker/Dockerfile.service` (builder) **и** `.github/workflows/cpp-build.yml`
   (CI-раннер) — иначе `cpp-build` падает на configure/link (как было с `libabsl`).
   FetchContent (pinned) снимает часть риска, но toolchain-deps должны совпадать.

## Альтернативы

| Вариант | Плюсы | Минусы | Вердикт |
| --- | --- | --- | --- |
| **OSQP (выбрано)** | полноценный QP eq+box; уже выбран ADR-034; один backend на F-09+F-05A | новая зависимость + Docker/CI + numerical/replay hardening | **принято** (спрос от F-05A настал) |
| **Ручной active-set/KKT на Eigen** | без новой либы | свой QP над деньгами — сложно, риск ошибок | отклонено (риск) |
| **Bisection/closed-form как в F-09** | уже есть | не решает cross-asset $Wx=0$ с квадратичной целью — задача F-05A другая | неприменимо |
| **Внешний QP-микросервис** | изоляция | сеть/latency/детерминизм/replay сложнее | отклонено (overkill для in-batch) |

## Последствия

- ✅ Один QP-backend закрывает и остаток F-09 (ADR-034 QP re-solve), и F-05A vector
  clearing — переиспользование, а не две реализации.
- ➕ Зависимость OSQP во всех образах; numerical + replay-детерминизм harness;
  `IVectorClearingSolver` + DI-привязка; квантование double→Decimal на выходе.
- ⚠️ Money-path включается только после numerical hardening (п.6) и тестов
  детерминизма/idempotency; до этого — `REJECT_IF_RESIDUAL` (ADR-047 дефолт).
- Контракты downstream (`ExecutionGroup`, ledger, risk) не меняются — solver
  заполняет существующие поля.

## Обратимость

Высокая. QP инкапсулирован за `IVectorClearingSolver`/`IGroupedSolver`; добавление —
аддитивно, fallback (gate/reject) доступен. OSQP можно снять без изменения контрактов.
Дефолт surplus `REJECT_IF_RESIDUAL` (ADR-047) означает, что до включения money-path
изменения money-модели нет.

## Трассировка

- Feature: **F-05A** (IN-014). Закрывает GAP-F05A-002 / KI-F05A-002; поддерживает
  AC-F05A-004, AC-F05A-005, AC-F05A-011.
- Расширяет/активирует: [ADR-034](ADR-034-grouped-constraint-solver.md) (QP deferred → now).
- Related: [ADR-035](ADR-035-fob-solver-mathematical-foundation.md) (clearing math),
  [ADR-047](ADR-047-surplus-exchange-pnl-policy.md) (residual → surplus),
  [ADR-005](ADR-005-fixed-point-decimal-money.md) (Decimal boundary),
  ADR-009 (replay isolation/determinism).
- Code (при реализации): `cpp/matching/src/domain/vector_qp_solver.{hpp,cpp}`,
  `IVectorClearingSolver`, `docker/Dockerfile.service`, `.github/workflows/cpp-build.yml`,
  `contracts/CMakeLists.txt`/matching CMake (FetchContent OSQP).
- Data/config: `solver_config` (детерминированные QP-параметры).
