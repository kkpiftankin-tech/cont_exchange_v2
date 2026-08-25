#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMPOSE="${ROOT_DIR}/infra/docker-compose.dev.yml"
OVERRIDE_COMPOSE="${ROOT_DIR}/Testing/.f11_test4.override.yml"
ARTIFACTS_DIR="${ROOT_DIR}/artifacts/f11_test4"

detect_parallelism() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
    return
  fi
  if command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN 2>/dev/null && return
  fi
  if command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu 2>/dev/null && return
  fi
  echo 1
}

BUILD_JOBS="${BUILD_JOBS:-$(detect_parallelism)}"
TEST_JOBS="${TEST_JOBS:-${BUILD_JOBS}}"
COMPOSE_PARALLEL_LIMIT="${COMPOSE_PARALLEL_LIMIT:-${BUILD_JOBS}}"
VERBOSE="${VERBOSE:-1}"
F11_TIMEOUT_SCALE="${F11_TIMEOUT_SCALE:-1}"
export BUILD_JOBS
export TEST_JOBS
export COMPOSE_PARALLEL_LIMIT
export VERBOSE
export F11_TIMEOUT_SCALE

mkdir -p "${ARTIFACTS_DIR}"
rm -rf "${ARTIFACTS_DIR:?}"/*

FAILS=0

compose() {
  if [[ "${VERBOSE}" == "1" ]]; then
    printf '[F11-TEST-4] RUN: docker compose -f %q -f %q' "${BASE_COMPOSE}" "${OVERRIDE_COMPOSE}" >&2
    for arg in "$@"; do
      printf ' %q' "${arg}" >&2
    done
    printf '\n' >&2
  fi
  docker compose -f "${BASE_COMPOSE}" -f "${OVERRIDE_COMPOSE}" "$@"
}

log() {
  printf '[F11-TEST-4] %s\n' "$*"
}

mark_fail() {
  printf '[F11-TEST-4][FAIL] %s\n' "$*" >&2
  FAILS=$((FAILS + 1))
}

retry_command() {
  local attempts="$1"
  local delay_sec="$2"
  shift 2

  local attempt=1
  local status=0
  while true; do
    if "$@"; then
      return 0
    fi
    status=$?
    if [[ "${attempt}" -ge "${attempts}" ]]; then
      return "${status}"
    fi
    log "Retrying command after failure (${attempt}/${attempts}): $*"
    sleep "${delay_sec}"
    attempt=$((attempt + 1))
  done
}

scaled_seconds() {
  local base="$1"
  local scale
  scale="$(tr -cd '0-9' <<<"${F11_TIMEOUT_SCALE}")"
  if [[ -z "${scale}" || "${scale}" -lt 1 ]]; then
    scale=1
  fi
  echo $((base * scale))
}

assert_delta_gt_zero() {
  local before="$1"
  local after="$2"
  local description="$3"
  before="$(tr -cd '0-9-' <<<"${before}")"
  after="$(tr -cd '0-9-' <<<"${after}")"
  if [[ -z "${before}" ]]; then before=0; fi
  if [[ -z "${after}" ]]; then after=0; fi
  local delta=$((after - before))
  if [[ "${delta}" -gt 0 ]]; then
    log "PASS: ${description} (delta=${delta})"
  else
    mark_fail "${description} (expected delta>0, before=${before}, after=${after})"
  fi
}

assert_delta_eq_zero() {
  local before="$1"
  local after="$2"
  local description="$3"
  before="$(tr -cd '0-9-' <<<"${before}")"
  after="$(tr -cd '0-9-' <<<"${after}")"
  if [[ -z "${before}" ]]; then before=0; fi
  if [[ -z "${after}" ]]; then after=0; fi
  local delta=$((after - before))
  if [[ "${delta}" -eq 0 ]]; then
    log "PASS: ${description} (delta=${delta})"
  else
    mark_fail "${description} (expected delta=0, before=${before}, after=${after})"
  fi
}

write_override() {
  cat >"${OVERRIDE_COMPOSE}" <<EOF
services:
  venues:
    environment:
EOF

  for kv in "$@"; do
    printf '      - "%s"\n' "${kv}" >>"${OVERRIDE_COMPOSE}"
  done

  cat >>"${OVERRIDE_COMPOSE}" <<EOF
  dex-mock:
    image: python:3.11-alpine
    command: ["python","-u","/app/f11_dex_mock.py"]
    environment:
      - "DEX_MOCK_MODE=healthy"
      - "DEX_MOCK_PORT=18081"
    volumes:
      - "${ROOT_DIR}/Testing/f11_dex_mock.py:/app/f11_dex_mock.py:ro"
EOF
}

wait_broker_ready() {
  local tries
  tries="$(scaled_seconds 60)"
  for _ in $(seq 1 "${tries}"); do
    if compose exec -T redpanda rpk topic list >/dev/null 2>&1; then
      return 0
    fi
    sleep 2
  done
  return 1
}

topic_hwm_sum() {
  local topic="$1"
  compose exec -T redpanda rpk topic describe "${topic}" -p 2>/dev/null \
    | awk 'NR > 1 {sum += $6} END {print sum + 0}'
}

group_offset_sum() {
  local group="$1"
  local topic="$2"
  local output=""
  output="$(compose exec -T redpanda rpk group describe "${group}" 2>/dev/null || true)"
  awk -v topic="${topic}" '$1 == topic && $3 ~ /^[0-9]+$/ {sum += $3} END {print sum + 0}' <<<"${output}"
}

wait_for_group_offset_growth() {
  local group="$1"
  local topic="$2"
  local before="$3"
  local timeout_sec="$4"
  local before_num
  before_num="$(tr -cd '0-9' <<<"${before}")"
  if [[ -z "${before_num}" ]]; then before_num=0; fi
  local after_num="${before_num}"
  for _ in $(seq 1 "${timeout_sec}"); do
    local after_raw
    after_raw="$(group_offset_sum "${group}" "${topic}")"
    local curr_num
    curr_num="$(tr -cd '0-9' <<<"${after_raw}")"
    if [[ -z "${curr_num}" ]]; then curr_num=0; fi
    if [[ "${curr_num}" -ge "${before_num}" ]]; then
      after_num="${curr_num}"
    fi
    if [[ "${curr_num}" -gt "${before_num}" ]]; then
      echo "${curr_num}"
      return 0
    fi
    sleep 1
  done
  echo "${after_num}"
  return 1
}

run_scenario() {
  local scenario="$1"
  local wait_sec="$2"
  shift 2

  local scenario_dir="${ARTIFACTS_DIR}/${scenario}"
  mkdir -p "${scenario_dir}"
  local since
  since="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  write_override "$@"
  retry_command 3 5 compose up -d --force-recreate --no-deps venues dex-mock
  sleep "$(scaled_seconds "${wait_sec}")"

  for svc in venues market_data matching risk dex-mock; do
    compose logs --since "${since}" "${svc}" >"${scenario_dir}/${svc}.log" 2>&1 || true
  done
}

cleanup() {
  rm -f "${OVERRIDE_COMPOSE}"
}
trap cleanup EXIT

log "Starting stand: Connector+Normalizer+CurveBuilder+Kafka+MDS+Matching+Risk"
log "Parallelism: BUILD_JOBS=${BUILD_JOBS}, TEST_JOBS=${TEST_JOBS}, COMPOSE_PARALLEL_LIMIT=${COMPOSE_PARALLEL_LIMIT}"
docker compose -f "${BASE_COMPOSE}" down -v --remove-orphans >/dev/null 2>&1 || true
write_override "VENUES_ADAPTER_MODE=simulated"
retry_command 3 5 compose up -d --build clickhouse redpanda topics-init venues market_data matching risk
if ! wait_broker_ready; then
  mark_fail "Broker redpanda did not become ready"
  exit 1
fi

# Exercise strict/relaxed gating on the mocked DEX path.
# Live-like simulated CEX books are intentionally smooth enough to stay publishable.
log "Scenario epsilon-thresholds (strict mock-dex -> no sustained publish)"
eps_strict_before_curve="$(topic_hwm_sum "venue.liquidity.fob")"
run_scenario "epsilon_strict" 12 \
  "VENUES_ADAPTER_MODE=dex_amm_rpc" \
  "DEX_SYNC_MODE=polling" \
  "DEX_RPC_URL=http://dex-mock:18081" \
  "DEX_POOL_ADDRESS=0xpool" \
  "DEX_CHAIN_ID=eth-mainnet" \
  "DEX_POLLING_INTERVAL_FAST_MS=300" \
  "DEX_POLLING_INTERVAL_SLOW_MS=300" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=true" \
  "CURVE_EPSILON1_DEGRADE=0.000001" \
  "CURVE_EPSILON1_DISABLE=0.000002" \
  "CURVE_EPSILON2_DEGRADE=0.000001" \
  "CURVE_EPSILON2_DISABLE=0.000002" \
  "CURVE_EPSILON3_DEGRADE=0.000001" \
  "CURVE_EPSILON3_DISABLE=0.000002"
eps_strict_after_curve="$(topic_hwm_sum "venue.liquidity.fob")"
sleep "$(scaled_seconds 8)"
eps_strict_late_curve="$(topic_hwm_sum "venue.liquidity.fob")"
assert_delta_eq_zero "${eps_strict_after_curve}" "${eps_strict_late_curve}" \
  "Strict epsilon thresholds block sustained venue.liquidity.fob publication"
if [[ "${eps_strict_after_curve}" -gt "${eps_strict_before_curve}" ]]; then
  log "INFO: strict thresholds allowed short transition publish before OFF (accepted)"
fi

log "Scenario epsilon-thresholds (relaxed mock-dex -> publish allowed)"
eps_relaxed_before_curve="$(topic_hwm_sum "venue.liquidity.fob")"
run_scenario "epsilon_relaxed" 12 \
  "VENUES_ADAPTER_MODE=dex_amm_rpc" \
  "DEX_SYNC_MODE=polling" \
  "DEX_RPC_URL=http://dex-mock:18081" \
  "DEX_POOL_ADDRESS=0xpool" \
  "DEX_CHAIN_ID=eth-mainnet" \
  "DEX_POLLING_INTERVAL_FAST_MS=300" \
  "DEX_POLLING_INTERVAL_SLOW_MS=300" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=true" \
  "CURVE_EPSILON1_DEGRADE=1000000" \
  "CURVE_EPSILON1_DISABLE=1000001" \
  "CURVE_EPSILON2_DEGRADE=1000000" \
  "CURVE_EPSILON2_DISABLE=1000001" \
  "CURVE_EPSILON3_DEGRADE=1000000" \
  "CURVE_EPSILON3_DISABLE=1000001"
eps_relaxed_after_curve="$(topic_hwm_sum "venue.liquidity.fob")"
assert_delta_gt_zero "${eps_relaxed_before_curve}" "${eps_relaxed_after_curve}" \
  "Relaxed epsilon thresholds allow venue.liquidity.fob publication"

log "Scenario compare: direct curve consumption"
direct_before_curve_topic="$(topic_hwm_sum "venue.liquidity.fob")"
direct_before_matching_curve="$(group_offset_sum "matching" "venue.liquidity.fob")"
run_scenario "direct_curve" 12 \
  "VENUES_ADAPTER_MODE=simulated" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=false" \
  "CURVE_SYNTHETIC_ENABLED=false"
direct_after_curve_topic="$(topic_hwm_sum "venue.liquidity.fob")"
direct_after_matching_curve="$(wait_for_group_offset_growth "matching" "venue.liquidity.fob" "${direct_before_matching_curve}" "$(scaled_seconds 25)" || true)"
assert_delta_gt_zero "${direct_before_curve_topic}" "${direct_after_curve_topic}" \
  "Direct mode publishes venue.liquidity.fob"
assert_delta_gt_zero "${direct_before_matching_curve}" "${direct_after_matching_curve}" \
  "Direct mode consumed by Matching via venue.liquidity.fob"

log "Scenario compare: synthetic compatibility consumption"
synth_before_topic="$(topic_hwm_sum "venue.synthetic")"
synth_before_risk_group="$(group_offset_sum "risk" "venue.synthetic")"
synth_before_curve_topic="$(topic_hwm_sum "venue.liquidity.fob")"
run_scenario "synthetic_compat" 12 \
  "VENUES_ADAPTER_MODE=simulated" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=false" \
  "CURVE_SYNTHETIC_ENABLED=true" \
  "VENUE_SYNTHETIC_TOPIC=venue.synthetic"
synth_after_topic="$(topic_hwm_sum "venue.synthetic")"
synth_after_risk_group="$(wait_for_group_offset_growth "risk" "venue.synthetic" "${synth_before_risk_group}" "$(scaled_seconds 25)" || true)"
synth_after_curve_topic="$(topic_hwm_sum "venue.liquidity.fob")"
assert_delta_gt_zero "${synth_before_topic}" "${synth_after_topic}" \
  "Synthetic compatibility publishes venue.synthetic"
assert_delta_gt_zero "${synth_before_risk_group}" "${synth_after_risk_group}" \
  "Synthetic compatibility consumed by Risk via venue.synthetic"
assert_delta_gt_zero "${synth_before_curve_topic}" "${synth_after_curve_topic}" \
  "Synthetic compatibility keeps direct venue.liquidity.fob path available"

if [[ "${FAILS}" -ne 0 ]]; then
  log "FAILED with ${FAILS} assertion(s). Artifacts: ${ARTIFACTS_DIR}"
  exit 1
fi

log "PASSED. Artifacts: ${ARTIFACTS_DIR}"
