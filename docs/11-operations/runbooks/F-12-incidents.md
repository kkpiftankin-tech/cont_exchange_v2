---
id: RUNBOOK-F-12-INCIDENTS
phase: 11-operations
feature: F-12
status: draft
owner: core-team
last_reviewed: 2026-05-27
covers_dod: DoD-17
---

# F-12 Execution Hedge — incident response runbook

Operational reference for on-call dealing with F-12 (Execution Hedge)
incidents. Linked from
[`docs/02-system/features/F-12-execution-hedge/feature.yaml#definitionOfDone DoD-17`](../../02-system/features/F-12-execution-hedge/feature.yaml).

> **Status: draft.** Prod is not live; this runbook is written against
> the dev/staging stack (docker compose) and the UI live pages shipped
> in PR-F12-6/7/8/9 (2026-05-26..27). When production deploys, revisit
> SLO thresholds and escalation contacts.

## Quick reference

### When to open this runbook

- A user-visible page (HedgeFlow Monitor, Hedge PnL, Reconciliation
  Alerts) is showing impossible numbers, stuck status, or errors.
- An alert firing from observability: `HEDGE_UNDERFILL`,
  `SLIPPAGE_EXCEEDED`, `LATENCY_SLA_BREACH`, `VENUE_CIRCUIT_OPEN`.
- A trader reports their flow order filled internally but the hedge
  side appears never to have run.
- Settlement Ledger logs show `Hedge PnL computed` with extreme delta
  values, or the ledger consumer is lagging on `execution.venue`.

### What this runbook does NOT cover

- F-04 batch clearing failures (see
  [`docs/02-system/features/F-04-batch-clearing/feature.yaml`](../../02-system/features/F-04-batch-clearing/feature.yaml)
  for matching incidents).
- F-11 venue connectivity (LOB/marketdata) outages — see
  [`docs/05-components/external-venues/`](../../05-components/external-venues/).
- Custody / on-chain settlement.

### One-screen dashboard

| Where to look | URL | What you'll see |
| --- | --- | --- |
| HedgeFlow Monitor (PG live) | `/hedge-flows-live` | per-flow status, fillRatio, hedgePnL |
| HedgeFlow drill-down | (click row in monitor) | child orders + execution timeline |
| Hedge PnL Dashboard | `/hedge-pnl-live` | summary KPI, hourly cumulative, breakdown by symbol/venue |
| Reconciliation Alerts | `/reconciliation-alerts-live` | UNDERFILLED / REJECTED / RISK_REJECTED flows |
| Execution Live Feed | `/execution-live-feed-live` | rolling CH `execution_reports` last 100 |
| Policy Config (read-only) | `/policy-config-live` | active HEDGE_TRIGGER_* + solver_config |

Public Funnel mirror in dev: `https://nik.tail318efe.ts.net/<path>`.

## Glossary of identifiers seen in logs

| Format | Meaning |
| --- | --- |
| `<uuid>\|hedge\|<provider>\|<symbol>` | Real F-12 auto-hedge `hedge_flow_id` from matching's `hedge_trigger_policy`. |
| `<batch>\|<order>\|<symbol>\|<venue>\|external_fill_N` | F-04 external-liquidity fill (composite intent_id). |
| `<hedge_flow_id>\|intent` | child-order `client_order_id` / `intent_id` (with `\|intent` suffix). |

If you see a `hedge_flow_id` in alerts or PG, the `|hedge|` token tells
you it came from F-12 trigger policy; absence means F-04 external_fill
path. Different escalation paths apply (see below).

## Operating principles

1. **Read before write.** Always pull the current state from PG/CH/UI
   before changing config or replaying. F-12 errors are usually
   diagnostic, not destructive — wrong config rollback is more
   expensive than the symptom.
2. **Never restart `matching` mid-batch unless absolutely necessary.**
   In-flight `ChildOrderRequest` payloads in Kafka survive, but
   in-memory state in `position_snapshot_calculator` resets — you'll
   re-trigger hedge intents that already executed.
3. **Sim mode (F-20, draft) does NOT affect F-12 live state.** If a
   SimSession exists, double-check `routingMode` before acting on
   downstream metrics that may include `simMode=true` rows. F-20 is
   docs-only at time of writing; sanity check passes today.

## Incident playbooks

### Incident 1 — Elevated slippage (`SLIPPAGE_EXCEEDED`)

**Symptom**: avg `slippage_bps` on the Hedge PnL Dashboard exceeds the
policy maximum (`HEDGE_INTENT_MAX_SLIPPAGE_BPS`, default 50 bps in dev).
Or, CH alert fires when rolling 5-minute median > threshold.

**Diagnose (5 min)**:

```sql
-- ClickHouse: which venue/symbol drove the spike?
SELECT venue_id, symbol,
       quantile(0.5)(slippage_bps) AS slip_p50,
       quantile(0.95)(slippage_bps) AS slip_p95,
       count() AS reports
FROM execution_reports
WHERE event_time_ms >= toUnixTimestamp64Milli(toDateTime64(now() - INTERVAL 15 MINUTE, 3))
GROUP BY venue_id, symbol
ORDER BY slip_p95 DESC
LIMIT 10;
```

```sql
-- PostgreSQL: are open hedges sitting OPEN unusually long?
SELECT hedge_flow_id, symbol, target_qty, filled_qty,
       EXTRACT(EPOCH FROM (now() - created_at)) AS age_sec
FROM hedgeflows
WHERE status = 'OPEN'
ORDER BY created_at ASC
LIMIT 20;
```

**Decision tree**:

- One venue dominates p95 → temporarily remove from
  `HEDGE_INTENT_ALLOWED_VENUES` env (matching restart required —
  acceptable when problem is venue-specific). Alternative: tighten the
  trigger threshold for affected symbol so fewer hedges fire until the
  venue recovers.
- Multiple venues, single symbol → suspect F-11 LOB staleness for that
  symbol. Pivot to Incident 4 (venue circuit) playbook.
- All venues + all symbols, sudden onset → suspect matching emitted a
  burst of large intents (e.g. operator user inserted oversized flow).
  Check `cpp/matching` logs for `Built auto-hedge execution intents`
  with abnormal `target_qty`.

**Mitigate**: reduce `HEDGE_INTENT_URGENCY` from `MEDIUM`/`HIGH` to
`LOW` (slower strategy → less aggressive book walking) via env update
+ matching restart. NOTE: changing urgency mid-incident is a real
intervention, not a no-op — record in incident log.

### Incident 2 — Mass UNDERFILLED (`HEDGE_UNDERFILL` alert spam)

**Symptom**: Reconciliation Alerts page shows >5 rows in UNDERFILLED
within a few minutes; PG `hedgeflows.status='UNDERFILLED'` count
spikes.

**Diagnose (5 min)**:

```sql
-- PG: distribution by venue + recent
SELECT
  COALESCE(NULLIF(SPLIT_PART(intent_id, '|', 4), ''), 'unknown') AS venue_guess,
  status, count(*)
FROM hedgeflows
WHERE updated_at >= now() - INTERVAL '15 minutes'
  AND status IN ('UNDERFILLED', 'REJECTED', 'RISK_REJECTED')
GROUP BY venue_guess, status
ORDER BY count(*) DESC;
```

```bash
# Kafka: is venues consumer keeping up with execution.intents?
docker exec infra-redpanda-1 rpk group describe venues
# Look at lag column; > 100 means venues is behind matching.
```

```bash
# Venues service logs: timeouts or rejections?
docker logs infra-venues-1 --since 15m 2>&1 | grep -E 'TIMEOUT|REJECTED|circuit_breaker' | tail -20
```

**Decision tree**:

- venues consumer lag > 0 → scale venues service OR if can't scale,
  pause F-12 emission by setting `HEDGE_TRIGGER_QTY_DEFAULT=999`
  (disables trigger) + matching restart. Re-enable when lag drains.
- venues lag = 0, REJECTED dominates → venue-side issue. Go to
  Incident 4.
- UNDERFILLED only, no REJECTED → could be `timeout_ms` too tight.
  Raise `HEDGE_INTENT_TIMEOUT_MS` from 30000 (default) to 60000;
  matching restart.

**Known sim-mode caveat (dev only)**: in dev, `venues-sim-zero-execution-price`
(see F-12 feature.yaml knownIssues) means executed_price is 0 and
slippage_bps is 0. If you see UNDERFILLED in dev that's a real bug —
sim adapter should always fully fill MARKET intents against simulated
liquidity. Capture in a new feature.yaml knownIssue and escalate to
core-team.

### Incident 3 — Latency SLA breach (`LATENCY_SLA_BREACH`)

**Symptom**: p95 latency from ExecutionIntent publish to first
ExecutionReport exceeds 100 ms (CEX) / 1000 ms (DEX/AMM) per acceptance
F12-10.

**Diagnose (5 min)**:

```sql
-- CH: end-to-end latency by venue
SELECT venue_id,
       quantile(0.5)(event_time_ms - intent_time_ms) AS lat_p50,
       quantile(0.95)(event_time_ms - intent_time_ms) AS lat_p95,
       quantile(0.99)(event_time_ms - intent_time_ms) AS lat_p99,
       count() AS reports
FROM execution_reports er
ANY LEFT JOIN (
  SELECT intent_id, min(event_time_ms) AS intent_time_ms
  FROM execution_reports
  WHERE status = 'PENDING'
  GROUP BY intent_id
) AS pending ON er.intent_id = pending.intent_id
WHERE er.event_time_ms >= toUnixTimestamp64Milli(toDateTime64(now() - INTERVAL 15 MINUTE, 3))
GROUP BY venue_id
ORDER BY lat_p95 DESC;
```

Note: today our CH `execution_reports` has only terminal-state rows;
intent timing must come from matching service logs or the `intent_id`
embedded creation timestamp (UUID v7-style). Per the implementation
plan, T-F12-501 will add `intent_published_ts` to ER; until then this
query approximates by `event_time_ms` only.

**Decision tree**:

- Kafka consumer/producer queue depth high → broker overloaded;
  consider scaling redpanda or moving topic partitions.
- One venue specifically slow → DEX/AMM congestion (gas spikes for
  Ethereum-side); the 1000 ms target accommodates this, but if
  sustained > 5 sec, disable the venue in `HEDGE_INTENT_ALLOWED_VENUES`.
- Latency rising linearly with order count → matching's
  `hedge_execution_intents_publisher` thread is bottlenecked. Restart
  matching service to clear consumer-group rebalancing; if it persists,
  open ticket for thread pool sizing.

### Incident 4 — Venue circuit breaker open (`VENUE_CIRCUIT_OPEN`)

**Symptom**: F-11 `venue.health` topic carries `status=CIRCUIT_OPEN`
for a venue; matching log shows `Rejected planner venue input after
health update reason=circuit_breaker_open`; F-12 hedge intents stop
flowing to that venue.

**Diagnose (2 min)**:

```bash
docker logs infra-venues-1 --since 30m 2>&1 | grep -i 'circuit' | tail -20
docker logs infra-market_data-1 --since 30m 2>&1 | grep -i 'venue.health\|disconnected' | tail -10
```

```bash
# Kafka: drain a recent venue.health message
docker exec infra-redpanda-1 rpk topic consume venue.health -n 5 -o end
```

**Decision tree**:

- Real venue outage (their API down) → wait it out; the circuit will
  half-open on a timer and recover automatically. Notify trading desk
  that hedges to this venue are paused.
- False positive from F-11 (LOB feed flapping but venue is fine) → see
  F-11 runbook; manually re-enable venue by sending
  `venue.health` event with `status=CONNECTED` (operator gRPC API).
- All three configured venues open simultaneously → DO NOT try to
  manually re-enable; that's a market-data or network problem upstream.
  Pause F-12 emission entirely (see Incident 2 mitigation).

### Incident 5 — Ledger HedgePnL implausible

**Symptom**: a single `hedge_pnl` row in PG shows a value 10×–100×
larger than expected for the trade size; or the cumulative PnL in the
Hedge PnL Dashboard jumps without corresponding fills.

**Diagnose (10 min)**:

```sql
-- PG: find the outlier flow
SELECT hedge_flow_id, symbol, side,
       target_qty::text, filled_qty::text,
       reference_mid::text, avg_fill_price::text,
       hedge_pnl::text
FROM hedgeflows
WHERE hedge_pnl IS NOT NULL
ORDER BY abs(hedge_pnl) DESC
LIMIT 10;
```

```bash
# Ledger logs: confirm what was applied
docker logs infra-ledger-1 --since 1h 2>&1 | grep '<hedge_flow_id_substring>' | tail -20
```

**Decision tree**:

- `reference_mid` is 0 → matching never set it. Bug in
  `BuildFromHedgeTriggerDecisions`; this should never happen for
  `|hedge|` flows (acceptance F12-11). Open issue.
- `executed_price` is 0 → in dev, expected (knownIssue
  `venues-sim-zero-execution-price`). In prod, indicates the venue
  adapter didn't populate the field — venue-specific bug.
- Both populated but PnL still wrong → check the side. Sign-flip
  inside ledger's `calculate_hedge_pnl` was fixed in PR-F12-3c
  (2026-05-26); regression would re-introduce sign errors. Compare:
  - `BUY` hedge: hedge_pnl = (reference_mid − executed_price) × filled_qty
  - `SELL` hedge: hedge_pnl = (executed_price − reference_mid) × filled_qty

**Recovery**: if a row is corrupt, you can manually correct it in PG:

```sql
-- DO NOT run without owner approval. Backup first.
UPDATE hedgeflows
SET hedge_pnl = 0, error_message = 'manually zeroed during incident <ticket>'
WHERE hedge_flow_id = '<exact_id>';
```

Document in the incident ticket and audit-log table once that lands
(PR-F12-9-followup).

### Incident 6 — Reconciliation Alerts UI shows nothing but PG has rejected flows

**Symptom**: PG query shows `SELECT count(*) FROM hedgeflows WHERE
status = 'REJECTED'` returns > 0, but `/reconciliation-alerts-live`
shows empty state.

**Diagnose (2 min)**:

```bash
# Is the endpoint returning the rows?
docker exec infra-postgres-1 psql -U cex -d cex -c "SELECT count(*) FROM hedgeflows WHERE status IN ('UNDERFILLED','REJECTED','RISK_REJECTED');"
curl -sS 'http://localhost:8090/api/v1/hedge/reconciliation-alerts' | python3 -m json.tool | head -20
```

**Likely cause**: in-memory `ACKNOWLEDGED_RECONCILIATION_ALERTS` set
in frontend-api was populated; rows are filtered out by default. Toggle
"Show acknowledged" on the page, OR restart frontend-api (clears the
set):

```bash
docker compose restart frontend-api
```

This is the captured limitation of PR-F12-9a (acknowledge is in-memory
only). PG-persisted ack column is a deferred follow-up.

### Incident 7 — Live UI page won't load / shows infinite spinner

**Symptom**: a `/hedge-*-live` route loads but the page sits on
"Загрузка...".

**Diagnose (1 min)**:

```bash
docker logs cex-frontend-api --tail 30 2>&1 | grep -i 'error\|fail'
# Also try direct curl from inside the docker network:
docker exec cex-frontend-web wget -qO- http://frontend-api:8090/api/v1/hedge/policy-config | head -5
```

**Likely causes**:

1. `FRONTEND_POSTGRES_DSN` not set → 503 with
   `postgres_not_configured`. Fix in `frontend/docker-compose.yml`.
2. PG container down → frontend-api connection errors. Run
   `docker compose ps` and restart `postgres` if needed.
3. CH container down (affects PnL Dashboard, Execution Live Feed,
   drill-down timeline tab). Same drill: check `infra-clickhouse-1`.

### Incident 8 — `dec-flowmonitor` shows external_fill rows but no `|hedge|` rows

**Symptom**: HedgeFlow Monitor lists rows but they all have suffix
`|external_fill_N`. No real F-12 hedge intents are firing.

**Diagnose (1 min)**:

```bash
# Is the trigger policy actually configured?
docker logs infra-matching-1 --since 5m 2>&1 | grep 'F-12 hedge config' | head -1
```

If the line shows `default_threshold_qty: 0` and
`per_symbol_overrides_count: 0`, the trigger is **disabled** — all
thresholds are zero. Check `.env-example` for `HEDGE_TRIGGER_*` values
and that matching service is using the updated file.

If thresholds are set but still no `|hedge|` rows: the snapshot
calculator computes per-batch delta (not cumulative position). If
each batch fills < threshold qty, no hedge fires even though
cumulative position would qualify. This is the known design limitation
of `position_snapshot_calculator.cpp`. Lower the per-symbol threshold
or wait for a larger order to come through.

## Routine operations

### Switching urgency policy mid-day

To make hedges more aggressive (e.g. risk team raises
`HEDGE_INTENT_URGENCY=HIGH`):

1. Edit `infra/env/.env-example` on the host.
2. `cd infra && docker compose -f docker-compose.dev.yml up -d --force-recreate matching`.
3. Verify the `F-12 hedge config loaded` startup log shows the new
   urgency: `urgency: 3` for HIGH (`URGENCY_HIGH` enum value).
4. Hot reload is **not** supported yet — restart is the only path.
   (Captured as F-12 follow-up after the policy config UI mutation
   endpoint lands.)

### Pausing F-12 emission entirely

Set `HEDGE_TRIGGER_QTY_DEFAULT=999999` and clear
`HEDGE_TRIGGER_SYMBOLS` (or set both per-symbol values very high).
Matching restart. F-04 external_fill intents continue to flow; only
the F-12 trigger policy is silenced.

### Re-enabling a previously disabled venue

`HEDGE_INTENT_ALLOWED_VENUES` is a CSV; add the venue back, matching
restart. Verify with the next emitted intent: in `cpp/matching` logs
look for the `allowed_venues` field in `Built auto-hedge execution
intents` line.

## Escalation

| Symptom | First responder | Escalate to |
| --- | --- | --- |
| Latency SLA breach | on-call ops | core-team (matching, venues) |
| Ledger HedgePnL implausible | on-call ops | core-team (ledger) + accounting |
| Multi-venue circuit open | on-call ops | network/SRE + venue vendors |
| Risk reject flood | on-call ops | risk-team (pre-hedge policy) |

(Contact details are a deferred deliverable — fill in when production
deployment lands.)

## Known feature gaps that affect this runbook

These items in the F-12 feature.yaml's `knownIssues` can change the
diagnostic story:

- `venues-sim-zero-execution-price` (dev only) — explains why
  hedge_pnl and slippage_bps are zero in dev.
- `observability-reporting-vs-frontend-api` (IN-009) — the frontend-api
  is currently fulfilling the Observability Reporting role; if you see
  schema mismatches between dashboards and PG/CH, this is the reason.

When in doubt, check
[`feature.yaml`](../../02-system/features/F-12-execution-hedge/feature.yaml#knownIssues)
for the most recent gap list.

## Change log for this runbook

| Date | Change | PR |
| --- | --- | --- |
| 2026-05-27 | Initial draft covering 8 incidents + 3 routine ops (DoD-17). | PR-F12-10 |
