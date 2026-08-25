<!-- level: kite -->
# F-05A — Vectorized External Liquidity for F-09 Batch/Combo Clearing

> **Статус:** `planned` · **Тип:** addendum к [F-05 Live Market Data](README.md) · **Потребитель:** [F-09](../F-09-batch-combo-orders/) `multileg_vector_solver`
> **Спецификация:** [`F-05A-feature.yaml`](F-05A-feature.yaml) · **План:** [implementation-plan](../../../implementation-plan/F-05A-vectorized-external-liquidity.tasks.md)
> **Источник:** IN-014 — [archive](../../../../incoming-docs/2026-07-07-F-05A-vectorized-external-liquidity-v1.md), [meta](../../../../incoming-docs/IN-014.meta.md), [fragment-map](../../../../incoming-docs/IN-014.fragment-map.md)

## 🧭 Navigation

| Уровень | Артефакт |
| --- | --- |
| ☁️ L0 | эта страница + `F-05A-feature.yaml` + L0 system sequences (TODO) |
| 🌊 L1 | Use Cases `UC-F05A-01..05` (TODO) + L1 service sequences (TODO) |
| 🐟 L2 | component-internal sequences (TODO) |
| 💻 src | `cpp/market_data`, `cpp/matching`, `cpp/ledger` (при реализации) |

## Назначение

F-05A расширяет [F-05](README.md): помимо live ticker/snapshot, `market_data`
преобразует **реальные внешние стаканы бирж** (CEX/DEX/AMM) в математически
пригодную форму для F-09 `multileg_vector_solver`.

```text
External Order Book
  → ExternalOrderLevel[]        # дискретные venue levels (provenance сохранён)
  → VectorFlowSegment[]         # каждый level → вектор активов w_i
  → W = [w_1 … w_I]             # матрица N активов × I levels
  → QP: max xᵀpH − ½xᵀDx  s.t. Wx = 0, 0 ≤ x ≤ q
  → ExecutionGroup / FillEvent / MarketDataSnapshot
```

Клиринг возникает из **баланса активов** `Wx = 0` (тот же принцип, что flow
conservation в F-04, business-rules R-CLR-003, [ADR-035](../../../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md)).

## Ключевое архитектурное решение

**Не строить синтетический pair book.** Каждый внешний order level остаётся
отдельным flow-сегментом и отдельным столбцом `W` (сохранены venue/source_order_id/
side/price). Matching multi-asset ликвидности (напр. цикл `BTC → USD → ETH → BTC`)
возникает из `Wx = 0`, а не из per-pair книг. Это:

- сохраняет provenance и source-trace каждого fill (AC-F05A-007/010);
- позволяет triangular / cross-pair clearing;
- переиспользует F-09 grouped framework (`ExecutionGroup`, `execution.groups`).

## Математика (кратко)

- **Bid X/Y:** `w_i = e_X − B_eff·e_Y` · **Ask X/Y:** `w_i = −e_X + A_eff·e_Y`.
- **Flow segment:** `(w_i, p_i^L=0, p_i^H=d_i^{HL}, q_i, Q_i^{max})`, `D = diag(dHL/q)`.
- **Residual:** `r = Wx`, `residualNorm = ‖r‖`; converged ⇔ `‖r‖ < tolerance`.

Полная модель — в [`F-05A-feature.yaml`](F-05A-feature.yaml) и IN-014 §3/§13/§16.

## Use Cases

| UC | Имя |
| --- | --- |
| UC-F05A-01 | Vectorize External Order Book |
| UC-F05A-02 | Run Vector Clearing with F-09 Solver |
| UC-F05A-03 | Display Vector Liquidity in UI |
| UC-F05A-04 | Validate Algorithm on Real Exchange Order Books |
| UC-F05A-05 | Replay Vector Clearing Scenario |

(Файлы `UC-F05A-*` создаются на ingress-close.)

## Components Involved

`market-data` (векторизация, `marketdata.vectorized`) · `external-venues`/`venues`
(источник стаканов) · `matching` (QP vector clearing, `ExecutionGroup`) · `ledger`
(balanced apply + surplus posting) · `risk` (surplus/residual alerts) ·
`observability` (метрики) · `gateway` (UI/API) · `backtest` (replay).

## Contracts & Data (planned)

- Proto: `contracts/proto/fob/marketdata/v1/vector_liquidity.proto` (T-F05A-101)
- Kafka: `marketdata.vectorized` (T-F05A-102); переиспользует `execution.groups`, `fills`, `venue.liquidity.fob`
- REST/WS: `/api/v1/marketdata/vector-liquidity*`, `WS /api/market/vector` (T-F05A-103)
- PG: `asset_basis`, `vector_flow_segments`, `vectorization_runs` (T-F05A-104)
- CH: `vector_clearing_results`, `surplus_events`, `vector_flow_segments_history` (T-F05A-105)

## Архитектурные решения (ADR)

- [ADR-047](../../../03-architecture/adr/ADR-047-surplus-exchange-pnl-policy.md) — surplus / EXCHANGE_PNL policy (`proposed`, blocker).
- [ADR-048](../../../03-architecture/adr/ADR-048-qp-solver-backend.md) — QP solver backend OSQP (`proposed`, blocker; активирует [ADR-034](../../../03-architecture/adr/ADR-034-grouped-constraint-solver.md)).

## Known Gaps

| ID | Severity | Description |
| --- | --- | --- |
| GAP-F05A-001 | blocker | ADR-047 surplus policy — proposed (решение владельца). |
| GAP-F05A-002 | blocker | ADR-048 QP backend — proposed; QP-решателя в matching пока нет. |
| GAP-F05A-003 | major | Dimensional units validation (KI-F05A-003). |
| GAP-F05A-004 | major | Stale external levels policy (KI-F05A-004). |
| GAP-F05A-005 | major | Vector/grouped replay в backtest (совм. с AC-F09-010, CN-IN014-05). |

## Трассировка

- Feature spec: [`F-05A-feature.yaml`](F-05A-feature.yaml)
- Implementation plan: [F-05A-*.tasks.md](../../../implementation-plan/F-05A-vectorized-external-liquidity.tasks.md)
- Base feature: [F-05 Live Market Data](README.md) · Consumer: [F-09](../F-09-batch-combo-orders/)
- Reused: [F-11 External Venues](../F-11-external-venues-lob-to-fob/) · [F-15 Backtest/Replay](../F-15-backtest-replay/)
- Source: IN-014 (см. header)
