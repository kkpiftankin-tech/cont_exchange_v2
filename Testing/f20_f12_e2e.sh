#!/usr/bin/env bash
# F-20 + F-12 end-to-end smoke — the real product path.
#
# Drives a FlowOrder through the live dev stack and observes the chain:
#   gateway POST /v1/flow-orders
#     -> order_flow.CreateFlowOrder
#     -> matching batch cycle (BATCH_INTERVAL_MS=5s)
#       -> fills + position snapshot
#       -> HedgeTriggerPolicy fires when |qty| >= 0.001 BTC (BTC/USDT config)
#         -> ExecutionIntent on `execution.intents`
#           -> venues exec_consume_loop
#             -> SimSessionRegistry says SIM_ONLY for binance/BTC/USDT
#               -> SimExecutionAssembler against the LIVE binance LOB
#                 -> ExecutionReport on `sim.execution.venue`  <- observed here
#
# Verification: snapshot the high-watermark of sim.execution.venue across all
# partitions, POST the flow order, then wait up to 60s for the high-watermark
# to grow. Any growth means a new sim ExecutionReport landed — the F-12 + Sim
# chain produced output end-to-end.
#
# Assumes the dev compose stack is already running (multi_real venues). Run
# Testing/f20_it_e2e.sh first to bring venues up if needed.
set -uo pipefail

PROJECT="${COMPOSE_PROJECT:-infra}"
NET="${PROBE_NET:-cex_net}"
ADMIN="http://venues:8087"
GATEWAY="http://gateway:8080"
RP="${RP_CONTAINER:-infra-redpanda-1}"
TOPIC="sim.execution.venue"
WAIT_S="${WAIT_S:-60}"
PASS=1
note() { echo "[f12-e2e] $*"; }
fail() { echo "[f12-e2e][FAIL] $*"; PASS=0; }

ccurl() { docker run --rm --network "${NET}" curlimages/curl:latest "$@"; }

# Sum of high-watermarks across all partitions of a topic.
hw_total() {
  docker exec "${RP}" rpk topic describe "$1" -p 2>/dev/null \
    | awk 'NR>1 {sum+=$NF} END {print sum+0}'
}

note "creating ACTIVE SimSession SIM_ONLY for binance/BTC/USDT"
SESSION_JSON='{"name":"f12-e2e","routingMode":"ROUTING_MODE_SIM_ONLY","scopeVenues":["binance"],"scopeInstruments":["BTC/USDT"],"latencyModel":{"distribution":"LATENCY_DISTRIBUTION_FIXED","p50Ms":5},"impactModel":{"modelType":"IMPACT_MODEL_TYPE_LEVEL_BY_LEVEL"},"staleLobThresholdMs":600000}'
CREATE=$(ccurl -sS -X POST "${ADMIN}/admin/v1/sim-sessions" -H 'Content-Type: application/json' -d "${SESSION_JSON}")
SID=$(printf '%s' "${CREATE}" | grep -o '"simSessionId"[^,]*' | head -1 | sed 's/.*: *"//; s/".*//')
[ -n "${SID}" ] || { fail "no simSessionId in create response: ${CREATE}"; exit 1; }
note "sim_session_id=${SID}"

# Give the sim.config consumer ~3s to apply.
sleep 3

HW_BEFORE=$(hw_total "${TOPIC}")
note "${TOPIC} high-watermark BEFORE = ${HW_BEFORE}"

# 0.002 BTC > F-12 trigger threshold 0.001 (HEDGE_TRIGGER_QTY_BTC_USDT).
# Realistic price band straddling the current binance ~$72k mid; matching
# accepts intents where price_low <= reference <= price_high.
note "POST FlowOrder via gateway (BUY 0.002 BTC, demo-user)"
ORDER_BODY='{"user_id":"demo-user","symbol":"BTC/USDT","side":"buy","total_qty":0.002,"price_low":71000,"price_high":73500,"max_speed":0.001}'
ORDER_RESP=$(ccurl -sS -X POST "${GATEWAY}/v1/flow-orders" -H 'Content-Type: application/json' -d "${ORDER_BODY}")
note "gateway response: ${ORDER_RESP}"
case "${ORDER_RESP}" in
  *'"accepted":true'*) note "FlowOrder accepted" ;;
  *) fail "FlowOrder rejected: ${ORDER_RESP}" ;;
esac

note "polling ${TOPIC} for ${WAIT_S}s waiting for high-watermark to grow"
HW_AFTER=${HW_BEFORE}
deadline=$(( $(date +%s) + WAIT_S ))
while [ "$(date +%s)" -lt "${deadline}" ]; do
  HW_AFTER=$(hw_total "${TOPIC}")
  if [ "${HW_AFTER}" -gt "${HW_BEFORE}" ]; then
    note "DETECTED new sim report (HW ${HW_BEFORE} -> ${HW_AFTER})"
    break
  fi
  sleep 2
done

if [ "${HW_AFTER}" -gt "${HW_BEFORE}" ]; then
  note "data-plane: F-12 hedge intent -> SIM_ONLY routing -> sim.execution.venue OK"
else
  fail "no new sim ExecutionReport within ${WAIT_S}s (HW still ${HW_AFTER})"
fi

# Diagnostics — last hedge-related lines from matching + venues regardless of result.
note "matching: last hedge-trigger / intent-publish log lines"
docker logs --since 2m infra-matching-1 2>&1 \
  | grep -iE "hedge_trigger|execution.intents|hedge intent published" | tail -5 \
  | sed 's/^/  /'

note "venues: last sim publish log lines"
docker logs --since 2m infra-venues-1 2>&1 \
  | grep -iE "publish_sim_execution|sim_router|received intent" | tail -5 \
  | sed 's/^/  /'

# Cleanup.
ccurl -sf -X POST "${ADMIN}/admin/v1/sim-sessions/${SID}/complete" >/dev/null \
  && note "session ${SID} completed" || true

echo "============================================================"
if [ "${PASS}" = 1 ]; then
  echo "[f12-e2e] RESULT: PASS"
  exit 0
fi
echo "[f12-e2e] RESULT: FAIL"
exit 1
