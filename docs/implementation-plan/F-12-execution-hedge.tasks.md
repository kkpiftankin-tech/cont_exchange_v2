# Implementation Tasks: F-12 Execution Hedge

## Source Artifacts

- Feature: [F-12 Execution Hedge](../02-system/features/F-12-execution-hedge/)
- Use Cases: [UC-F12-01..05](../02-system/use-cases/) (auto-hedge, manual, partial-retry, rejection-fallback, timeout)
- System Sequences: 5 шт. (`SEQ-UC-F12-NN-system.md` под каждой use case)
- Service Sequences: [SEQ-F12-01-auto-hedge-services](../05-components/sequences/SEQ-F12-01-auto-hedge-services.md), [SEQ-F12-02-rejection-fallback-services](../05-components/sequences/SEQ-F12-02-rejection-fallback-services.md), [SEQ-F12-03-error-scenarios-services](../05-components/sequences/SEQ-F12-03-error-scenarios-services.md)
- Contracts: [execution.intents](../06-api/messaging/execution-intents.md), [execution.venue](../06-api/messaging/execution-venue.md), [REST hedgeflows](../06-api/rest/hedgeflows.md), [gRPC PreHedgeCheck](../06-api/grpc/risk-pre-hedge-check.md)
- Proto: [`fob.execution.v1`](../../contracts/proto/fob/execution/v1/execution.proto), [`fob.hedge.v1`](../../contracts/proto/fob/hedge/v1/hedge.proto), [`fob.venue.v1`](../../contracts/proto/fob/venue/v1/venue.proto)
- OpenAPI: [`contracts/openapi/fob/hedge/v1/api/hedge.yaml`](../../contracts/openapi/fob/hedge/v1/api/hedge.yaml)
- Data: [hedgeflows](../07-data/hedgeflows.md), [child_orders](../07-data/child-orders.md), [execution_reports](../07-data/execution-reports.md)
- Business rules: [04-domain/business-rules.md → F-12](../04-domain/business-rules.md#f-12-execution-hedge)
- Test plan: [10-testing/features/F-12-test-plan.md](../10-testing/features/F-12-test-plan.md)
- Source v1: IN-005 [`incoming-docs/2026-05-20-F-12-execution-hedge-v1.md`](../../incoming-docs/2026-05-20-F-12-execution-hedge-v1.md) (superseded)
- Source v2: IN-008 [`incoming-docs/IN-008.meta.md`](../../incoming-docs/IN-008.meta.md) (superseded by IN-009) — добавил 18-item Definition of Done и 6 UX экранов
- Source v2.0-final: IN-009 [`incoming-docs/IN-009.meta.md`](../../incoming-docs/IN-009.meta.md) — добавил Observability Reporting (read-side role), refresh modes per UI screen, operator timeline (10 steps), architecture mini-diagram. См. `feature.yaml#architecture`, `feature.yaml#operatorTimeline`, `feature.yaml#uxRefreshModes`

## Preconditions (docs-as-code gate)

- [x] Feature exists ([F-12-execution-hedge/](../02-system/features/F-12-execution-hedge/))
- [x] Use cases exist (5 шт.)
- [x] System sequences exist
- [x] Service sequences exist (3 шт.)
- [x] Contracts exist (Kafka, REST, gRPC, proto)
- [x] Data objects exist (hedgeflows, child_orders, execution_reports — DDL spec)
- [x] Acceptance criteria exist
- [x] Business rules / формулы расчётов

Все docs-первые артефакты на месте; код может быть имплементирован.

## Architecture decisions pending

См. [ADR-012](../03-architecture/adr/ADR-012-venue-execution-adapter-decomposition.md) — выделять ли `venue-execution-adapter` в отдельный сервис.
См. [ADR-013](../03-architecture/adr/ADR-013-execution-planning-placement.md) — `execution-planning` как сервис vs library.

## Tasks

### Phase 1 — Proto и контракты

#### T-F12-101. Расширить risk.proto методом PreHedgeCheck

AC: F12-8.

- Файл: [`contracts/proto/fob/risk/v1/risk.proto`](../../contracts/proto/fob/risk/v1/risk.proto).
- Добавить messages `PreHedgeCheckRequest`, `PreHedgeCheckResponse`, `PreHedgeCheckDetails` (см. [06-api/grpc/risk-pre-hedge-check.md](../06-api/grpc/risk-pre-hedge-check.md)).
- Добавить `rpc PreHedgeCheck(PreHedgeCheckRequest) returns (PreHedgeCheckResponse);` в `RiskService`.
- Пересборка `contracts_proto`; проверка backward compat (новый метод — добавление, не breaking change).

Зависимости: нет.

#### T-F12-102. (Опционально) hedge_policy_config в risk.proto

AC: F12-1, F12-3, F12-5.

- Добавить `HedgePolicyConfig` message с полями `thresholdQty[symbol]`, `thresholdNotional`, `maxNotionalPerHedge`, `hedgeExposureLimit[symbol]`, `maxSlippage[urgency]`, `overfillThreshold`, `reconciliationGapThreshold`, `hedgeTimeoutMs[urgency]`.
- Pa-альтернатива: хранить как JSONB в PostgreSQL `hedge_policy_config` без proto.

#### T-F12-103. Канонизировать топик execution.venue

AC: F12-6.

- Открытая задача: deprecation legacy `execution.reports` (см. Conflict Note в `feature.yaml`).
- Обновить [`cpp/venues/src/infra/execution_report_producer.cpp`](../../cpp/venues/src/infra/execution_report_producer.cpp): switch на `execution.venue`.
- Обновить consumers (Ledger, ClickHouse ingestion).
- Период dual-publish для backward compat.
- Удалить `execution.reports` topic из `infra/kafka/create_topics.sh` после migration window.

### Phase 2 — PostgreSQL persistence

#### T-F12-201. DDL hedgeflows

AC: F12-7.

- Добавить DDL из [07-data/hedgeflows.md](../07-data/hedgeflows.md) в [`infra/postgres/init.sql`](../../infra/postgres/init.sql).
- Проверить migration на свежем compose-стенде.

#### T-F12-202. DDL child_orders

AC: F12-7.

- Добавить DDL из [07-data/child-orders.md](../07-data/child-orders.md) в `infra/postgres/init.sql`.
- Foreign key к `hedgeflows`, индексы.

#### T-F12-203. ClickHouse DDL execution_reports

AC: F12-7, F12-6.

- Добавить DDL из [07-data/execution-reports.md](../07-data/execution-reports.md) в [`infra/clickhouse/init.sql`](../../infra/clickhouse/init.sql).
- Kafka engine table + materialized view → MergeTree.
- Тест: 100 ExecutionReport в Kafka → 100 строк в `execution_reports` за ≤ 5 s.

### Phase 3 — Venue Execution Adapter (persistence + state machine)

#### T-F12-301. PostgresHedgeflowRepository

AC: F12-7.

- Новый файл `cpp/venues/src/infra/postgres_hedgeflow_repository.{hpp,cpp}`.
- Методы: `Insert(HedgeFlow)`, `UpdateAggregates(hedge_flow_id, filled_qty, avg_fill_price, hedge_pnl, tot_fee)`, `UpdateStatus(hedge_flow_id, status, completed_at, error)`.
- Connection через `libpqxx` (как в F-04 PostgresFlowOrderRepository).

#### T-F12-302. PostgresChildOrderRepository

AC: F12-7.

- Новый файл `cpp/venues/src/infra/postgres_child_order_repository.{hpp,cpp}`.
- Методы: `Insert(ChildOrder)`, `UpdateOnAck(child_order_id, venue_order_id)`, `UpdateOnFill(child_order_id, filled_qty, avg_price, fee, status)`, `UpdateOnCancel(...)`.

#### T-F12-303. State machine в ExecuteOnVenue

AC: F12-7, F12-4.

- Расширить [`cpp/venues/src/app/execute_on_venue.cpp`](../../cpp/venues/src/app/execute_on_venue.cpp):
  - INSERT hedgeflows + child_orders before placing.
  - UPDATE child_orders на каждое venue event.
  - Aggregate update в hedgeflows.
  - Overfill detection (F12-4): при `accumulated > target + threshold` → CancelAllOpenOrders + publish ExecutionReport(OVERFILL_GUARD).

#### T-F12-304. Timeout Watchdog

AC: F12-5.

- Background thread в `cpp/venues/src/app/timeout_watchdog.{hpp,cpp}`.
- Каждые N ms запрос: `SELECT hedge_flow_id, timeout_ms, created_at FROM hedgeflows WHERE status='OPEN' AND now() - created_at > timeout_ms * interval '1 ms'`.
- Для каждого выявленного HedgeFlow: CancelAllOpenOrders + publish ExecutionReport(CANCELLED, reason=timeout) + reconciliation.

#### T-F12-305. Reconciliation Loop

AC: F12-5.

- В `ExecuteOnVenue` после каждого ExecutionReport или после timeout:
  - Compute `gap = targetQty - filledQty`.
  - Если `gap ≤ reconciliationGapThreshold` → status=COMPLETED.
  - Если `gap > threshold` и `policy.auto_retry_on_underfill` → publish retry ExecutionIntent с urgency=HIGH.
  - Иначе → status=UNDERFILLED + risk.alerts(HEDGE_UNDERFILL).

### Phase 4 — Execution Planning

#### T-F12-401. Execution Planning service skeleton

AC: F12-3, F12-12.

- Реализовать `IExecutionPlanningUseCases` в новом классе `cpp/matching/src/app/execution_planning_impl.{hpp,cpp}` (или вынести в отдельный модуль).
- Consume `execution.intents`, `venue.liquidity.fob`, `venue.health`.
- Internal state: latest VenueLiquidityCurve + venue.health per (venue, symbol).

#### T-F12-402. Routing plan algorithm

AC: F12-3.

- Реализовать формулу `qty[v] = L(v) / Sum L(v') * targetQty` (см. [04-domain/business-rules.md#routing-plan](../04-domain/business-rules.md#routing-plan)).
- Учесть lot rounding и maxOrderSize per (venue, instrument).
- Покрытие unit-тестом U9.

#### T-F12-403. PreHedgeCheck gRPC client

AC: F12-8.

- gRPC stub в Execution Planning к `RiskService.PreHedgeCheck`.
- При REJECT → publish risk.alert(HEDGE_REJECTED) + skip routing + signal Adapter `RISK_REJECTED`.

#### T-F12-404. Fallback routing

AC: F12-12.

- Метод `IExecutionPlanningUseCases::RequestFallback(intent_id, excluded_venues)`.
- Исключить venue, опционально повысить urgency.
- Покрытие unit-тестом U4.

### Phase 5 — Risk Manager extension

#### T-F12-501. RiskService PreHedgeCheck реализация

AC: F12-8.

- В `cpp/risk/src/transport/grpc_risk_service.cpp` (или эквивалент) добавить handler `PreHedgeCheck`.
- 3 проверки: notional, exposure, slippage (per [04-domain/business-rules.md](../04-domain/business-rules.md)).
- Использовать `risk_limits` + `risk_snapshots` (PostgreSQL).

### Phase 6 — Ledger HedgePnL

#### T-F12-601. ApplyExecutionReport считает HedgePnL

AC: F12-11.

- В [`cpp/ledger/src/app/ledger_uc.cpp`](../../cpp/ledger/src/app/ledger_uc.cpp): расширить `ApplyExecutionReport`:
  - UPDATE positions (provider hedge account).
  - Compute HedgePnL по формуле (см. [04-domain/business-rules.md#hedgepnl](../04-domain/business-rules.md#hedgepnl)).
  - Atomic transaction: positions + hedgeflows.hedge_pnl update.

### Phase 7 — REST API HedgeFlow Monitor

#### T-F12-701. REST endpoints в gateway

AC: F12-11.

- Реализовать endpoints из [06-api/rest/hedgeflows.md](../06-api/rest/hedgeflows.md):
  - GET `/hedge/flows`, GET `/hedge/flows/{id}`, GET `/hedge/flows/{id}/child-orders`, GET `/hedge/flows/{id}/execution-reports`.
  - POST `/hedge/intents/manual` → publish ExecutionIntent в Kafka.
  - POST `/hedge/flows/{id}/cancel`, POST `/hedge/flows/{id}/retry`.
- RBAC: `operator` / `admin` для POST endpoints, `provider` read-only.

### Phase 8 — Observability

#### T-F12-801. Metrics + alerts

AC: F12-10, F12-N1..N8.

- Metrics: `hedge_intent_latency_ms`, `hedge_first_child_order_latency_ms` (p50/p95/p99), `hedge_fill_rate`, `hedge_slippage_bps`, `hedge_cancel_rate`, `hedge_overfill_count`, `hedge_underfill_count`.
- Alerts:
  - p95 latency > 100 ms (CEX) или > 1000 ms (DEX) 5 минут подряд → WARN.
  - fill_rate < 95% для urgency=HIGH 1 час → CRITICAL.
  - slippage_bps > maxSlippage[urgency] → WARN.
  - hedge_underfill_count > 0 в течение 5 минут → WARN.

### Phase 9 — Tests

#### T-F12-901. Unit U1-U10

См. [10-testing/features/F-12-test-plan.md §1](../10-testing/features/F-12-test-plan.md#1-юнит-тесты-u1-u10).

#### T-F12-902. Integration IT-1..IT-6

См. [10-testing/features/F-12-test-plan.md §2](../10-testing/features/F-12-test-plan.md#2-integration-тесты-it-1it-7). IT-7 (backtest parity) уже ✅.

### Phase 10 — UI

#### T-F12-1001. HedgeFlow Monitor screen

#### T-F12-1002. Execution Live Feed

#### T-F12-1003. Hedge PnL Dashboard

#### T-F12-1004. Reconciliation Alerts

#### T-F12-1005. Manual Override form

#### T-F12-1006. Policy Config screen

Все per IN-005 §9 «UI экраны». Frontend scope.

## Out of scope (для F-12)

- Multi-leg / portfolio hedge (F-09).
- AMM-specific gas pricing / MEV protection.
- Cross-venue arbitrage execution.
- DEX bridging / cross-chain settlement.

## Маппинг AC → задача

| AC | Closing tasks |
| --- | --- |
| F12-1 | (matching code uses HedgeTriggerPolicy — wiring в RunBatch) |
| F12-2 | (existing in ExecutionIntentBuilder, full coverage в T-F12-901 U6) |
| F12-3 | T-F12-401, T-F12-402 |
| F12-4 | T-F12-303 |
| F12-5 | T-F12-304, T-F12-305 |
| F12-6 | T-F12-103 |
| F12-7 | T-F12-201, T-F12-202, T-F12-203, T-F12-301, T-F12-302, T-F12-303 |
| F12-8 | T-F12-101, T-F12-403, T-F12-501 |
| F12-9 | ✅ (existing VenueSimAdapter + backtest tests) |
| F12-10 | T-F12-801 (metrics), T-F12-902 IT-5 |
| F12-11 | T-F12-601, T-F12-701 (GET endpoints), T-F12-1003 |
| F12-12 | T-F12-404 |

## Definition of Done (IN-005 §11)

18 items:

1. [ ] Публикация ExecutionIntent с полным контрактом (`source`, `urgency`, `allowed_venues`, `client_order_id`) — T-F12-101 / T-F12-103.
2. [ ] Реализован Execution Planning с routing plan по venue.liquidity.fob + venue.health — T-F12-401, T-F12-402.
3. [ ] Реализован Risk Manager pre-hedge check (3 проверки) — T-F12-501.
4. [ ] Venue Execution Adapter с HedgeFlow + child_orders в PostgreSQL — T-F12-301, T-F12-302, T-F12-303.
5. [ ] Публикация ExecutionReport в Kafka `execution.venue` — T-F12-103.
6. [ ] Settlement Ledger потребляет execution.venue и обновляет positions + HedgePnL — T-F12-601.
7. [ ] ClickHouse `execution_reports` с retention 90+ дней — T-F12-203.
8. [ ] Unit U1-U10 пройдены — T-F12-901.
9. [ ] Integration IT-1..IT-7 пройдены — T-F12-902.
10. [ ] p95 latency CEX ≤ 100 ms — T-F12-801, T-F12-902.
11. [ ] Backtest parity confirmed — ✅ (IT-7).
12. [ ] HedgeFlow Monitor, Execution Live Feed, Hedge PnL Dashboard в UI — T-F12-1001..T-F12-1006.
13. [ ] Metrics + alerts в Observability — T-F12-801.
14. [ ] Operator runbook для инцидентов — `docs/11-operations/runbooks/F-12-hedge-incident.md` (TODO).
15. [ ] Архитектурная документация (этот PR) — ✅.
16. [ ] PostgreSQL DDL hedgeflows + child_orders — T-F12-201, T-F12-202.
17. [ ] ClickHouse DDL execution_reports — T-F12-203.
18. [ ] Топики execution.intents + execution.venue в `infra/kafka/create_topics.sh` — ✅ (но требуется switch с legacy `execution.reports` — T-F12-103).

## Estimation (порядок величин)

| Phase | Tasks | Усилия |
| --- | --- | --- |
| 1: Proto + Kafka migration | T-F12-101..103 | 2-3 дня |
| 2: Postgres + ClickHouse DDL | T-F12-201..203 | 2 дня |
| 3: Venue Execution Adapter | T-F12-301..305 | 7-10 дней |
| 4: Execution Planning | T-F12-401..404 | 5-7 дней |
| 5: Risk Manager extension | T-F12-501 | 2-3 дня |
| 6: Ledger HedgePnL | T-F12-601 | 2-3 дня |
| 7: REST API | T-F12-701 | 3-5 дней |
| 8: Observability | T-F12-801 | 2-3 дня |
| 9: Tests | T-F12-901..902 | 5-7 дней |
| 10: UI | T-F12-1001..1006 | 10-15 дней (frontend) |

Итого: **40-58 рабочих дней** для одного back-end + 10-15 для frontend (параллельно).

## Source Fragments

- IN-005 §1..11 (полная спецификация F-12)
- IN-001-FR-027, IN-001-FR-028 (baseline)
