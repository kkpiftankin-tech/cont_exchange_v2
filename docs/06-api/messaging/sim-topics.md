# Sim Topics (F-20 Live Venue Simulator)

Bundled contract for the four Kafka topics emitted by `venues` when running in
**simulator mode** (`VENUES_SIMULATE_ORDERS=1` or per-session
`SimSession.mode=sim`). They isolate simulator traffic from live execution flow
so a sim cohort cannot leak into the production hedge pipeline.

| Topic | Schema | Producer | Consumers | Partition key | Retention |
| --- | --- | --- | --- | --- | --- |
| `sim.config` | `fob.sim.v1.SimConfigEvent` | `venues` | `venues` (hot reload), `observability` | `sim_session_id` | 7 d |
| `sim.execution.venue` | `fob.execution.v1.ExecutionReport` (same wire contract as live `execution.venue`, namespace-isolated by topic) | `venues` (VenueSimRouter) | `ledger` (sim namespace only), `observability` | `hedge_flow_id` | 7 d |
| `sim.execution.annotations` | `fob.sim.v1.SimExecutionAnnotation` (sidecar telemetry correlated by `report_id`) | `venues` | `observability`, `frontend-api` (operator UI) | `report_id` | 7 d |
| `sim.alerts` | `fob.sim.v1.SimAlert` | `venues` (sim watchdogs) | `observability`, `risk` (advisory) | `sim_session_id` | 30 d |

References:

- Feature: [F-20 Live Venue Simulator](../../02-system/features/F-20-live-venue-simulator/feature.yaml).
- Architectural rationale: ADR-015 (sim/live isolation) — sim ExecutionReports
  must NEVER share the live `execution.venue` topic so a sim cohort cannot
  affect live hedge accounting.
- Topic-creation script: [`infra/kafka/create_topics.sh`](../../../infra/kafka/create_topics.sh).
- Catalog row in [`topics.md`](topics.md) — to be added (kafka-contract-auditor
  WARN on missing catalog entry).

## Stream-by-stream contract

### `sim.config`

Emitted on every `SimSession` lifecycle change (create / update mode / pause /
resume / complete / abort). Consumed by `venues` itself to hot-reload
`VenueSimRouter` policy without restart. Operator UI subscribes through
`observability` for live status.

Key invariants:

- `event_id` (UUID) per change — idempotent consumer side.
- `sim_session_id` partition key keeps all events for one session in order.
- `version_monotonic` strictly increasing per session — late events ignored.

### `sim.execution.venue`

Same `fob.execution.v1.ExecutionReport` proto as the live `execution.venue`
topic. **Topology-level isolation**: consumers of this topic must apply
balance/PnL updates to a separate `sim.*` namespace in the ledger; mixing
sim and live in one account is a contract violation.

Key invariants:

- `report_id` (UUID) — idempotency.
- `venue_id` carries a `SIM-` prefix (e.g. `SIM-CEX-binance-spot`) so even if
  a payload escapes the topic, it cannot be confused with live.
- `hedge_flow_id` partition key keeps fills of one hedge plan ordered.

### `sim.execution.annotations`

Sidecar telemetry that pairs with `sim.execution.venue` reports by `report_id`.
Carries simulator-specific diagnostics:

- inferred slippage vs LOB at time of submit;
- LOB source freshness in ms;
- random-reject flag and reason code;
- queue position / depth-consumed metrics.

Not part of the canonical ExecutionReport contract — consumers that don't
understand simulators can ignore this topic.

### `sim.alerts`

Watchdog alerts unique to sim runtime:

- `STALE_LOB` — LOB age > `VENUES_AUTO_SIM_STALE_LOB_THRESHOLD_MS`.
- `SIM_TIMEOUT` — sim fill loop exceeded SLA (`SIM_FILL_TIMEOUT_MS`).
- `LOB_SOURCE_DOWN` — feed lost; sim cannot price.
- `RANDOM_REJECT_SPIKE` — random-reject rate exceeded operator threshold.

These are advisory to risk; live kill-switch is NOT triggered automatically
from sim alerts (separate audit trail).

## Migration notes

This file was added by [AUDIT-001 T-AUDIT-007 follow-up](../../00-methodology/audits/AUDIT-001-feature-development-process.md)
to close the kafka-contract-auditor gap surfaced after Phase 1 landing. Prior
to it the four sim topics existed in `create_topics.sh` and were produced by
`venues` but had no contract doc — exact same drift class as PM-001 (`fills`).
