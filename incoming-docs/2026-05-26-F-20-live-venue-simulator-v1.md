<!-- Auto-archived placeholder for IN-010.
     Original source: chat-attached "F-20 -Live-Venue-Simulator.md".

     CONFLICT RESOLUTION: the spec body uses the ID "F-16" throughout,
     but in this repository F-16 is already assigned to
     "Operator Console & Kill-Switch" (see
     docs/02-system/features/feature-index.md). The source filename
     itself ("F-20 -Live-Venue-Simulator.md") is the correct hint:
     **Live Venue Simulator is registered as F-20**. All derived
     artifacts use F-20; the spec's internal "F-16" labels are
     mechanically rewritten to F-20 during ingest.

     Same situation as IN-008 / IN-009: the chat transcript contained
     mojibake (UTF-8 source interpreted as Latin-1) AND non-printable
     continuation bytes were lost in transit, so the Russian prose
     cannot be byte-perfectly recovered from the transcript. Code
     blocks (Mermaid, JSON, SQL, C++), section headings, JSON field
     names, and identifiers were ASCII-clean and ARE preserved in
     the derived artifacts (feature.yaml, fragment-map, tasks).

     If you need the byte-exact source, it lives in:
       - the chat session transcript at
         /Users/aleksandrpiftankin/.claude/projects/.../95b9e055-2974-4e0b-bb14-a06b2998dd9f.jsonl
         (search for the user message timestamped 2026-05-26 with the
          F-20 attachment).
       - the Perplexity-style S3 URL embedded in the message
         (presigned, expires ~1777537654 = 2026-04-25 UTC; will be
          stale before this archive is read).

     The structural content (concepts, parameters, DoD, sequence
     diagrams, contracts, SLO/load tests, error matrix) was recovered
     into:
       - docs/02-system/features/F-20-live-venue-simulator/feature.yaml
       - docs/implementation-plan/F-20-live-venue-simulator.tasks.md
       - incoming-docs/IN-010.fragment-map.md
       - incoming-docs/IN-010.meta.md
-->

# IN-010 — F-20 Live Venue Simulator (placeholder)

This file intentionally documents the gap rather than carrying the
original content. See header comment above for the rationale and
pointers to the derived structured artifacts.

## Spec summary (re-stated, not copy-paste)

F-20 introduces a **hybrid execution mode** where the system keeps
receiving live LOB snapshots from external CEX/DEX/AMM via the
existing F-11 pipeline, but child-order execution is routed through
an internal `VenueSimulator` that uses the live snapshot as its
liquidity source. Synthetic ExecutionReports flow through the same
`execution.venue` Kafka topic with `simMode=true`, and downstream
(Ledger, Risk, ClickHouse) treats them as a separate book.

Key new components (none of which exist today):

- **VenueSimRouter** — switchboard between SIM / LIVE / SHADOW per
  `venueId+symbol`.
- **VenueSimulator** — LEVEL_BY_LEVEL LOB matcher with pluggable
  `LatencyModel`, `ImpactModel`, `FeeModel`, `RejectionModel`.
- **SimSession Manager** — CRUD for `sim_sessions` rows + hot-reload
  via `sim.config` topic.
- **Divergence Service** — pairs LIVE and SIM ExecutionReports on
  `clientOrderId` in SHADOW mode and computes delta metrics.

New Kafka topics: `sim.config`, `sim.alerts`, `sim.execution.venue`.
New PostgreSQL table: `sim_sessions`. New ClickHouse tables:
`sim_execution_reports`, `sim_divergence_log`.

19 DoD items, 5 detailed scenarios, full sequence diagrams, contracts,
SLO targets (p95 overhead < 50ms, throughput >= 500 orders/sec),
and load tests LT-1..LT-4 are captured in the derived feature.yaml.
