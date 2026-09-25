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

#### T-F05A-206. CH persistence сегментов  ✅
AC: AC-F05A-012.
- [x] Персист `vector_flow_segments_history` (`ClickHouseVectorSegmentStorage`).

#### T-F05A-207. Batch-window aggregator (cross-venue / triangular)  ⏳ — [ADR-050](../03-architecture/adr/ADR-050-f05a-batch-window-aggregation.md)
AC: AC-F05A-006 (треугольный кейс клирится), UC-F05A-01.
> **Причина:** текущий `OnLiquidityCurve` векторизует **каждую кривую отдельно** →
> single-venue/single-pair batch → `x≈0` (арбитраж непредставим). Постановка
> (IN-014 §12.1/§12.3, R-F05A-002/004, AC-F05A-006, UC-F05A-01 batch-timer) требует
> агрегировать многие венью/пары в один клиринг над общим asset-basis.
- [ ] **207a. Freshness-буфер + batch-timer** в `market_data`: буфер «последняя кривая
      на `(venue_id, pair)`» (новая вытесняет старую); фоновый таймер `F05A_BATCH_WINDOW_MS`
      (dev 1000ms) с чётким lifecycle + graceful shutdown (§12.2). За флагом
      `F05A_BATCH_WINDOW_ENABLED` (default off → прежнее поканальное поведение).
- [ ] **207b. Оконный vectorize:** собрать уровни всех свежих кривых → **общий**
      `BuildAssetBasis` (объединение активов всех пар) → один `W`; вес свежести
      `f=clamp(1−age/F05A_STALE_LEVEL_MS,0,1)` на `q_max` (`q_max_eff=q_max·f`);
      `age≥порог` → отбросить (метрика `stale_external_levels_total`). Публиковать
      **один** `marketdata.vectorized` с оконным `batch_id`.
- [ ] **207c. `seg_index`:** `ALTER TABLE vector_flow_segments_history ADD COLUMN seg_index UInt32`;
      писать позицию сегмента в векторе (детерминированный порядок `(venue,pair,side,k)`),
      чтобы `x[i]↔segment[i]` восстанавливалось точно при мульти-venue.
- [ ] **207d. Detail-API** переключить выравнивание черновиков с реконструкции по
      `source_order_id` на `seg_index`.
- [ ] **207e. Тесты:** unit (оконный vectorize: 2 венью одной пары с расхождением →
      ненулевой `x`; треугольный AC-F05A-006 → `Wx=0`); детерминизм окна.

#### T-F05A-208. Линейный сегмент ликвидности венью  ⏳ — [ADR-051](../03-architecture/adr/ADR-051-f05a-linear-venue-segment.md)
AC: ликвидность стороны венью = один линейный сегмент с реальным наклоном.
> **Причина:** `curve_to_levels` делал сегмент на каждую точку `q_grid` (135–300/сторону),
> а `d_hl = dhl_fraction·P_eff` (фикс. 1 %) — не реальный наклон. Постановка (владелец):
> ликвидность = линейная функция `p(q)=a+b·q`.
- [x] Поле `ExternalOrderLevel.d_hl_override` (0 = policy); `Vectorize` использует его.
- [x] `LinearSegmentsFromCurve`: сторона → 1 уровень: anchor `a=p_of_q[0]`, `q_max=q_grid[last]`,
      `d_hl_override=|p_of_q[last]−p_of_q[0]|` (= `|b|·q_max` ⇒ `D=|b|`).
- [x] Выбор Linear vs per-level в `OnLiquidityCurve` и `FlushVectorWindow` по флагу
      `F05A_LINEAR_SEGMENTS_ENABLED` (default off; on в dev). Вес свежести масштабирует
      и `d_hl_override` (наклон инвариантен).
- [x] Тест `TestLinearSegments` (vectorize_wiring_test).
- Поправка R-F05A-002: provenance с per-level на per-(venue,pair,side) при модели A.

#### T-F05A-209. Двусторонний знаковый сегмент венью (academic)  ✅ — [ADR-052](../03-architecture/adr/ADR-052-f05a-two-sided-signed-segment.md)
AC: одна двусторонняя кривая/венью, знаковый `x` (покупка/продажа), кросс-venue арбитраж → ненулевой `x`.
> Каноника постановки (academic §5.1.1, `entities.md:148-150`): венью = одна кривая
> `p(q̇)=m·q̇+a`, `q̇` знаковая. Заменяет per-side (ADR-051).
- [x] proto `VectorFlowSegment` += `q_min`, `anchor`, `slope`.
- [x] matching `AssembleProblemTwoSided` (P=diag(m), q=+a, знаковый box) + флаг
      `F05A_TWO_SIDED_ENABLED`; closed-form тест (`x_i=(p*−a_i)/m_i`).
- [x] market_data `TwoSidedSegmentsFromCurves`: 1 сегмент/венью (w=e_base−e_quote,
      anchor=mid, slope=avg, `q_max=+Q_ask`, `q_min=−Q_bid`).
- [x] detail-API: курс = `pi[quote]−pi[base]` (дуальная p*); сторона черновика по знаку x.
- [x] **Проверено live:** 3 венью → `x=[+1.33,+0.61,−1.94]` (Σ=0), курс 78211, 3 черновика
      (2 BUY + 1 SELL, балансируются).
- Остаток: status часто degraded (tol на 3 сегм. — косметика); staleness иногда роняет венью.

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
- [x] **Money-path MVP — hedge (PR #43, ADR-049):** решено — сошедшийся external-клиринг = биржевой хедж (F-12). `vector_clearing_hedge_builder`: сегмент `x_i>0` → `ExecutionIntent` (venue/side против external level, `target_qty=x_i`, `limit_price=|w[quote]|`) → `execution.intents` → venues → ledger `venue_balances_`/hedge-PnL. За флагом **`F05A_MONEY_ENABLED` (default OFF)**, только `kProceedNoSurplus`. `intent_id=batch|segment` (идемпотентно). НЕ user-проводки. Тест `matching_vector_clearing_hedge_builder_test`.
- [ ] **Дальнейшее (по решению владельца):** user-проводки через F-09 `ExecutionGroup` (нужна привязка user/parent + `LegResult` source-trace T-F05A-106); `EXCHANGE_PNL` surplus (нужен house-счёт в ledger, T-F05A-401); persistent-идемпотентность на F-05A execution-path.

#### T-F05A-306. Клиринговые цены (pi) — дуальные OSQP  ✅
AC: результат клиринга содержит равновесные цены по активам.
- [x] `pi` = первые `N` дуальных OSQP по ограничению `Wx=0` (тень актива = равновесная цена). `OsqpBackend` копирует `work->solution->y`; `VectorQpSolver` извлекает первые `N` и квантует (граница §9); `ToVectorClearingResult` пробрасывает в proto-поле `VectorClearingResult.pi` (уже существовало, теперь наполняется). `market_data` персистит `pi_json` без изменений. Тесты: извлечение pi (fake + реальный OSQP e2e) + маппер.
- Семантика: абсолютные `pi` определены с точностью до нормировки (Wx=0 однороден; D-регуляризатор фиксирует масштаб). **Клиринговая цена инструмента = `pi[base]/pi[quote]`** (стабильна, ≈ рыночная): проверено live — BTC/USDT ≈ 79 300 при разных абсолютных `pi`.
- НЕ breaking (наполняем существующее контракт-поле, ADR не требуется).

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

#### T-F05A-607. Clearing detail (клик по строке)  ✅
AC: по клику строки клиринга видны исходные заявки + клиринговые цены + черновики хеджа.
- [x] Ops-страница `Clearing` (`/vector-clearing-live`): клик по строке раскрывает деталь через `GET /api/vector-clearing/detail?batch_id&ts` (frontend-api, read-only ClickHouse). Три секции:
  - **(1) Исходные заявки** — `vector_flow_segments_history` (инструмент `pair`, биржа `venue_id`, сторона, скорость `q_rate`, цена `effective_price`, `q_max`), привязка к циклу по `batch_id` + ближайшей группе `event_time_ms`.
  - **(2) Клиринговые цены** — `pi_json` (T-F05A-306) с метками активов (asset-basis восстановлен как отсортированное объединение `pair`); заголовок — клиринговый курс `pi[base]/pi[quote]`.
  - **(3) Черновики хеджа** — вычисляются read-time из сегментов + `x_json` (та же логика `vector_clearing_hedge_builder`: `x_i>0` → venue/side/qty/limit). Выравнивание `x[i]↔сегмент[i]` восстановлено по порядку сборки vectorize (bid k=0..n, затем ask k=0..n; k числовой), т.к. `batch` single-venue.
- Ограничение: multi-venue цикл потребует порядка венью солвера (или `seg_index` в persist) — для текущих single-venue данных корректно.

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
