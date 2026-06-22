# Solver Foundation: Mapping IN-012 ↔ `solver_impl.cpp`

> **Назначение**: implementation-level reference, связывающий
> canonical IN-012 формулы с конкретными строками
> [cpp/matching/src/domain/solver_impl.cpp](../../cpp/matching/src/domain/solver_impl.cpp).
>
> **Foundation**: [ADR-035](../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md),
> [IN-012](../../incoming-docs/IN-012.meta.md),
> [business-rules.md §Clearing Mechanics](../04-domain/business-rules.md#clearing-mechanics-in-012).

## Mapping: IN-012 объекты ↔ C++ переменные

| IN-012 notation | C++ identifier | File location |
| --- | --- | --- |
| `Q_i` (position) | `order.q_max`, `order.remaining_qty()` | `domain/flow_order.hpp` |
| `Q̇_i` (speed) | `q_rate`, `x[i]` (solver output) | `solver_impl.cpp::Solve()` |
| `P` (price vector) | `pi` (Eigen::VectorXd) | `solver_impl.cpp::SolveImpl()` |
| `L_i` (Lagrangian) | объединён в matrix `W` + диагональ `d` | `solver_impl.cpp::Init()` |
| `H_i` (Hamiltonian) | dual potential, implicit в KKT | — |
| `H(Q, P) = Σ_i H_i` | normal equation matrix `W·η·W^⊤` | `solver_impl.cpp::SolveImpl()` predictor |
| `V_i(Q_i, P)` | `Vi(Qi, P) = ∇_P H_i(Qi, P)` | implicit — solver выдаёт `x = V` |
| `M_i` (inertia) | `m_i = (p_high - p_low) / q_rate` | `Init()` строка `d(order) = ...` |
| `a_i` (anchor) | `pH(order) = (double)p_high` | `Init()` строка `pH(order) = ...` |
| `B = Σ M_i^(-1)` | агрегированный через `W^⊤ · diag(η) · W` | predictor `Lmatrix` |
| `b = Σ M_i^(-1) a_i` | `pH` vector + virtual buy/sell columns | `Init()` |
| `P* = arg min_P H(Q, P)` | `pi` после convergence interior-point loop | `SolveImpl()` return |
| `Σ_i Q̇_i = 0` (clearing) | `r_cp = W·x` residual | `SolveImpl()::predictor()` |

## Алгоритмический выбор: Mehrotra interior-point

Текущий solver — **predictor-corrector interior-point** (Mehrotra-style)
с LLT decomposition нормальной системы. Это **numerical implementation**
формулировки IN-012, не альтернатива ей.

Соответствие:

```text
IN-012 §4.3 (Lagrange multiplier для Σ Q̇ = 0)
  ⇕ канонически эквивалентно ⇕
solver_impl.cpp KKT-системе:
  pH - d·x - W^⊤·π + μ - λ = 0   (stationarity, gradient)
  W·x = 0                          (primal feasibility, clearing)
  μ_i · x_i = σ·τ                  (relaxed complementarity)
  λ_i · (qH-x)_i = σ·τ             (relaxed complementarity)
  0 ≤ x ≤ qH, μ, λ ≥ 0              (box bounds + dual feasibility)
```

`π` (mult. Lagrange для `W·x = 0`) ≡ `P*` из IN-012.

Альтернативы (см. ADR-035 §Alternatives):

- A. Walras tâtonnement — отвергнуто (slow, no closed-form tests).
- B. LP approximation — отвергнуто (теряем strict convexity).
- C. Closed-form `B^(-1) b` — **сохранено как test oracle** (см. ниже).
- D. Mean-field game — отвергнуто (overkill для local clearing).

## Test vectors из IN-012 (closed-form oracles)

Эти формулы — canonical reference для verification solver'а.
Реализация — [continuous_market_replica_test.cpp](../../cpp/matching/tests/domain/).

### Test 1: N standard 1D agents (IN-012 Предложение 6.1)

```text
INPUT:  N agents с (m_i, a_i), m_i > 0
EXPECT:
  m* = (Σ 1/m_i)^(-1)
  a* = m* · Σ (a_i / m_i)
  p* = a*
  V_aggregate(P) = (P - a*) / m*
```

Проверяет R-CLR-006. Минимум 3 теста: N = 2, N = 3, N = 5.

### Test 2: 3-agent correlated 2D market (IN-012 §6.2.5)

```text
INPUT:  два одноактивных {(m_1, a_1)}, {(m_2, a_2)} + один spread agent (m_3, 0)
EXPECT (closed-form):
  p*_1 = (a_1·(m_2 + m_3) + a_2·m_1) / (m_1 + m_2 + m_3)
  p*_2 = (a_2·(m_1 + m_3) + a_1·m_2) / (m_1 + m_2 + m_3)
  p*_1 − p*_2 = m_3 / (m_1 + m_2 + m_3) · (a_1 − a_2)
```

Проверяет R-CLR-007. Spread-agent scales spread proportionally to `m_3`.

### Test 3: Standard 1D agent isolated (sanity)

```text
INPUT:  1 agent с (m=1, a=100)
EXPECT:
  H(p) = (p - 100)² / 2
  q̇(p) = p - 100
  p* = 100  (нет других агентов → clearing на anchor'е)
```

Тривиальный sanity — проверяет single-agent path.

### Test 4: Quadratic closed-form B^(-1) b (IN-012 §4.5)

```text
INPUT: K agents с (M_i, a_i), 2D case
EXPECT:
  B = Σ M_i^(-1)
  b = Σ M_i^(-1) a_i
  P* = − B^(-1) b
```

Проверяет R-CLR-005 в multi-agent multi-asset режиме.

## Tolerance contract

Solver IS numerical → exact equality невозможна. Tolerance contract
для test vectors:

| Quantity | Absolute tolerance | Relative tolerance |
| --- | --- | --- |
| `P*` (clearing price) | `1e-9` | `1e-6` |
| `V(P*)` (residual flow) | `1e-9` | — |
| `H(P*)` (energy minimum) | — | `1e-6` |

Эти tolerances компатибельны с `cfg_.tolerance = 1e-6` в
`ContinuousClearingSolver` default config.

## Singular cases handling (IN-012 §A)

| Singular limit | IN-012 §A recommendation | C++ implementation |
| --- | --- | --- |
| `m_i → 0` (perfect liquidity) | Add `ε·q̇²/2` regularization | Solver не accepts m=0 directly. Users подают `m_min ≥ 1e-9`. |
| `m_i → ∞` (perfect rigidity) | Clamp `M_i ≼ M_max·I` | Не enforced explicitly — рассчитано на user discipline. |
| `W·η·W^⊤` near-singular | Diagonal regularization | `reg = 1e-12 * Lmatrix.diagonal().maxCoeff()` в `predictor()` |

OQ-035-03 (ADR-035): достаточно ли `1e-12` для всех use cases? Open
для F-15 determinism investigations.

## When to update this document

Update mandatory:

- При замене numerical solver (Mehrotra → PROXQP / OSQP / LDLT).
- При добавлении нового agent class в `entities.md §Continuous-Order Primitives`.
- При changes в `solver_impl.cpp` line numbers (refactor) — обновить
  mapping table.
- При обнаружении breakage tolerance contract.

Update НЕ требуется:

- При reorder методов внутри одного и того же solver file.
- При rename переменных — обновить только mapping table.

## Related artifacts

- ADR-035 ([../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md](../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md))
- IN-012 fragment-map ([../../incoming-docs/IN-012.fragment-map.md](../../incoming-docs/IN-012.fragment-map.md))
- Domain — [entities.md](../04-domain/entities.md), [business-rules.md](../04-domain/business-rules.md)
- Solver source — [cpp/matching/src/domain/solver_impl.cpp](../../cpp/matching/src/domain/solver_impl.cpp)
- Unit tests — [cpp/matching/tests/domain/](../../cpp/matching/tests/domain/)
