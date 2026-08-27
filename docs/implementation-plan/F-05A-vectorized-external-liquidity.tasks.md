# Implementation Tasks: F-05A Vectorized External Liquidity for F-09 Batch/Combo Clearing

## Source Artifacts

- Source: **IN-014** — [`incoming-docs/2026-07-07-F-05A-vectorized-external-liquidity-v1.md`](../../incoming-docs/2026-07-07-F-05A-vectorized-external-liquidity-v1.md), [meta](../../incoming-docs/IN-014.meta.md), [fragment-map](../../incoming-docs/IN-014.fragment-map.md)
- Base feature: [F-05 Live Market Data](../02-system/features/F-05-live-market-data/) (primary owner)
- Consumer feature: [F-09 Batch/Combo Orders](../02-system/features/F-09-batch-combo-orders/) (`multileg_vector_solver`)
- Reused infra: [F-11 External Venues](../02-system/features/F-11-external-venues-lob-to-fob/) (venue normalization `venue.liquidity.fob`), [F-15 Backtest/Replay](../02-system/features/F-15-backtest-replay/)
- ADRs: [ADR-048 QP solver backend (OSQP)](../03-architecture/adr/ADR-048-qp-solver-backend.md) — **accepted 2026-08-25**; [ADR-047 surplus/EXCHANGE_PNL](../03-architecture/adr/ADR-047-surplus-exchange-pnl-policy.md) — **accepted 2026-08-26** (money-path разблокирован); extends [ADR-034](../03-architecture/adr/ADR-034-grouped-constraint-solver.md), math [ADR-035](../03-architecture/adr/ADR-035-fob-solver-mathematical-foundation.md)
- Math foundation: IN-012 continuous-order market (clearing as Lagrange multiplier), business-rules **R-CLR-003** (flow conservation)

## Preconditions (docs-as-code gate)

> **Docs-gate почти снят** (ingress-close IN-014 завершён 2026-07-23). **ADR-048 (OSQP)
> принят 2026-08-25** — QP-путь (Phase 3) разблокирован. **ADR-047 (surplus) принят
> 2026-08-26** (prod-default `REJECT_IF_RESIDUAL`) — money-path (T-F05A-304/305) разблокирован.

- [x] `F-05A-feature.yaml` + README (addendum к F-05)
- [x] Use cases `UC-F05A-01..05` + L0 system sequences
- [x] L1 service sequences `SEQ-F05A-UC-F05A-*-services.md`
- [x] Contracts: `vector_liquidity.proto` (компилируется) + `docs/06-api/{grpc,messaging}/*` + proto-map
- [x] Data schemas: PG (`asset_basis`, `vector_flow_segments`, `vectorization_runs`) + CH (`vector_clearing_results`, `surplus_events`, `vector_flow_segments_history`)
- [x] Test plan `docs/10-testing/features/F-05A-test-plan.md`
- [x] Acceptance criteria (feature.yaml) + business rules §F-05A (R-F05A-001..007) + FR/NFR-F05A
- [x] **ADR-048 → `accepted`** (2026-08-25, подтверждение OSQP) — QP-путь разблокирован
- [x] **ADR-047 → `accepted`** (2026-08-26, surplus prod-default `REJECT_IF_RESIDUAL`) — money-path разблокирован

## Architecture decisions pending (решение владельца)

| # | Вопрос | ADR / Conflict Note |
| --- | --- | --- |
| D1 | Placement: addendum к F-05 vs F-09 vs отдельная фича | CN-IN014-01 (рекоменд. F-05) |
| D2 | Вход векторизации: `marketdata.raw` vs F-11 `venue.liquidity.fob` | CN-IN014-03 (рекоменд. F-11) |
| D3 | QP backend | [ADR-048](../03-architecture/adr/ADR-048-qp-solver-backend.md) (OSQP) |
| D4 | Surplus prod-default | [ADR-047](../03-architecture/adr/ADR-047-surplus-exchange-pnl-policy.md) (MVP `REJECT_IF_RESIDUAL`) |
| D5 | Объём UI в первой итерации | CN-IN014-04 (MVP: W/x/Wx/source-trace) |

## Critical path

`Phase 0 (docs+ADR accept)` → `Phase 1 (contracts+data)` → **`Phase 3 (OSQP QP solver)`** → `Phase 2↔3 integration` → `Phase 4 (ledger surplus)` → `Phase 5 (fixtures+tests)`. QP-solver (Phase 3) — самый длинный полюс и главный риск.

---

## Tasks

### Phase 0 — Docs & ADR (ingress-close IN-014, no code)

#### T-F05A-001. Feature docs + YAML
- `docs/02-system/features/F-05-live-market-data/F-05A-feature.yaml` + `addendum-F05A-vectorized-external-liquidity.md` (skill `create-feature`). Source: IN-014 §1,§19. Owner: system-analyst.

#### T-F05A-002. Use cases + sequences
- `UC-F05A-01..05` (`docs/02-system/use-cases/UC-F05A-*/use-case.md`) + L0 `SEQ-UC-F05A-*-system.md` + L1 `docs/05-components/sequences/SEQ-F05A-UC-F05A-*-services.md` (mermaid из IN-014 §15). Owner: system-analyst.

#### T-F05A-003. FR/NFR + domain entities + business rules
- FR-F05A-001..011 → `functional-requirements.md`; NFR-F05A-001..005 → `non-functional-requirements.md`; entities (ExternalOrderLevel/AssetBasis/VectorFlowSegment/VectorClearingInput/Result) → `docs/04-domain/entities.md`; math (bid/ask w_i, D_i, Wx=0) → `business-rules.md §F-05A`. Owner: trading-domain-specialist.

#### T-F05A-004. ADR → accepted (частично)

- [x] [ADR-048](../03-architecture/adr/ADR-048-qp-solver-backend.md) (QP/OSQP, D3) → `accepted` **2026-08-25**.
- [x] [ADR-047](../03-architecture/adr/ADR-047-surplus-exchange-pnl-policy.md) (surplus prod-default, D4) → `accepted` **2026-08-26**.

#### T-F05A-005. Test plan + coverage row
- `docs/10-testing/features/F-05A-test-plan.md` (incl. real-orderbook fixtures policy) + строка F-05A в `docs/traceability/coverage-matrix.md`. Owner: test-architect.

---

### Phase 1 — Контракты и схемы данных

#### T-F05A-101. `vector_liquidity.proto`
AC: AC-F05A-003, AC-F05A-004.
- `contracts/proto/fob/marketdata/v1/vector_liquidity.proto`: `ExternalOrderLevel`, `AssetBasis`, `VectorFlowSegment`, `VectorClearingInput`, `VectorClearingResult`, `SurplusEvent`. **Все денежные/количественные поля — `fob.common.v1.Decimal`** (§9).
- Регистрация в `specs/contracts/proto-map.yaml` (иначе `spec-validation` красный — урок F-09) + `docs/06-api/grpc/marketdata-get-vectorized-liquidity.md`.
- Зависимости: T-F05A-001. Owner: proto-contract-designer.

#### T-F05A-102. Kafka topic `marketdata.vectorized`
AC: AC-F05A-009.
- `infra/kafka/create_topics.sh` + `docs/06-api/messaging/marketdata-vectorized.md` (skill `register-kafka-topic`). Key: `symbol`/`batch_id`. Producers: market-data. Consumers: matching, backtest, observability.

#### T-F05A-103. REST/WS contract
- `docs/06-api/rest/marketdata-vector-liquidity.md` (endpoints §7.3/§10.3) + WS `/api/market/vector`. Owner: proto-contract-designer.

#### T-F05A-104. PostgreSQL schemas
AC: AC-F05A-012.
- `infra/postgres/init.sql`: `asset_basis`, `vector_flow_segments`, `vectorization_runs`, (`venue_order_levels`) + docs `docs/07-data/*` (skill `register-pg-table`). NUMERIC для денег.

#### T-F05A-105. ClickHouse schemas
AC: AC-F05A-012.
- `infra/clickhouse/init.sql`: `vector_clearing_results`, `surplus_events`, `vector_flow_segments_history` + docs. `Decimal128(18)` для денег (как grouped_* в olap-schema). Зеркалить в `EnsureSchema` market_data (паттерн F-09 grouped_*).

#### T-F05A-106. ExecutionGroup vector source-trace
AC: AC-F05A-010.
- Расширить `execution_group.proto` `LegResult` полем vector source-trace (venue_id/source_order_id/segment_id/w-ref) — **backward-compatible** (как T-F09-062). ADR если breaking. Owner: proto-contract-designer.

---

### Phase 2 — Векторизация (market_data)

> **Прогресс (2026-08-26):** доменное ядро + wiring в `main` (PR #30/#31/#32).
> Векторизация подключена к живому consumer `venue.liquidity.fob` и публикует
> `marketdata.vectorized`. Тесты `market_data_vectorize_test` +
> `market_data_vectorize_wiring_test` — Passed. Остаётся T-F05A-206 (CH-persist).

#### T-F05A-201. Domain value-objects  ✅ (PR #30)
AC: AC-F05A-001, AC-F05A-002.
- [x] `cpp/market_data/src/domain/{external_order_level,asset_basis,vector_flow_segment}.hpp` — чистые VO, `Decimal`.

#### T-F05A-202. Bid/Ask → w_i mapping  ✅ (PR #30)
AC: AC-F05A-002. (U-F05A-001/002)
- [x] `cpp/market_data/src/domain/vectorize.cpp`: bid `w=e_X−P_eff·e_Y`, ask `w=−e_X+P_eff·e_Y`. Unit-tested.

#### T-F05A-203. Effective price  ✅ (PR #30)
AC: (U-F05A-003)
- [x] `effective_price.hpp`: fees/latency/slippage buffers → `P_eff` (double = solver input, §9).

#### T-F05A-204. Dimensional guard (KI-F05A-003)  ⏳
- `cpp/market_data/src/domain/dimensional_guard.*` — валидация единиц (base/quote/price/rate/batch). **Высокий риск корректности** → отдельный слой + unit-тесты. (Частично: `Vectorize` уже skip'ает invalid_pair/asset_not_in_basis/non_positive_quantity; отдельный guard-слой — TODO.)

#### T-F05A-205. Vectorize use case + publish  ✅ (PR #31/#32)
AC: AC-F05A-009.
- [x] `app/curve_to_levels` (VenueLiquidityCurve → уровни) + `transport/mappers/vectorized_liquidity` (domain → proto); `MarketDataUseCases::OnLiquidityCurve` → `Vectorize` → publish `marketdata.vectorized` через `KafkaVectorizedProducer` (вход = F-11 `venue.liquidity.fob`, D2). Пропуск невалидных.
- [ ] staleness-фильтр по `ts_event` (KI-F05A-004) — TODO (нужен clock/порог).

#### T-F05A-206. CH persistence сегментов  ⏳ next
AC: AC-F05A-012.
- Персист `vector_flow_segments_history` (переиспользовать паттерн `ClickHouseBatchStorage::SaveExecutionGroup`, F-09).

---

### Phase 3 — QP solver (matching) — **критический путь** ([ADR-048](../03-architecture/adr/ADR-048-qp-solver-backend.md))

> **Прогресс (2026-08-25/26):** QP-солвер F-05A **полностью реализован** в `main` —
> доменное ядро (PR #24) + реальный OSQP-backend (PR #26). Оба с зелёными тестами
> (`matching_vector_qp_solver_test`, `matching_osqp_backend_test` — Passed).
> Осталось: T-F05A-304 (surplus policy — ADR-047 принят) и T-F05A-305 (интеграция в batch loop).

#### T-F05A-301. Подключить OSQP  ✅ (PR #26)
- [x] `OsqpBackend : IQpBackend` (`cpp/matching/src/infra/osqp_backend.{hpp,cpp}`): Eigen standard-form → CSC → `osqp_setup/solve`; детерм. settings (adaptive_rho=0, fixed max_iter/tol, no warm-start).
- [x] OSQP через FetchContent (pinned v0.6.3, static) в matching CMake; синхронно помечены `docker/Dockerfile.service` и `.github/workflows/cpp-build.yml` (ADR-048 §7 — OSQP из исходников, apt-пакет не нужен).
- [x] тест `matching_osqp_backend_test`: known-solution QP (interior/box/equality) + end-to-end `VectorQpSolver(OsqpBackend)`.

#### T-F05A-302. `vector_qp_solver`  ✅ domain core (PR #24)
AC: AC-F05A-004, AC-F05A-005. (U-F05A-005/006/007/008)
- [x] `cpp/matching/src/domain/vector_qp_solver.{hpp,cpp}` за `IVectorClearingSolver` (Eigen-only). Отображение в OSQP: `P=D, q=−pH, A=[W;I], l=[0;0], u=[0;q]`. **Детерминированный режим** (fixed max_iter, no adaptive-rho). `double→Decimal` квантование на выходе (§9).
- [ ] реальный solve — через `OsqpBackend` (T-F05A-301).

#### T-F05A-303. Residual + diagnostics  ✅ (PR #24)
AC: AC-F05A-004.
- [x] `r=Wx`, `residualNorm`, solver_status (converged/degraded/failed), iterations. `solveTimeMs` — при интеграции OSQP (T-F05A-301).

#### T-F05A-304. Surplus policy ([ADR-047](../03-architecture/adr/ADR-047-surplus-exchange-pnl-policy.md))  ✅ (PR #29)
AC: AC-F05A-005, AC-F05A-007. (U-F05A-009)
- [x] `cpp/matching/src/domain/surplus_policy.hpp` (`REJECT_IF_RESIDUAL` default) — `DecideSurplus`. Эмит `SurplusEvent` — при интеграции (T-F05A-305 money-path).

#### T-F05A-305. Интеграция в batch loop
AC: AC-F05A-008, AC-F05A-010.
- [x] **Оркестрация (safe slice, PR #34):** `app/vector_clearing_use_case` — proto `VectorClearingInput` → `domain::VectorSegment[]` → `VectorQpSolver(OsqpBackend)` → `surplus_policy.DecideSurplus`; тест end-to-end (proto→OSQP→surplus). БЕЗ эмиссии денег.
- [x] **Consumer-wiring (1a, PR #36/#37):** решено — **Kafka-событие** (matching не пишет CH). `MatchingLoop` подписан на `marketdata.vectorized` → `VectorClearingUseCase.Clear` → `ToVectorClearingResult` → publish `matching.vector_clearing` (топик + `docs/06-api/messaging/matching-vector-clearing.md`) + структурный лог. try/catch, БЕЗ денег. Пайплайн F-05A замкнут живьём end-to-end.
- [x] **Persister диагностики (PR #40):** market_data потребляет `matching.vector_clearing` → ClickHouse `vector_clearing_results` (`ClickHouseVectorClearingStorage` + `MarketDataUseCases::OnVectorClearingResult`). Диагностический контур F-05A замкнут end-to-end с персистом.
- [ ] **Money-path:** эмит `ExecutionGroup`+`FillEvent` c source-trace (переиспользовать `execution.groups` + grouped batch F-09), `SurplusEvent`, ledger-проводки. **Трогает деньги — отдельный явный шаг** (converged-only, feature-flag off; EXCHANGE_PNL требует house-счёта в ledger).

---

### Phase 4 — Ledger / Risk / Observability

#### T-F05A-401. Ledger: balanced apply + surplus posting
AC: AC-F05A-005. (money §9/§17)
- `cpp/ledger/src/app/ledger_uc.*`: применить сбалансированный `ExecutionGroup` без phantom inventory; при `EXCHANGE_PNL` — house-account posting (идемпотентно по `execution_group_id`, before/after snapshot).

#### T-F05A-402. Risk alerts
- surplus/residual выше порога → `risk.alerts`.

#### T-F05A-403. Observability metrics
AC: AC-F05A-014.
- `vector_segments_total`, `vector_solver_residual_norm`, `vector_solver_solve_time_ms`, `surplus_events_total`, `stale_external_levels_total` + alerts. Расширить market_data CH-ingestion (паттерн grouped_*, F-09).

---

### Phase 5 — Fixtures & Testing

#### T-F05A-501. Real-orderbook fixture pipeline
AC: AC-F05A-006. (§8.2/§8.3/§8.8)
- Формат + capture-скрипты (Binance `/api/v3/depth`, Coinbase product book, Kraken `/Depth`); immutable + `raw_response_sha256` + metadata; `tests/fixtures/real-orderbooks/`. **CI offline** (без live API).

#### T-F05A-502. Unit tests
- U-F05A-001..010 (bid/ask, W shape, D=diag(dHL/q), 0≤x≤q, Wx=0 triangle, surplus, source mapping, dimensional-guard).

#### T-F05A-503. Integration tests
- I-F05A-001..010: real fixtures → W; `marketdata.vectorized → matching → execution.groups`; CH `vector_clearing_results`.

#### T-F05A-504. Replay determinism (F-15)
AC: AC-F05A-011. (R-F05A-001)
- ⚠️ Зависит от grouped/vector пути в `cpp/backtest` (тот же незакрытый разрыв AC-F09-010, CN-IN014-05) — закрыть здесь.

#### T-F05A-505. Performance + CI gate
- P-F05A-001..005 (1k/10k levels p95); CI не зависит от live API.

---

### Phase 6 — Frontend (Vector Liquidity Explorer) — отдельный под-проект (D5)

#### T-F05A-601. MVP charts
AC: AC-F05A-UI-003/004/005/007.
- W heatmap, execution vector `x`, residual `Wx`, source-trace (клик segment→venue level). Остальные экраны (raw depth, clearing graph, surplus/PnL, replay) — T-F05A-602..606, по мере.

---

### Phase 7 — F-15 backtest/replay для vector clearing

#### T-F05A-701. Grouped/vector replay в backtest
AC: AC-F05A-011.
- Закрыть разрыв AC-F09-010: `cpp/backtest` реплеит grouped combo + vector clearing детерминированно (синергия с остатком F-09).

---

## Known risks (из IN-014)

| Риск | Severity | Митигизация | Task |
| --- | --- | --- | --- |
| Корректность/детерминизм QP (OSQP) | high | fixed OSQP params; replay-тест как гейт | T-F05A-302/504 |
| Размерности единиц (KI-F05A-003) | high | `dimensional_guard` + unit-тесты | T-F05A-204/502 |
| Деньги/surplus (§9/§17) | high | ADR-047 + идемпотентный ledger; double-apply тесты | T-F05A-304/401 |
| Staleness внешних levels (KI-F05A-004) | major | freshness score + snapshot ts + latency buffer | T-F05A-205 |
| CI-инфра (новый OSQP dep) | major | Dockerfile.service + cpp-build.yml синхронно | T-F05A-301 |
| Combo/vector replay в backtest | major | закрыть вместе с AC-F09-010 | T-F05A-701 |

## Definition of Done

Feature `implemented` только при выполнении всех DoD-групп из IN-014 §10
(Documentation / Contract / Data / Backend / UI / Testing / Observability) +
ADR-048 `accepted` (✅ 2026-08-25) + ADR-047 `accepted` (✅ 2026-08-26) + все AC-F05A-001..015 покрыты тестами + coverage-matrix ✅.
