#!/usr/bin/env bash
# F-20 DoD-10 — end-to-end smoke for the Live Venue Simulator.
#
# Control-plane: create a SIM_ONLY SimSession via the venues Admin REST API
# (proves transport + PostgresSimSessionRepository + sim.config producer), and
# confirm the in-process consumer applied it (proves the hot-reload loop).
# Data-plane: produce an ExecutionIntent and assert the report is routed to
# sim.execution.venue and NOT execution.venue (proves the SIM/SHADOW fork).
#
# All HTTP and Kafka access happens INSIDE the compose network via sidecar
# containers (curl, the venues-f20-probe image), so nothing is published to the
# host and there is no port clash with a running dev stack.
#
# Requires: docker compose, a built `venues-f20-probe` image (Dockerfile.service
# TARGET=venues_f20_e2e_probe). Run from anywhere; paths are resolved.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="${COMPOSE_PROJECT:-infra}"
COMPOSE="docker compose -p ${PROJECT} -f ${ROOT}/infra/docker-compose.dev.yml -f ${ROOT}/Testing/docker-compose.f20-e2e.override.yml"
NET="${PROBE_NET:-cex_net}"
PROBE_IMAGE="${PROBE_IMAGE:-venues-f20-probe}"
ADMIN="http://venues:8087"
PASS=1
note() { echo "[f20-e2e] $*"; }
fail() { echo "[f20-e2e][FAIL] $*"; PASS=0; }

# curl run inside the compose network (reaches the venues service by name).
ccurl() { docker run --rm --network "${NET}" curlimages/curl:latest "$@"; }

note "bringing up redpanda + postgres + topics-init + venues (simulated mode, --build to pick up latest source)"
${COMPOSE} up -d --build redpanda postgres topics-init venues || { fail "compose up failed"; exit 1; }

note "waiting for venues admin API (${ADMIN}/healthz)"
up=0
for _ in $(seq 1 60); do
  if ccurl -sf "${ADMIN}/healthz" >/dev/null 2>&1; then up=1; break; fi
  sleep 2
done
[ "${up}" = 1 ] || { fail "venues admin not reachable"; ${COMPOSE} logs --tail=40 venues; exit 1; }
note "venues admin up"

# --- control-plane: create a SIM_ONLY session for binance / BTC/USDT ---
SESSION_JSON='{"name":"f20-e2e","routingMode":"ROUTING_MODE_SIM_ONLY","scopeVenues":["binance"],"scopeInstruments":["BTC/USDT"],"latencyModel":{"distribution":"LATENCY_DISTRIBUTION_FIXED","p50Ms":5},"impactModel":{"modelType":"IMPACT_MODEL_TYPE_LEVEL_BY_LEVEL"},"staleLobThresholdMs":600000}'
CREATE=$(ccurl -sS -X POST "${ADMIN}/admin/v1/sim-sessions" -H 'Content-Type: application/json' -d "${SESSION_JSON}")
note "create response: ${CREATE}"
SID=$(printf '%s' "${CREATE}" | grep -o '"simSessionId"[^,]*' | head -1 | sed 's/.*: *"//; s/".*//')
[ -n "${SID}" ] || fail "no simSessionId in create response"

if [ -n "${SID}" ]; then
  note "created sim_session_id=${SID}"
  ccurl -sf "${ADMIN}/admin/v1/sim-sessions/${SID}" >/dev/null && note "GET ok" || fail "GET session failed"
  ccurl -sf "${ADMIN}/admin/v1/sim-sessions?status=ACTIVE" >/dev/null && note "LIST ok" || fail "LIST failed"
fi

note "checking sim.config round-trip in venues logs (consumer applied event)"
applied=0
for _ in $(seq 1 30); do
  if ${COMPOSE} logs venues 2>&1 | grep -q "Applied sim.config event"; then applied=1; break; fi
  sleep 1
done
[ "${applied}" = 1 ] && note "sim.config applied by consumer (hot-reload loop ok)" || fail "no 'Applied sim.config event' in venues logs"

# --- data-plane: intent -> sim.execution.venue (not execution.venue) ---
note "running data-plane probe (network=${NET})"
if docker run --rm --network "${NET}" -e KAFKA_BROKERS=redpanda:9092 -e F20_PROBE_TIMEOUT_MS=25000 "${PROBE_IMAGE}"; then
  note "data-plane probe PASSED"
else
  fail "data-plane probe failed"
fi

# --- lifecycle: complete the session (publishes sim.config DELETE) ---
if [ -n "${SID}" ]; then
  ccurl -sf -X POST "${ADMIN}/admin/v1/sim-sessions/${SID}/complete" >/dev/null && note "complete ok" || fail "complete failed"
fi

echo "============================================================"
if [ "${PASS}" = 1 ]; then
  echo "[f20-e2e] RESULT: PASS"
  exit 0
fi
echo "[f20-e2e] RESULT: FAIL"
exit 1
