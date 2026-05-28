#!/usr/bin/env bash
# F-12 DoD-10 — Integration E2E (IT-1) over the running dev stack.
#
# IT-1 (full happy-path chain): a flow order that crosses external
# liquidity produces a net position -> matching's hedge_trigger_policy
# emits a real F-12 ExecutionIntent -> venues executes it -> publishes
# ExecutionReport to execution.venue -> the row lands in PG hedgeflows
# (status COMPLETED) + child_orders (FILLED) + ClickHouse
# execution_reports, and the ledger applies it.
#
# This drives the gateway HTTP API (not direct Kafka injection) so it
# exercises the real matching->planner->venues->ledger chain. The only
# nondeterminism is "did a hedge trigger at all" within the timeout,
# mitigated by an aggressive order + generous polling.
#
# IT-2 (partial+retry), IT-3 (rejection+fallback), IT-4 (timeout+
# UNDERFILLED) require FAULT INJECTION on the venue side (partial fill,
# forced reject, response timeout) which the current dev sim adapter
# does NOT support. Those scenarios are the domain of F-20 Live Venue
# Simulator (RejectionModel / LatencyModel / partialFillMode). They are
# defined here as explicit SKIP cases with the reason, so the suite is
# honest about coverage rather than silently passing.
#
# Usage (run ON the host that can reach the dev stack, e.g. ubuntu-dev):
#   bash Testing/f12_it_e2e.sh
# Env:
#   GATEWAY_URL   (default http://localhost:8088)
#   PG_CONTAINER  (default infra-postgres-1)
#   CH_URL        (default http://localhost:8123)
#   POLL_TIMEOUT  (default 90 seconds)

set -uo pipefail

GATEWAY_URL="${GATEWAY_URL:-http://localhost:8088}"
PG_CONTAINER="${PG_CONTAINER:-infra-postgres-1}"
CH_URL="${CH_URL:-http://localhost:8123}"
POLL_TIMEOUT="${POLL_TIMEOUT:-90}"
LEDGER_CONTAINER="${LEDGER_CONTAINER:-infra-ledger-1}"

pass=0
fail=0

log()  { printf '[F12-IT] %s\n' "$*"; }
ok()   { printf '[F12-IT]   PASS: %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf '[F12-IT]   FAIL: %s\n' "$*"; fail=$((fail+1)); }
skip() { printf '[F12-IT]   SKIP: %s\n' "$*"; }

pg() {
  docker exec "${PG_CONTAINER}" psql -U cex -d cex -t -A -c "$1" 2>/dev/null
}

ch() {
  curl -sS "${CH_URL}/?query=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$1")" 2>/dev/null
}

# ---------------------------------------------------------------------------
# IT-1
# ---------------------------------------------------------------------------
it1_full_cycle() {
  log "IT-1: full happy-path E2E (flow order -> hedge -> report -> PG/CH/ledger)"

  local before
  before="$(pg "SELECT count(*) FROM hedgeflows WHERE hedge_flow_id LIKE '%|hedge|%' AND status='COMPLETED';")"
  before="${before:-0}"
  log "  hedgeflows COMPLETED |hedge| rows before: ${before}"

  # Aggressive SELL so it crosses external liquidity within a batch and
  # leaves a net short -> hedge BUYs to flatten.
  local resp order_id
  resp="$(curl -sS -X POST "${GATEWAY_URL}/v1/flow-orders" \
            -H 'Content-Type: application/json' \
            -d '{"user_id":"demo-user","symbol":"BTC/USDT","side":"sell","total_qty":0.5,"price_low":70000,"price_high":90000,"max_speed":0.5}' 2>/dev/null)"
  order_id="$(printf '%s' "${resp}" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("order_id",""))' 2>/dev/null)"
  if [[ -z "${order_id}" ]]; then
    bad "IT-1: order not accepted (resp=${resp})"
    return
  fi
  log "  posted order ${order_id}; polling up to ${POLL_TIMEOUT}s for a new COMPLETED hedge flow"

  local deadline=$(( $(date +%s) + POLL_TIMEOUT ))
  local hedge_flow_id=""
  while [[ $(date +%s) -lt ${deadline} ]]; do
    hedge_flow_id="$(pg "SELECT hedge_flow_id FROM hedgeflows WHERE hedge_flow_id LIKE '%|hedge|%' AND status='COMPLETED' ORDER BY created_at DESC LIMIT 1;")"
    local now_count
    now_count="$(pg "SELECT count(*) FROM hedgeflows WHERE hedge_flow_id LIKE '%|hedge|%' AND status='COMPLETED';")"
    now_count="${now_count:-0}"
    if [[ -n "${hedge_flow_id}" && "${now_count}" -gt "${before}" ]]; then
      break
    fi
    sleep 3
  done

  if [[ -z "${hedge_flow_id}" ]]; then
    bad "IT-1: no new COMPLETED hedge flow within ${POLL_TIMEOUT}s (matching/fill may be idle in dev)"
    return
  fi
  ok "IT-1: hedge flow reached COMPLETED: ${hedge_flow_id}"

  # Assert aggregates on the hedgeflows row.
  local filled target
  filled="$(pg "SELECT filled_qty FROM hedgeflows WHERE hedge_flow_id='${hedge_flow_id}';")"
  target="$(pg "SELECT target_qty FROM hedgeflows WHERE hedge_flow_id='${hedge_flow_id}';")"
  if python3 -c "import sys; sys.exit(0 if float('${filled:-0}')>0 else 1)" 2>/dev/null; then
    ok "IT-1: hedgeflows.filled_qty > 0 (${filled} of ${target})"
  else
    bad "IT-1: hedgeflows.filled_qty not positive (${filled})"
  fi

  # child_orders FILLED for this hedge flow.
  local co_filled
  co_filled="$(pg "SELECT count(*) FROM child_orders WHERE hedge_flow_id='${hedge_flow_id}' AND status='FILLED';")"
  co_filled="${co_filled:-0}"
  if [[ "${co_filled}" -ge 1 ]]; then
    ok "IT-1: child_orders FILLED rows = ${co_filled}"
  else
    bad "IT-1: no FILLED child_orders for ${hedge_flow_id}"
  fi

  # ClickHouse execution_reports for this hedge flow.
  local ch_rows
  ch_rows="$(ch "SELECT count() FROM execution_reports WHERE hedge_flow_id = '${hedge_flow_id}'")"
  ch_rows="$(printf '%s' "${ch_rows}" | tr -d '[:space:]')"
  ch_rows="${ch_rows:-0}"
  if [[ "${ch_rows}" -ge 1 ]]; then
    ok "IT-1: ClickHouse execution_reports rows = ${ch_rows}"
  else
    bad "IT-1: no ClickHouse execution_reports for ${hedge_flow_id}"
  fi

  # Ledger applied the report (structured log within recent window).
  if docker logs "${LEDGER_CONTAINER}" --since 120s 2>&1 | grep -q "Ledger applied execution report"; then
    ok "IT-1: ledger applied execution report (log present)"
  else
    bad "IT-1: no 'Ledger applied execution report' in ledger logs (last 120s)"
  fi
}

# ---------------------------------------------------------------------------
# IT-2 / IT-3 / IT-4 — fault-injection scenarios (require F-20 sim models)
# ---------------------------------------------------------------------------
it234_skipped() {
  skip "IT-2 partial+retry: needs venue partial-fill injection (F-20 partialFillMode). Unit coverage: execute_on_venue_test U2."
  skip "IT-3 rejection+fallback: needs venue forced-reject (F-20 RejectionModel). Unit coverage: execute_on_venue_test U4 + matching planner fallback."
  skip "IT-4 timeout+UNDERFILLED+alert: needs venue response-timeout (F-20 LatencyModel timeoutMs). Unit coverage: execute_on_venue_test U5."
}

# ---------------------------------------------------------------------------

main() {
  log "F-12 integration E2E starting (gateway=${GATEWAY_URL}, pg=${PG_CONTAINER}, ch=${CH_URL})"

  # sanity: stack reachable
  if ! pg "SELECT 1;" >/dev/null 2>&1; then
    log "FATAL: cannot reach PostgreSQL via docker exec ${PG_CONTAINER}"
    exit 2
  fi

  it1_full_cycle
  it234_skipped

  log "----------------------------------------"
  log "RESULT: ${pass} passed, ${fail} failed"
  if [[ "${fail}" -gt 0 ]]; then
    exit 1
  fi
  log "F-12 IT-1 E2E: ALL ASSERTIONS PASS"
}

main "$@"
