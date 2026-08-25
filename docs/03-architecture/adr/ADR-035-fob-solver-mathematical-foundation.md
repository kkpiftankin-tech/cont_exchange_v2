# ADR-035 — FOB Solver Mathematical Foundation: Hamiltonian-Based Formulation

> **Status**: accepted (retroactive — solver уже реализован)
>
> **Date**: 2026-06-09
>
> **Source**: IN-012 (continuous-order market academic note,
> dated 2026-04-15)

## Context

Matching engine реализует Flow Order Book (FOB) — рынок непрерывных
заявок (CSLO). До IN-012 формулировка solver'а была implicit: код
[cpp/matching/src/domain/solver_impl.cpp](../../../cpp/matching/src/domain/solver_impl.cpp)
использует KKT-based interior-point loop (Mehrotra predictor-corrector)
с LLT decomposition нормальной системы, но **каноническая
математическая основа** в документах не была явно зафиксирована.

IN-012 (44-страничная академическая записка от 2026-04-15)
систематически излагает теорию рынка непрерывных заявок в
выпукло-аналитической формулировке:

- 4 эквивалентных представления индивидуальной кривой агента
  (V-form, P-form, L-form, H-form);
- агрегированный рынок через сумму гамильтонианов + инфимальную
  свёртку лагранжианов;
- цена клиринга как множитель Лагранжа для ограничения баланса
  потоков;
- замкнутые формулы для квадратического семейства;
- типология одно- и двумерных агентов (12 классов) и теорема
  репликации произвольной матрицы ликвидности `Λ`.

Этот ADR ретроактивно фиксирует выбор формулировки и устанавливает
canonical reference для всех future research и упрощений.

## Decision

Принимается **Hamiltonian-based convex QP formulation** в качестве
canonical математической основы для production solver:

- Каждый агент моделируется через `L_i(Q_i, Q̇_i)` (или эквивалентно
  через `H_i(Q_i, P) = L_i(Q_i, ·)*`).
- Совокупный рынок — `H(Q, P) = Σ_i H_i(Q_i, P)` (canonical sum,
  IN-012 §4.3, формула 4.10).
- Цена клиринга — общий множитель Лагранжа:
  `P*(Q) = arg min_P H(Q, P)` ⟺ `Σ_i Q̇_i(Q_i, P*) = 0` (IN-012
  §4.3, Следствие 4.2).
- Solver реализует interior-point метод над этой QP-формулировкой.
- Все типы агентов проекта (`StandardAgent`, `LinearFeeAgent`,
  `PortfolioAgent`, etc — см. [entities.md](../../04-domain/entities.md))
  трактуются как конкретные параметризации `L_i` / `H_i`.

## Alternatives considered

### A. Walrasian tâtonnement (price-discovery iteration)

Дискретный поиск цены `P*` методом Уолраса (tâtonnement): итеративно
поднимаем `P`, пока `Σ_i Q̇_i(Q_i, P) > 0`, иначе понижаем.

**Отклонено**: численно медленный (линейная сходимость), плохо
работает для многомерного случая, не даёт замкнутых формул для
тестов, не использует convexity напрямую.

### B. LP / linear-programming formulation

Свести clearing к LP через piecewise-linear approximation `L_i`.

**Отклонено**: теряем precision strict convexity (необходима для
единственности клиринга — IN-012 Предложение 4.3), плохо
масштабируется на квадратические штрафы.

### C. Walras / matrix inversion в замкнутом виде

Для квадратического класса `P*(Q) = − B^(-1) b` (IN-012 формула 4.23).

**Отклонено как exclusive choice**, но **сохранено как test oracle**:
для unit-тестов (replicate scenarios IN-012 §6) используется
точная формула.

### D. Mean-field game formulation (Gomes-Saúde)

Constrained MFG с price formation как mean-field equilibrium
(IN-012 §2, ссылка [12]).

**Отклонено**: переусложнение для local instant clearing. Применимо
к dynamic optimal execution problems (F-12 hedge planning), не к
batch clearing.

## Consequences

### Positive

1. **Каноническая ссылка** для всей matching mathematics — IN-012.
   Будущий research и refactor имеют единую academic basis.
2. **Test vectors** доступны из закрытых формул IN-012 §4.5, §6.1.3,
   §6.2.5. См. [solver-foundation.md](../../09-implementation/solver-foundation.md)
   и [continuous_market_replica_test.cpp](../../../cpp/matching/tests/domain/).
3. **Доменная типология** (12 классов агентов) даёт canonical
   vocabulary для F-04, F-09, F-10, F-11 — см.
   [entities.md §Continuous-Order Primitives](../../04-domain/entities.md#continuous-order-market-primitives-in-012).
4. **Verifiability**: 3 эквивалентных характеристики clearing price
   позволяют independent verification в determinism tests F-15.

### Negative / Trade-offs

1. **Singular cases** (m → 0 или m → ∞) требуют регуляризации
   (R-CLR-008). Solver не принимает singular agents напрямую.
2. **Multi-leg combo orders (F-09)**: §6.3.1 даёт `r*(Λ) ≤ 1` для 2D,
   но для N-leg может требоваться выше rank. Open research для F-09.
3. **Numerical solver** — Mehrotra interior-point — реализационный
   выбор, не каноническая часть теории. Можно заменить на LDLT с
   регуляризацией или PROXQP без нарушения mathematical foundation.

### Neutral

1. ADR ретроактивен — solver уже реализован. ADR не вызывает
   изменений в production code, только фиксирует documentation.
2. PDF (44 страницы) остаётся canonical reference; markdown-транскрипция
   (incoming-docs/2026-04-15-continuous-order-market-academic-v1.md)
   — для grep'а / cross-link / fragment-mapping.

## Reversibility

**Medium reversibility**. Mathematical foundation решение редко
revertится без значимого rewrite. Если будущий research предложит
fundamentally different framework (например, free-energy market
formulation или non-convex extension), требуется новый ADR
(ADR-NNN-replaces-035) и обоснованная миграция unit tests.

Замена numerical solver (Mehrotra → PROXQP / OSQP / etc) **не**
требует нового ADR — это внутренняя infrastructure detail.

## Related artifacts

- IN-012 (canonical reference) —
  [incoming-docs/2026-04-15-continuous-order-market-academic-v1.md](../../../incoming-docs/2026-04-15-continuous-order-market-academic-v1.md),
  [IN-012.meta.md](../../../incoming-docs/IN-012.meta.md),
  [IN-012.fragment-map.md](../../../incoming-docs/IN-012.fragment-map.md).
- Domain — [entities.md §Continuous-Order Primitives](../../04-domain/entities.md#continuous-order-market-primitives-in-012),
  [business-rules.md §Clearing Mechanics](../../04-domain/business-rules.md#clearing-mechanics-in-012).
- Implementation — [solver-foundation.md](../../09-implementation/solver-foundation.md),
  [cpp/matching/src/domain/solver_impl.cpp](../../../cpp/matching/src/domain/solver_impl.cpp).
- Tests — [cpp/matching/tests/domain/continuous_market_replica_test.cpp](../../../cpp/matching/tests/domain/).
- Features — F-04 (canonical user), F-09 (multi-leg extension),
  F-10 (PortfolioAgent), F-11 (LOB-FOB curve typology).

## Open questions

| OQ | Description | Tracked in |
|---|---|---|
| OQ-035-01 | Verify text exact alignment между текущим `solver_impl.cpp` Mehrotra-loop и Lagrange-multiplier formulation IN-012 §4.3. | CN-IN012-01 in IN-012.meta.md |
| OQ-035-02 | Применимость теоремы репликации §6.3.1 к F-09 multi-leg solver на N>2 активах. | F-09 implementation plan |
| OQ-035-03 | Регуляризация singular cases: текущее `reg = 1e-12 * diag_max` достаточно для всех use cases? | F-15 determinism tests |
