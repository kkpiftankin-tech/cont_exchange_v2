#!/usr/bin/env bash
# F-20 + F-12 interactive demo wrapper.
#
# Brings up the dev stack with REAL exchange feeds (multi_real venues) and
# wraps the operator-facing actions needed to verify Sim + F-12 by hand.
#
# Usage: bash Testing/f20-demo.sh <command> [args]
# See `bash Testing/f20-demo.sh` for the command list.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="${COMPOSE_PROJECT:-infra}"
COMPOSE="docker compose -p ${PROJECT} -f ${ROOT}/infra/docker-compose.dev.yml -f ${ROOT}/Testing/docker-compose.f20-e2e.override.yml"
NET="${PROBE_NET:-cex_net}"
ADMIN="http://venues:8087"
GATEWAY="http://gateway:8080"
RP="${RP_CONTAINER:-infra-redpanda-1}"

ccurl() { docker run --rm --network "${NET}" curlimages/curl:latest "$@"; }
hw_total() {
  docker exec "${RP}" rpk topic describe "$1" -p 2>/dev/null \
    | awk 'NR>1 {sum+=$NF} END {print sum+0}'
}
parse_sid() {
  grep -o '"simSessionId"[^,]*' | head -1 | sed 's/.*: *"//; s/".*//'
}

# ----- commands -----

cmd_start() {
  echo "[demo] bringing up the full dev stack (--build to pick up local source)"
  ${COMPOSE} up -d --build || return 1
  echo "[demo] waiting for gateway + venues admin..."
  local up=0
  for _ in $(seq 1 60); do
    if ccurl -sf "${GATEWAY}/healthz" >/dev/null 2>&1 \
       && ccurl -sf "${ADMIN}/healthz" >/dev/null 2>&1; then
      up=1; break
    fi
    sleep 2
  done
  [ "${up}" = 1 ] || { echo "[demo] stack didn't come ready in 120s"; return 1; }
  echo "[demo] stack ready"
  echo ""
  cmd_status
}

cmd_status() {
  echo "=== containers (project ${PROJECT}) ==="
  docker ps --filter "name=${PROJECT}-" --format "table {{.Names}}\t{{.Status}}" | head -20
  echo ""
  echo "=== venues adapter mode ==="
  docker exec infra-venues-1 sh -c 'echo VENUES_ADAPTER_MODE=$VENUES_ADAPTER_MODE' 2>/dev/null \
    || echo "(venues container not found)"
  echo ""
  echo "=== live binance LOB (last cached best_bid/best_ask) ==="
  docker logs --tail 60 infra-venues-1 2>/dev/null \
    | grep -oE '"best_bid":"[0-9.]+","best_levels"|"best_bid":"[0-9.]+",.*"best_ask":"[0-9.]+"' \
    | tail -1
  echo ""
  echo "=== active SimSessions ==="
  ccurl -sS "${ADMIN}/admin/v1/sim-sessions?status=ACTIVE" 2>/dev/null \
    | sed -E 's/\}/}\n/g' | grep -E '"simSessionId"|"name"|"routingMode"' | head -20
  echo ""
  echo "=== topic high-watermarks ==="
  for t in execution.intents execution.venue sim.execution.venue sim.execution.annotations sim.config sim.alerts; do
    printf "  %-30s HW=%s\n" "$t" "$(hw_total $t)"
  done
}

cmd_sim_on() {
  local mode="${1:-SIM_ONLY}"
  case "${mode}" in
    SIM_ONLY|LIVE_ONLY|SHADOW) ;;
    *) echo "[demo] invalid mode '${mode}' (use SIM_ONLY / LIVE_ONLY / SHADOW)"; return 1 ;;
  esac
  echo "[demo] creating ${mode} SimSession for binance/BTC/USDT"
  local body='{"name":"demo-'"${mode}"'","routingMode":"ROUTING_MODE_'"${mode}"'","scopeVenues":["binance"],"scopeInstruments":["BTC/USDT"],"latencyModel":{"distribution":"LATENCY_DISTRIBUTION_FIXED","p50Ms":5},"impactModel":{"modelType":"IMPACT_MODEL_TYPE_LEVEL_BY_LEVEL"},"staleLobThresholdMs":600000}'
  local resp=$(ccurl -sS -X POST "${ADMIN}/admin/v1/sim-sessions" \
                 -H 'Content-Type: application/json' -d "${body}")
  local sid=$(printf '%s' "${resp}" | parse_sid)
  if [ -z "${sid}" ]; then
    echo "[demo] FAILED to create session — response:"
    echo "${resp}"
    return 1
  fi
  echo "[demo] sim_session_id=${sid}"
  echo "[demo] (give ~2 sec for sim.config to propagate to the router)"
}

cmd_sim_off() {
  local sid="${1:-}"
  if [ -z "${sid}" ]; then
    sid=$(ccurl -sS "${ADMIN}/admin/v1/sim-sessions?status=ACTIVE" 2>/dev/null | parse_sid)
  fi
  if [ -z "${sid}" ]; then
    echo "[demo] no active session to complete"; return 1
  fi
  echo "[demo] completing ${sid}"
  ccurl -sS -X POST "${ADMIN}/admin/v1/sim-sessions/${sid}/complete" >/dev/null
  echo "[demo] done"
}

cmd_order() {
  local qty="${1:-0.002}"
  echo "[demo] POST FlowOrder BUY ${qty} BTC via gateway"
  local resp=$(ccurl -sS -X POST "${GATEWAY}/v1/flow-orders" \
                 -H 'Content-Type: application/json' \
                 -d "{\"user_id\":\"demo-user\",\"symbol\":\"BTC/USDT\",\"side\":\"buy\",\"total_qty\":${qty},\"price_low\":71000,\"price_high\":73500,\"max_speed\":0.001}")
  echo "${resp}"
  case "${resp}" in
    *'"accepted":true'*)
      echo "[demo] order accepted; F-12 hedge fires on the NEXT matching batch (~5s)."
      echo "[demo] watch the sim.execution.venue HW grow:  bash Testing/f20-demo.sh topics"
      ;;
    *) echo "[demo] gateway rejected the order" ;;
  esac
}

cmd_watch() {
  echo "[demo] tailing matching + venues logs (Ctrl-C to stop)"
  ${COMPOSE} logs -f --tail 5 matching venues \
    | grep -iE "hedge_trigger|publish_sim_execution|Produced sim|received intent|Applied sim.config|Published auto-hedge|Built auto-hedge"
}

cmd_topics() {
  for t in execution.intents execution.venue sim.execution.venue sim.execution.annotations sim.config sim.alerts; do
    printf "%-32s HW=%s\n" "$t" "$(hw_total $t)"
  done
}

cmd_stop() {
  echo "[demo] stopping the stack"
  ${COMPOSE} down
}

# ----- dispatch -----

case "${1:-}" in
  start)            cmd_start ;;
  status)           cmd_status ;;
  sim-on|sim-create) shift; cmd_sim_on "${1:-SIM_ONLY}" ;;
  sim-shadow)       cmd_sim_on SHADOW ;;
  sim-off)          shift; cmd_sim_off "${1:-}" ;;
  order)            shift; cmd_order "${1:-0.002}" ;;
  watch)            cmd_watch ;;
  topics)           cmd_topics ;;
  stop)             cmd_stop ;;
  ""|-h|--help|help)
    cat <<USAGE
F-20 + F-12 interactive demo.

Usage: bash Testing/f20-demo.sh <command> [args]

Commands:
  start                 Bring up the dev stack with multi_real venues
                        (real binance/coinbase/uniswap LOB feeds).
  status                Containers, adapter mode, active SimSessions, topic HWs.
  sim-on  [MODE]        Activate a SimSession (default SIM_ONLY; also
                        LIVE_ONLY or SHADOW).
  sim-shadow            Shortcut for 'sim-on SHADOW'.
  sim-off [id]          Complete the active SimSession (or specified id).
  order   [qty]         POST a test FlowOrder via gateway (default 0.002 BTC).
                        Crosses the F-12 hedge threshold (notional ~\$140).
  watch                 Tail matching + venues logs filtered to the chain.
  topics                Show high-watermarks of all execution topics.
  stop                  docker compose down.

Typical session:
  1) bash Testing/f20-demo.sh start
  2) bash Testing/f20-demo.sh sim-on              # SIM_ONLY by default
  3) bash Testing/f20-demo.sh order               # POST 0.002 BTC BUY
  4) bash Testing/f20-demo.sh topics              # see sim.execution.venue grow
  5) bash Testing/f20-demo.sh sim-off

Endpoints (browse from the host or another container on cex_net):
  - Gateway:           http://localhost:8088  (mapped from gateway:8080)
  - Redpanda Console:  http://localhost:8080  (topic browser, schema)
  - Prometheus:        http://localhost:9090
USAGE
    ;;
  *) echo "unknown command: $1 (run without args for help)"; exit 1 ;;
esac
