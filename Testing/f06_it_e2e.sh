#!/usr/bin/env bash
# ============================================================================
# F-06 Positions / PnL / Margin — Integration E2E harness (T-F06-061).
#
# Scenarios I1..I6 from docs/implementation-plan/F-06-positions-pnl-margin.tasks.md:
#   I1  persistence: ledger restart -> balances/positions survive
#   I2  full cycle: FlowOrder -> batch.outputs -> ledger apply -> positions updated
#   I3  batch.outputs -> risk -> risk_snapshots row + GetRiskSnapshot (GET /v1/positions margin block)
#   I4  GET /v1/positions aggregates ledger + risk
#   I5  WS push on new snapshot / margin call
#   I6  WS down -> frontend polling fallback
#
# This harness drives the RUNNING dev stack (infra/docker-compose.dev.yml) via
# the gateway HTTP API + direct PostgreSQL inspection. It does NOT mock Kafka/PG;
# it exercises the real matching->ledger->risk->gateway chain.
#
# WHAT RUNS HERE vs WHAT NEEDS THE FULL STACK
#   I1, I2, I3, I4 run against a running `docker compose -f
#   infra/docker-compose.dev.yml up` stack. They are the deterministic core of
#   F-06 (fill -> position -> snapshot -> aggregated GET).
#   I5 (WS push) and I6 (WS->polling fallback) are documented as SKIP unless a
#   WS smoke client is wired (frontend-driven, see Testing/f05_load_test.js
#   ws_load scenario for the WS endpoint shape). They are exercised by the
#   frontend integration suite and are out of scope for this shell harness;
#   I4 already proves the polling-source (GET /v1/positions) that I6 falls back to.
#
# Usage (run ON a host that can reach the dev stack, e.g. ubuntu-dev / 100.65.232.81):
#   bash Testing/f06_it_e2e.sh
# Env:
#   GATEWAY_URL    (default http://localhost:8088)
#   PG_CONTAINER   (default infra-postgres-1)
#   LEDGER_CONTAINER (default infra-ledger-1)
#   USER_ID        (default demo-user)
#   POLL_TIMEOUT   (default 90 seconds)
#   RUN_RESTART    (default 0; set 1 to run I1 which restarts the ledger container)
# ============================================================================

set -uo pipefail

GATEWAY_URL="${GATEWAY_URL:-http://localhost:8088}"
PG_CONTAINER="${PG_CONTAINER:-infra-postgres-1}"
LEDGER_CONTAINER="${LEDGER_CONTAINER:-infra-ledger-1}"
USER_ID="${USER_ID:-demo-user}"
POLL_TIMEOUT="${POLL_TIMEOUT:-90}"
RUN_RESTART="${RUN_RESTART:-0}"

pass=0
fail=0

log()  { printf '[F06-IT] %s\n' "$*"; }
ok()   { printf '[F06-IT]   PASS: %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf '[F06-IT]   FAIL: %s\n' "$*"; fail=$((fail+1)); }
skip() { printf '[F06-IT]   SKIP: %s\n' "$*"; }

pg() {
  docker exec "${PG_CONTAINER}" psql -U cex -d cex -t -A -c "$1" 2>/dev/null
}

# GET /v1/positions for USER_ID. Dev gateway resolves user from X-User-Id when
# no JWT middleware is configured.
get_positions() {
  curl -sS "${GATEWAY_URL}/v1/positions" -H "X-User-Id: ${USER_ID}" 2>/dev/null
}

jqp() { # jqp '<python expr on data dict>' <json>
  printf '%s' "$2" | python3 -c "import sys,json; data=json.load(sys.stdin); print($1)" 2>/dev/null
}

# ---------------------------------------------------------------------------
# I2 — full cycle: post a FlowOrder, then assert the positions table changed.
# ---------------------------------------------------------------------------
i2_full_cycle() {
  log "I2: FlowOrder -> batch.outputs -> ledger apply -> positions updated"
  local before
  before="$(pg "SELECT count(*) FROM positions WHERE user_id='${USER_ID}';")"
  before="${before:-0}"
  log "  positions rows for ${USER_ID} before: ${before}"

  local resp order_id
  resp="$(curl -sS -X POST "${GATEWAY_URL}/v1/flow-orders" \
            -H 'Content-Type: application/json' \
            -H "X-User-Id: ${USER_ID}" \
            -d '{"user_id":"'"${USER_ID}"'","symbol":"BTC/USDT","side":"buy","total_qty":0.02,"price_low":1000,"price_high":200000,"max_speed":0.02}' 2>/dev/null)"
  order_id="$(jqp 'data.get("order_id","")' "${resp}")"
  if [[ -z "${order_id}" ]]; then
    bad "I2: order not accepted (resp=${resp})"
    return
  fi
  log "  posted order ${order_id}; polling up to ${POLL_TIMEOUT}s for a position update"

  local deadline=$(( $(date +%s) + POLL_TIMEOUT ))
  local updated=0
  while [[ $(date +%s) -lt ${deadline} ]]; do
    # A non-flat BTC/USDT position with non-zero qty proves the fill applied.
    local q
    q="$(pg "SELECT quantity FROM positions WHERE user_id='${USER_ID}' AND symbol IN ('BTC/USDT','BTCUSDT') AND side <> 'flat' LIMIT 1;")"
    if [[ -n "${q}" ]] && python3 -c "import sys; sys.exit(0 if float('${q:-0}')>0 else 1)" 2>/dev/null; then
      updated=1
      log "  positions row: qty=${q}"
      break
    fi
    sleep 3
  done

  if [[ "${updated}" -eq 1 ]]; then
    ok "I2: positions updated after fill (matching->ledger chain live)"
  else
    bad "I2: no non-flat position within ${POLL_TIMEOUT}s (matching/fill idle in dev?)"
  fi
}

# ---------------------------------------------------------------------------
# I3 — risk_snapshots row written + readable via GetRiskSnapshot (margin block).
# ---------------------------------------------------------------------------
i3_risk_snapshot() {
  log "I3: batch.outputs -> risk -> risk_snapshots row + GetRiskSnapshot"
  local deadline=$(( $(date +%s) + POLL_TIMEOUT ))
  local snap=""
  while [[ $(date +%s) -lt ${deadline} ]]; do
    snap="$(pg "SELECT count(*) FROM risk_snapshots WHERE entity_id='${USER_ID}';")"
    snap="${snap:-0}"
    if [[ "${snap}" -ge 1 ]]; then break; fi
    sleep 3
  done
  if [[ "${snap}" -ge 1 ]]; then
    ok "I3: risk_snapshots rows for ${USER_ID} = ${snap}"
  else
    bad "I3: no risk_snapshots row for ${USER_ID} within ${POLL_TIMEOUT}s"
    return
  fi
  # latest snapshot must have a maintenance/initial margin column populated.
  local im
  im="$(pg "SELECT initial_margin FROM risk_snapshots WHERE entity_id='${USER_ID}' ORDER BY \"timestamp\" DESC LIMIT 1;")"
  if [[ -n "${im}" ]]; then
    ok "I3: latest risk_snapshot has initial_margin=${im}"
  else
    bad "I3: latest risk_snapshot missing initial_margin"
  fi
}

# ---------------------------------------------------------------------------
# I4 — GET /v1/positions aggregates ledger (positions+pnl) + risk (margin).
# ---------------------------------------------------------------------------
i4_aggregate_get() {
  log "I4: GET /v1/positions aggregates ledger + risk"
  local resp
  resp="$(get_positions)"
  if [[ -z "${resp}" ]]; then
    bad "I4: empty response from GET /v1/positions"
    return
  fi
  # positions[] and pnl{} must be present (ledger side, mandatory).
  if jqp '"positions" in data and "pnl" in data' "${resp}" | grep -qi true; then
    ok "I4: response has positions[] + pnl{} (ledger aggregation)"
  else
    bad "I4: response missing positions[]/pnl{} (resp=${resp})"
  fi
  # margin{} present (risk aggregation) OR explicit degraded flag (acceptable partial degradation).
  if jqp 'bool(data.get("margin"))' "${resp}" | grep -qi true; then
    ok "I4: response has margin{} block (risk aggregation)"
  elif jqp 'data.get("marginDegraded", False)' "${resp}" | grep -qi true; then
    ok "I4: margin degraded to null with marginDegraded=true (documented partial degradation)"
  else
    bad "I4: response has neither margin{} nor marginDegraded flag (resp=${resp})"
  fi
}

# ---------------------------------------------------------------------------
# I1 — persistence across ledger restart. Optional (restarts container).
# ---------------------------------------------------------------------------
i1_persistence() {
  if [[ "${RUN_RESTART}" -ne 1 ]]; then
    skip "I1 persistence: set RUN_RESTART=1 to restart ${LEDGER_CONTAINER} and re-check positions survive"
    return
  fi
  log "I1: ledger restart -> balances/positions survive"
  local before
  before="$(pg "SELECT count(*) FROM positions WHERE user_id='${USER_ID}';")"
  before="${before:-0}"
  docker restart "${LEDGER_CONTAINER}" >/dev/null 2>&1
  sleep 8
  local after
  after="$(pg "SELECT count(*) FROM positions WHERE user_id='${USER_ID}';")"
  after="${after:-0}"
  if [[ "${after}" -ge "${before}" && "${before}" -ge 1 ]]; then
    ok "I1: positions rows preserved across restart (${before} -> ${after})"
  else
    bad "I1: position rows not preserved (${before} -> ${after})"
  fi
}

# ---------------------------------------------------------------------------
# I5 / I6 — WS push + polling fallback (frontend-driven).
# ---------------------------------------------------------------------------
i56_skipped() {
  skip "I5 WS push on new snapshot/margin-call: requires a WS smoke client against /v1/stream. WS endpoint shape: see Testing/f05_load_test.js ws_load. Exercised by the frontend integration suite."
  skip "I6 WS down -> polling fallback: the polling source GET /v1/positions is proven by I4; the WS->poll switch is frontend logic (frontend/web/src/api/positionsService.js) covered by frontend tests."
}

# ---------------------------------------------------------------------------

main() {
  log "F-06 integration E2E starting (gateway=${GATEWAY_URL}, pg=${PG_CONTAINER}, user=${USER_ID})"

  if ! pg "SELECT 1;" >/dev/null 2>&1; then
    log "FATAL: cannot reach PostgreSQL via docker exec ${PG_CONTAINER}. Is the dev stack up?"
    log "  Start it with: cd infra && docker compose -f docker-compose.dev.yml up -d"
    exit 2
  fi

  i1_persistence
  i2_full_cycle
  i3_risk_snapshot
  i4_aggregate_get
  i56_skipped

  log "----------------------------------------"
  log "RESULT: ${pass} passed, ${fail} failed"
  if [[ "${fail}" -gt 0 ]]; then
    exit 1
  fi
  log "F-06 integration E2E: ALL ASSERTIONS PASS"
}

main "$@"
