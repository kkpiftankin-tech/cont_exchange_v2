---
id: DOC-TEST-F-05A
phase: 10-testing
status: draft
owner: core-team
source:
  - IN-014 §8 (Testing), §9 (Acceptance Criteria), §3/§13/§16 (Математика/Domain/Algorithm)
related:
  - docs/02-system/features/F-05-live-market-data/F-05A-feature.yaml
  - docs/02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md
  - docs/05-components/sequences/SEQ-F05A-UC-F05A-01-services.md
  - docs/05-components/sequences/SEQ-F05A-UC-F05A-02-services.md
  - docs/07-data/vector-clearing-results.md
  - docs/03-architecture/adr/ADR-044-surplus-exchange-pnl-policy.md
  - docs/03-architecture/adr/ADR-045-qp-solver-backend.md
  - docs/10-testing/features/F-15-test-plan.md
---

# F-05A Vectorized External Liquidity — план тестирования

Полный источник: [`incoming-docs/2026-07-07-F-05A-vectorized-external-liquidity-v1.md`](../../../incoming-docs/2026-07-07-F-05A-vectorized-external-liquidity-v1.md) §8–§9.

> Легенда: ✅ выполнено, ⚠ частично, ❌ не выполнено.
>
> F-05A в статусе `planned` — реализация не начата, все тесты ❌ (запланированы).
>
> **Главный принцип (IN-014 §8.1):** unit-тесты на синтетике **недостаточны**.
> Обязательны реальные captured order book snapshots минимум из двух независимых venues;
> CI выполняется **offline** (без live API).

## Раздел 1. Unit-тесты (U-F05A-001..010)

Чистая доменная/алгоритмическая логика без Kafka/живых venue. GTest.
Файлы — `cpp/market_data/tests/` (векторизация) и `cpp/matching/tests/` (QP solver).

| # | Тест | Цель / инвариант | AC |
| --- | --- | --- | --- |
| U-F05A-001 | BuildBidVector | BID X/Y → `w = e_X − P_eff·e_Y` (R-F05A-001); Decimal | AC-F05A-002 |
| U-F05A-002 | BuildAskVector | ASK X/Y → `w = −e_X + P_eff·e_Y`; знак противоположен bid | AC-F05A-002 |
| U-F05A-003 | EffectivePrice | `P_eff = P ± fees ± latency ± slippage` корректно меняет цену | AC-F05A-001 |
| U-F05A-004 | AssetBasisDeterministic | Asset basis стабилен и детерминирован для одного набора активов | AC-F05A-003 |
| U-F05A-005 | WMatrixShape | `W` имеет размер `N × I`; каждый level — отдельный столбец (no synthetic book) | AC-F05A-003 |
| U-F05A-006 | DMatrixDiag | `D = diag(dHL/q)` строится корректно; SPD | AC-F05A-004 |
| U-F05A-007 | BoxConstraint | `0 ≤ x ≤ q` enforced солвером | AC-F05A-004 |
| U-F05A-008 | TriangleClearsToZero | Сбалансированный synthetic triangle → `Wx = 0`, `residualNorm < tol` | AC-F05A-006 |
| U-F05A-009 | SurplusDetected | `‖Wx‖ > tol` → surplus обнаружен и не скрыт; `surplus_policy` применена | AC-F05A-005, AC-F05A-007 |
| U-F05A-010 | SourceMappingPreserved | `venue_id` / `source_order_id` сохранены сегмент→fill (traceability) | AC-F05A-007 |
| U-F05A-011 | DimensionalGuard | Несовместные единицы (base/quote/price/rate) → level отклонён (KI-F05A-003) | AC-F05A-001 |

## Раздел 2. Integration на реальных стаканах (I-F05A-001..010)

Реальные captured fixtures (`tests/fixtures/real-orderbooks/`). Offline, детерминированно.

| # | Тест | Цель | AC |
| --- | --- | --- | --- |
| I-F05A-001 | Binance depth → segments | Real Binance `/api/v3/depth` → `ExternalOrderLevel[]` → `VectorFlowSegment[]` | AC-F05A-001, AC-F05A-006 |
| I-F05A-002 | Coinbase product book → W | Real Coinbase product book → normalized levels → `W` | AC-F05A-006 |
| I-F05A-003 | Kraken depth → W | Real Kraken `/Depth` → normalized levels → `W` | AC-F05A-006 |
| I-F05A-004 | Multi-pair vector input | BTC/USDT + ETH/USDT + ETH/BTC fixtures → vector solver input | AC-F05A-003 |
| I-F05A-005 | Solver residual/surplus | `x` даёт `residualNorm < tol` **или** явный surplus | AC-F05A-004, AC-F05A-005 |
| I-F05A-006 | Kafka round-trip | `marketdata.vectorized` продюсится и консюмится Matching | AC-F05A-009 |
| I-F05A-007 | ExecutionGroup source-trace | `ExecutionGroup` связывает каждую ногу с venue/source level | AC-F05A-010 |
| I-F05A-008 | ClickHouse persistence | `vector_clearing_results` записан | AC-F05A-012 |
| I-F05A-009 | API diagnostics | REST/gRPC отдают `(W, x, residual)` для fixture | AC-F05A-013 |
| I-F05A-010 | Deterministic replay | Replay captured fixtures → детерминированные `(W, x, residual)` | AC-F05A-011 |

## Раздел 3. UI-тесты (UI-F05A-001..008)

| # | Тест | AC |
| --- | --- | --- |
| UI-F05A-001 | Raw order book chart рендерит bids/asks из real fixture | AC-F05A-UI-001 |
| UI-F05A-002 | W heatmap рендерит матрицу с корректными размерами | AC-F05A-UI-003 |
| UI-F05A-003 | Execution vector chart рендерит `x_i` | AC-F05A-UI-004 |
| UI-F05A-004 | Residual chart рендерит `Wx` по активам | AC-F05A-UI-005 |
| UI-F05A-005 | `residualNorm > tolerance` показан как warning | AC-F05A-UI-006 |
| UI-F05A-006 | Клик по сегменту открывает source venue/orderbook level | AC-F05A-UI-007 |
| UI-F05A-007 | Clearing graph показывает asset cycle (triangular) | AC-F05A-UI-008 |
| UI-F05A-008 | Replay UI: expected vs actual solver result | AC-F05A-UI-010 |

## Раздел 4. Replay (R-F05A-001) — F-15

`R-F05A-001`: те же external snapshots → тот же `W / x / π / residual / ExecutionGroup`
(бит-в-бит, NFR-F05A-001). ⚠ Зависит от grouped/vector пути в `cpp/backtest`
(совместно с AC-F09-010, CN-IN014-05). AC: **AC-F05A-011**.

## Раздел 5. Performance (P-F05A-001..005)

| # | Тест | SLA |
| --- | --- | --- |
| P-F05A-001 | Vectorize 1,000 levels | `p95 < 100 ms` |
| P-F05A-002 | Vectorize 10,000 levels | под configured SLA |
| P-F05A-003 | UI W heatmap для 1,000 segments | без freeze |
| P-F05A-004 | WS publishes vector diagnostics | в бюджете latency F-05 |
| P-F05A-005 | Replay 100 captured snapshots | детерминированно |

## Раздел 6. Fixture quality gate (IN-014 §8.8)

Fixture валиден **только** если: raw response сохранён; metadata сохранена;
`raw_response_sha256` сохранён; venue + endpoint задокументированы; normalizer version
сохранён; asset basis воспроизводим; тест **не** зависит от live API; тест выполняется
**offline**. Live API — только для refresh fixtures, не для CI.

Fixture metadata: `fixture_id`, `venue`, `endpoint`, `capture_time_utc`, `symbol_or_pair`,
`limit_or_count`, `raw_response_sha256`, `schema_version`, `normalization_version`,
`license_notes`.

## Трассировка

- Feature: [F-05A](../../02-system/features/F-05-live-market-data/addendum-F05A-vectorized-external-liquidity.md)
- Use cases: UC-F05A-01..05; Sequences: SEQ-F05A-UC-F05A-01..05-services
- Data: [vector-clearing-results](../../07-data/vector-clearing-results.md), [vector-flow-segments](../../07-data/vector-flow-segments.md)
- Implementation: [F-05A tasks §Phase 5](../../implementation-plan/F-05A-vectorized-external-liquidity.tasks.md)
- Business rules: [§F-05A](../../04-domain/business-rules.md#f-05a--vectorized-external-liquidity)
