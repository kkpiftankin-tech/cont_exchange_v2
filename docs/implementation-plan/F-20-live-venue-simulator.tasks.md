---
id: F-20-live-venue-simulator.tasks
feature: F-20
status: planned
owner: core-team
created: 2026-05-26
---

# F-20 Live Venue Simulator — implementation plan

Source: [`docs/02-system/features/F-20-live-venue-simulator/feature.yaml`](../02-system/features/F-20-live-venue-simulator/feature.yaml)
(IN-010, mojibake placeholder, 19 DoD items, 5 uxScreens).

## Phasing

Implementation is **strictly gated on docs**: before any `cpp/venues/src/app/venue_simulator.*`
exists, every dependency below must land. Use this as a Jira epic
breakdown.

### Phase 0 — Docs (no code)

**Goal**: complete the docs-as-code chain before the first impl PR.

#### T-F20-001. Use cases UC-F20-01..06

Create 6 use cases under `docs/02-system/use-cases/UC-F20-{01..06}-*/use-case.md`:

- `UC-F20-01-sim-only-validation` — operator runs SIM_ONLY session for pre-prod hedge strategy validation.
- `UC-F20-02-shadow-comparison` — operator runs SHADOW for live SIM-vs-LIVE delta tracking.
- `UC-F20-03-test-new-impact-latency-models` — engineer A/B-tests new model parameters on live data.
- `UC-F20-04-simulate-venue-degradation` — operator configures RejectionModel to test F-12 fallback logic.
- `UC-F20-05-sim-to-live-go-live` — operator switches routingMode=LIVE_ONLY after validation.
- `UC-F20-06-model-calibration-from-shadow-data` — operator updates models via hot reload based on accumulated SHADOW divergence.

Each must include system-level sequence diagram
(`docs/02-system/use-cases/UC-F20-{id}/sequences/SEQ-UC-F20-{id}-system.md`).

#### T-F20-002. Service sequence diagrams

Create `docs/05-components/sequences/`:

- `SEQ-F20-01-sim-only-flow-services.md` — full SIM_ONLY flow (ChildOrderRequest → VenueSimulator → SimExecutionReport → Ledger).
- `SEQ-F20-02-shadow-flow-services.md` — SHADOW dual-fork + Divergence Service.
- `SEQ-F20-03-stale-lob-and-alerts-services.md` — stale LOB handling.

#### T-F20-003. Component docs

Create `docs/05-components/`:

- `venue-simulator/overview.md`
- `venue-sim-router/overview.md`
- `sim-session-manager/overview.md`
- `divergence-service/overview.md`

#### T-F20-004. Contracts docs

- `docs/06-api/messaging/sim-config-topic.md` — schema for `sim.config` Kafka events.
- `docs/06-api/messaging/sim-alerts-topic.md` — schema for `sim.alerts`.
- `docs/06-api/messaging/sim-execution-venue-topic.md` — schema for optional `sim.execution.venue` mirror.
- `docs/06-api/messaging/sim-execution-venue-topic.md` — `sim.execution.venue` carries the *unchanged* `ExecutionReport` (ADR-015). Document that `execution.proto` is NOT extended and that sim telemetry rides the separate `SimExecutionAnnotation` (sim.proto).
- `docs/06-api/rest/sim-sessions.md` — REST API SimSession Manager.

#### T-F20-005. Data docs

- `docs/07-data/sim-sessions.md` — PG `sim_sessions` schema.
- `docs/07-data/sim-execution-reports-ch.md` — CH `sim_execution_reports` schema.
- `docs/07-data/sim-divergence-log-ch.md` — CH `sim_divergence_log` schema.

#### T-F20-006. Test plan

`docs/10-testing/features/F-20-test-plan.md` covering U1..U10, IT-1..IT-5,
LT-1..LT-4 from spec.

#### T-F20-007. ADRs — ✅ DONE (PR-F20-1, 2026-05-28)

- [`ADR-015-sim-execution-topic-isolation.md`](../03-architecture/adr/ADR-015-sim-execution-topic-isolation.md) — **strict isolation**: SimExecutionReport only on `sim.execution.venue`, never shared `execution.venue` (avoids SHADOW double-apply).
- [`ADR-016-ledger-sim-book-storage.md`](../03-architecture/adr/ADR-016-ledger-sim-book-storage.md) — **separate tables** `sim_positions`/`sim_hedge_pnl`, not a simMode flag (financial-safety by construction).
- [`ADR-017-venue-simulator-vs-legacy-adapter.md`](../03-architecture/adr/ADR-017-venue-simulator-vs-legacy-adapter.md) — **new class** `VenueSimulator`, legacy `venue_sim_adapter` stays as backtest helper.

These three gates are now resolved; Phase 1 is unblocked.

#### T-F20-008. Update feature-index.md

Append F-20 row to `docs/02-system/features/feature-index.md`.

### Phase 1 — Proto contracts

#### T-F20-101. Define `contracts/proto/fob/sim/v1/sim.proto` — ✅ DONE (PR-F20-3)

Created and protoc-verified (cpp + grpc stubs generate cleanly). Picked
up automatically by `contracts/CMakeLists.txt` GLOB_RECURSE.

Messages:

- `SimSession` — config + status.
- `SimConfigEvent` — Kafka topic envelope for hot reload.
- `SimAlert` — STALE_LOB / SIM_TIMEOUT / LOB_SOURCE_DOWN / RANDOM_REJECT_SPIKE.
- `SimExecutionAnnotation` — sim telemetry sidecar correlated to an
  `ExecutionReport` by `report_id`: `sim_session_id`, `lob_snapshot_id`,
  `lob_age_ms`, `impact_bps`, `latency_sample_ms`. (See T-F20-102 for why
  this is separate from ExecutionReport.)
- Enums: `RoutingMode { SIM_ONLY, LIVE_ONLY, SHADOW }`, `SimSessionStatus { ACTIVE, PAUSED, COMPLETED, CANCELLED }`.
- Embedded models (`LatencyModel`, `ImpactModel`, `FeeModel`, `RejectionModel`) as messages with `oneof distribution_type` etc.

#### T-F20-102. `execution.proto` is NOT extended (ADR-015 decision)

The shared `ExecutionReport` contract stays **unchanged**. Per ADR-015,
the simulator emits the *same* `ExecutionReport` a real venue would
(populating the existing `filled_qty` / `average_price` / `slippage_bps`
/ `hedge_pnl` / `reference_mid` / `fee_total` fields), just to a
different topic (`sim.execution.venue`). The topic is the sim/live
discriminator — there is no `sim_mode` field.

Sim-only provenance/telemetry (`sim_session_id`, `lob_snapshot_id`,
`lob_age_ms`, `impact_bps`, `latency_sample_ms`) lives in
`fob.sim.v1.SimExecutionAnnotation` (T-F20-101), published to a sim-only
telemetry stream and landed as columns in CH `sim_execution_reports`.
The VenueSimulator, which has the LOB context, writes the annotation;
the live path never produces or consumes it.

Rationale: keep the venues↔ledger execution contract venue-agnostic
(the user's principle: sim and real exchanges interact via identical
contracts). See ADR-015 §"ExecutionReport остаётся неизменным".

#### T-F20-103. `contracts/openapi/fob/sim/v1/api/sim.yaml`

REST contract for SimSession Manager CRUD.

### Phase 2 — Storage

#### T-F20-201. PostgreSQL `sim_sessions` (+ sim-book) — ✅ DONE (PR-F20-4)

Added to `infra/postgres/init.sql`, applied + verified on live PG:

- `sim_sessions` (spec §4.5 fields, models as JSONB).
- `sim_positions` and `sim_hedge_pnl` (ADR-016 isolated sim-book, FK to
  sim_sessions ON DELETE CASCADE for clean teardown).

#### T-F20-202. ClickHouse `sim_execution_reports` + `sim_divergence_log` — ✅ DONE (PR-F20-4)

Added to `infra/clickhouse/init.sql`, applied + verified on live CH
(DESCRIBE ok). `sim_execution_reports` carries the shared ExecutionReport
columns plus the SimExecutionAnnotation sidecar columns (no `sim_mode` —
topic is the discriminator, ADR-015). 90-day TTL, monthly partition.

### Phase 3 — Core simulator

#### T-F20-301. VenueSimulator skeleton

`cpp/venues/src/app/venue_simulator.{cpp,hpp}`. Subscribe to
`venue.snapshots`, maintain LOB cache keyed by `(venueId, symbol)`,
expose `SimulateOrder(ChildOrderRequest) -> SimExecutionReport`.

#### T-F20-302. LOB-matching algorithm

Per spec §4.4. Tests U4 (LEVEL_BY_LEVEL precision) + DoD-4.

#### T-F20-303. ImpactModel implementations

LINEAR / SQRT / POWER_LAW / LEVEL_BY_LEVEL per spec §1.0 ImpactModel
section. Tests U5.

#### T-F20-304. FeeModel + RejectionModel

Tests U6, U7.

#### T-F20-305. LatencyModel + async-delay publication

Tests U8.

#### T-F20-306. Stale-LOB protection

`lobAge > staleLobThresholdMs` → `REJECTED, SIM_STALE_LOB` + `sim.alerts`.
Tests U3, DoD-5, F20-4.

#### T-F20-307. Overfill guard

Trim filledQty to targetQty. Test U10.

### Phase 4 — Router + sessions

#### T-F20-401. SimSession Manager

`cpp/venues/src/app/sim_session_manager.{cpp,hpp}` + `postgres_sim_session_repository.{cpp,hpp}`.
gRPC + REST endpoints per OpenAPI. Hot reload via Kafka `sim.config` producer.

#### T-F20-402. VenueSimRouter

`cpp/venues/src/app/venue_sim_router.{cpp,hpp}`. Cache `(venueId,symbol)→routingMode`.
Subscribe to `sim.config` for hot reload. Implement SIM_ONLY / LIVE_ONLY /
SHADOW (fork). Test F20-1, F20-2, F20-7, F20-11, F20-12.

#### T-F20-403. Wire VenueSimRouter into Venue Execution Adapter

Replace direct EVC call with `VenueSimRouter.Route(ChildOrderRequest)`.
Backward-compatible default: when no SimSession active for
`(venueId,symbol)`, behaviour is identical to current LIVE_ONLY.

### Phase 5 — Downstream extensions

#### T-F20-501. Ledger sim-book

Per ADR-016 (separate sim_* tables). The sim-book consumer subscribes to
`sim.execution.venue` (ADR-015) — every report on that topic is sim by
definition (no `sim_mode` field needed), so it updates the isolated
`sim_positions` / `sim_hedge_pnl` records without touching real provider
state. Test DoD-7, F20-5.

#### T-F20-502. ClickHouse ingest for sim_execution_reports

Either via `market_data` writer extension or dedicated CH-Kafka connector.
Test DoD-8, F20-6.

#### T-F20-503. Divergence Service

`cpp/venues/src/app/divergence_service.{cpp,hpp}`. Consume
`execution.venue`, pair LIVE+SIM by `clientOrderId`, compute deltas,
write to CH `sim_divergence_log`. Test DoD-9, F20-10.

### Phase 6 — Observability + UI

#### T-F20-601. Prometheus metrics

`cpp/venues/src/infra/sim_metrics.cpp`:

- `f20_sim_orders_total{venue, symbol, status}`
- `f20_sim_latency_ms` (histogram, p50/p95/p99)
- `f20_lob_age_ms` (gauge)
- `f20_sim_rejections_total{reason}`
- `f20_shadow_divergence_count{type}`

#### T-F20-602. Admin UI SimSession Manager (DoD-14)

`frontend/web/src/pages/SimManager/`.

#### T-F20-603. Admin UI Live Sim Feed (DoD-15)

#### T-F20-604. Admin UI Sim vs Live Comparison (DoD-16)

#### T-F20-605. Admin UI LOB Quality Monitor

#### T-F20-606. Admin UI Model Calibration Panel

### Phase 7 — Tests

#### T-F20-701. Unit tests U1..U10

Coverage list in spec §6 DoD-10.

#### T-F20-702. Integration tests IT-1..IT-5

Coverage list in spec §6 DoD-11.

#### T-F20-703. Load tests LT-1..LT-4

Coverage list in spec §5.2.

### Phase 8 — Ops

#### T-F20-801. Runbook

`docs/11-operations/runbooks/F-20-incidents.md` per DoD-18.

## DoD → task mapping

| DoD | Closing tasks |
| --- | --- |
| DoD-1 (VenueSimulator) | T-F20-301..305 |
| DoD-2 (VenueSimRouter) | T-F20-402, T-F20-403 |
| DoD-3 (SimSession Manager) | T-F20-401 |
| DoD-4 (LOB-matching correctness) | T-F20-302, T-F20-701 (U4) |
| DoD-5 (Stale-LOB) | T-F20-306, T-F20-701 (U3) |
| DoD-6 (ExecutionReport unchanged + SimExecutionAnnotation sidecar) | T-F20-101 (annotation), T-F20-301 |
| DoD-7 (Ledger sim-book) | T-F20-501 |
| DoD-8 (ClickHouse ingest) | T-F20-202, T-F20-502 |
| DoD-9 (Divergence Service) | T-F20-503 |
| DoD-10 (Unit U1..U10) | T-F20-701 |
| DoD-11 (Integration IT-1..IT-5) | T-F20-702 |
| DoD-12 (SLA) | T-F20-006 (plan), T-F20-703 |
| DoD-13 (LT-1) | T-F20-703 |
| DoD-14 (UI SimSession Manager) | T-F20-602 |
| DoD-15 (UI Live Sim Feed) | T-F20-603 |
| DoD-16 (UI Sim vs Live) | T-F20-604 |
| DoD-17 (Metrics/alerts) | T-F20-601 |
| DoD-18 (Runbook) | T-F20-801 |
| DoD-19 (Architecture docs) | T-F20-002, T-F20-003, T-F20-004, T-F20-005 |

## Out of scope

- HFT microsecond effects (front-running, queue position).
- Multi-instrument LOB depletion coupling (LOB depletion only correct
  for serial single-instrument orders).
- Legal/financial significance of `simMode=true` reports — never used
  for real P&L.

## Critical path

```text
Phase 0 (docs+ADRs) -> Phase 1 (protos) -> Phase 2 (storage) ->
Phase 3 (simulator) -> Phase 4 (router) -> Phase 5 (ledger/CH/divergence) ->
Phase 6 (UI) -> Phase 7 (tests) -> Phase 8 (runbook)
```

Phases 1-2 can overlap. Phase 6 UI can start in parallel with Phase 5
once Phase 4 contract endpoints are stable. Phase 7 load tests gate
go-live; SLA failures rollback PR.

## ADR gates — RESOLVED (PR-F20-1, 2026-05-28)

1. ✅ [ADR-015](../03-architecture/adr/ADR-015-sim-execution-topic-isolation.md) sim-topic-isolation → strict isolation (`sim.execution.venue` only).
2. ✅ [ADR-016](../03-architecture/adr/ADR-016-ledger-sim-book-storage.md) ledger sim-book → separate `sim_positions`/`sim_hedge_pnl` tables.
3. ✅ [ADR-017](../03-architecture/adr/ADR-017-venue-simulator-vs-legacy-adapter.md) venue-simulator-vs-legacy → new `VenueSimulator` class.

All three `knownIssues` in feature.yaml flipped to `status=resolved`.
**Phase 1 (proto) is unblocked.** Next concrete step: T-F20-101
(define `contracts/proto/fob/sim/v1/sim.proto` incl. `SimExecutionAnnotation`).
Per ADR-015, `execution.proto` is NOT extended — the simulator emits the
unchanged `ExecutionReport` to `sim.execution.venue`, and sim telemetry
travels in the separate `SimExecutionAnnotation` sidecar.
