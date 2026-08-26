---
id: ADR-047
title: Surplus / EXCHANGE_PNL policy для vector clearing (F-05A)
status: accepted
date: 2026-07-07
accepted: 2026-08-26
level: sea
feature: F-05A
related: [ADR-034, ADR-035, ADR-033, ADR-048]
---

# ADR-047 — Surplus / EXCHANGE_PNL policy для vectorized external clearing

> **Принято 2026-08-26** (решение владельца): surplus-политика с enum-выбором и
> **prod-дефолтом `REJECT_IF_RESIDUAL`** утверждена. Непереговорный инвариант —
> ledger никогда не применяет несбалансированный `ExecutionGroup` (§17); остаток —
> либо в пределах tolerance ($Wx\approx0$), либо явной house-проводкой. `surplus.amount`
> = `Decimal` (§9), идемпотентно по `execution_group_id`. Разблокирует T-F05A-304
> (surplus_policy) и money-path T-F05A-305 (эмиссия fills/ExecutionGroup).

## Контекст

F-05A (IN-014) подаёт внешнюю venue-ликвидность в matching как набор векторных
flow-сегментов $w_i$ (столбцы матрицы $W \in \mathbb{R}^{N\times I}$, N — активы, I — order levels) и
решает QP:

$$
\max_x \left[ x^\top p^H - \tfrac12 x^\top D x \right]\quad\text{s.t.}\quad Wx = 0,\ 0 \le x \le q.
$$

Условие $Wx = 0$ — это **баланс активов** (никакой актив не создаётся и не
уничтожается при клиринге). Это тот же принцип, что flow conservation в F-04
($\sum_i \dot{Q}_i(Q_i, P^*) = 0$, business-rules **R-CLR-003**, ADR-035).

**Проблема.** На реальных стаканах идеальный треугольник (`BTC/USD ask + BTC/ETH bid +
ETH/USD bid`) почти никогда не замыкается точно. Solver возвращает остаток

$$
r = Wx,\qquad residualNorm = \|r\|,
$$

где $r \neq 0$ по одному или нескольким активам — это **surplus/deficit** (например, +ε USD).
Это денежная величина (CLAUDE.md **§9** — только `Decimal`), и она **не должна молча
теряться**: иначе ledger применит несбалансированный `ExecutionGroup`/`FillEvent`,
создастся **phantom inventory** и нарушится инвариант сохранения (§17).

Сейчас в matching нет ни QP-решателя (ADR-034 отложил QP), ни понятия surplus, ни
«домового» (house) счёта в ledger. F-05A требует явную политику до любого money-flow
(GAP-F05A-001 / KI-F05A-001 / AC-F05A-005 / AC-F05A-007, blocker).

## Решение

Ввести **явную surplus-политику** с enum-выбором (config-driven, per solver_config):

```text
SurplusPolicy = REJECT_IF_RESIDUAL | EXCHANGE_PNL | SURPLUS_ASSET | MM_LAST_RESORT
```

1. **`REJECT_IF_RESIDUAL` — дефолт для MVP.** Если `residualNorm > tolerance`, группа
   помечается `degraded`/`failed`, **исполнения не применяются** как полностью
   клиринговые (никакие несбалансированные проводки в ledger не идут). Безопасно,
   согласуется с консервативным MVP-подходом F-09 (ср. MVP-7 auto-resolve default-off).

2. **`EXCHANGE_PNL`.** Остаток `r` проводится на выделенный **house-счёт**
   (`EXCHANGE_PNL` / `SURPLUS`), так что суммарно сохраняется баланс:
   $\sum(\text{user postings}) + (\text{house posting}) = 0$. Идемпотентно по `execution_group_id`,
   с before/after snapshot и audit trail (§17). Требует появления house-счёта в ledger.

3. **`SURPLUS_ASSET`.** Остаток фиксируется как отдельная surplus-позиция по активу
   (без немедленного PnL-признания) для последующего разбора/неттинга.

4. **`MM_LAST_RESORT`.** Остаток закрывается малой линейной MM-ликвидностью
   («market maker of last resort»); допустимо только при явно сконфигурированном
   лимите MM.

**Инвариант (непереговорный).** Ledger **никогда** не применяет несбалансированный
`ExecutionGroup`. Либо группа балансируется в пределах `tolerance` ($Wx \approx 0$), либо
остаток **явно** проводится на house-счёт — total conservation держится, phantom
inventory не создаётся.

**Наблюдаемость.** На каждый ненулевой остаток эмитится `SurplusEvent` →
`surplus_events` (ClickHouse) + метрика `surplus_events_total`; при превышении порога —
`risk.alerts`. Surplus **всегда виден**, никогда не скрыт (AC-F05A-005/007).

**Деньги.** `surplus.amount` — `Decimal` (§9); house-проводка идемпотентна по
`execution_group_id` (§17); операторские изменения политики — авторизованы и audit-logged.

## Альтернативы

- **Молча отбрасывать остаток** — ❌ отклонено: phantom inventory, нарушение сохранения
  и §17.
- **Всегда `REJECT` (любой ненулевой residual блокирует)** — слишком строго для
  production: теряется реальный cross-venue арбитраж с малым честным surplus. Оставляем
  как MVP-дефолт, но не как единственный режим.
- **Всегда `EXCHANGE_PNL` по умолчанию** — требует house-счёта с первого дня и рискует
  «прятать» убытки в PnL; небезопасно как дефолт. Делаем opt-in.
- **Отдельный micro-order на остаток внутрь FOB-книги** — усложняет и смешивает
  внешний клиринг с внутренним; отложено.

## Последствия

- ✅ Явная, аудируемая обработка surplus; ledger-инвариант сохранения защищён.
- ✅ Дефолт `REJECT_IF_RESIDUAL` не меняет money-модель, пока `EXCHANGE_PNL` выключен
  (обратимо, как default-off у F-09 MVP-7).
- ➕ Требует: house-счёт (`EXCHANGE_PNL`/`SURPLUS`) в ledger; контракт `SurplusEvent`
  (в будущем `vector_liquidity.proto`); CH-таблица `surplus_events`;
  поле `surplus_policy` в `solver_config`; метрика + alert в observability.
- ⚠️ `EXCHANGE_PNL`/`MM_LAST_RESORT` — money-touching, включаются только после
  реализации house-счёта и тестов double-apply/idempotency.

## Обратимость

Высокая. Политика — config (`solver_config.surplus_policy`). Дефолт `REJECT_IF_RESIDUAL`
означает отсутствие изменения money-модели до явного включения `EXCHANGE_PNL`. Откат —
вернуть политику в reject; house-проводки аналитически отделимы (идемпотентны по
`execution_group_id`).

## Трассировка

- Feature: **F-05A** (IN-014). Закрывает GAP-F05A-001 / KI-F05A-001; поддерживает
  AC-F05A-005, AC-F05A-007.
- Related: [ADR-034](ADR-034-grouped-constraint-solver.md) (QP solver — отдельный
  ADR-048 backend), [ADR-035](ADR-035-fob-solver-mathematical-foundation.md) (flow
  conservation / clearing math), [ADR-033](ADR-033-execution-groups-topic.md)
  (`ExecutionGroup`).
- Domain: business-rules **R-CLR-003** (flow conservation), CLAUDE.md **§9** (Decimal),
  **§17** (ledger инварианты).
- Data: `surplus_events` (ClickHouse), `solver_config.surplus_policy` (PostgreSQL).
- Code (при реализации): `cpp/matching/src/domain/surplus_policy.hpp`,
  `cpp/matching/src/domain/vector_qp_solver.*`, `cpp/ledger/src/app/ledger_uc.*`
  (house-account posting).
