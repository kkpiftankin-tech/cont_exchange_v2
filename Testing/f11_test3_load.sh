#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMPOSE="${ROOT_DIR}/infra/docker-compose.dev.yml"
OVERRIDE_COMPOSE="${ROOT_DIR}/Testing/.f11_test3.override.yml"
ARTIFACTS_DIR="${ROOT_DIR}/artifacts/f11_test3"

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

F11_LOAD_VENUES="${F11_LOAD_VENUES:-10}"
F11_LOAD_SNAPSHOTS_PER_SEC_PER_VENUE="${F11_LOAD_SNAPSHOTS_PER_SEC_PER_VENUE:-20}"
F11_LOAD_WARMUP_SEC="${F11_LOAD_WARMUP_SEC:-12}"
F11_LOAD_MEASURE_SEC="${F11_LOAD_MEASURE_SEC:-30}"
F11_LOAD_SAMPLE_SEC="${F11_LOAD_SAMPLE_SEC:-2}"
F11_LOAD_LAG_LIMIT="${F11_LOAD_LAG_LIMIT:-200}"
F11_LOAD_P95_L1_L2_LIMIT_MS="${F11_LOAD_P95_L1_L2_LIMIT_MS:-80}"
F11_LOAD_P95_L3_LIMIT_MS="${F11_LOAD_P95_L3_LIMIT_MS:-150}"
F11_LOAD_P95_SNAPSHOT_LIMIT_MS="${F11_LOAD_P95_SNAPSHOT_LIMIT_MS:-500}"
F11_LOAD_MAX_CPU_PCT="${F11_LOAD_MAX_CPU_PCT:-1200}"
F11_LOAD_MAX_RAM_MB="${F11_LOAD_MAX_RAM_MB:-8192}"
F11_LOAD_MIN_TOTAL_TPS="${F11_LOAD_MIN_TOTAL_TPS:-100}"
F11_LOAD_TRACE_MARKET_DATA="${F11_LOAD_TRACE_MARKET_DATA:-1}"

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
    printf '[F11-TEST-3] RUN: docker compose -f %q -f %q' "${BASE_COMPOSE}" "${OVERRIDE_COMPOSE}" >&2
    for arg in "$@"; do
      printf ' %q' "${arg}" >&2
    done
    printf '\n' >&2
  fi
  docker compose -f "${BASE_COMPOSE}" -f "${OVERRIDE_COMPOSE}" "$@"
}

log() {
  printf '[F11-TEST-3] %s\n' "$*"
}

mark_fail() {
  printf '[F11-TEST-3][FAIL] %s\n' "$*" >&2
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

loop_interval_ms() {
  local sps="$1"
  if [[ "${sps}" -le 0 ]]; then
    echo 50
    return
  fi
  local ms=$((1000 / sps))
  if [[ "${ms}" -lt 1 ]]; then
    ms=1
  fi
  echo "${ms}"
}

venue_service_name() {
  local idx="$1"
  if [[ "${idx}" -eq 1 ]]; then
    echo "venues"
  else
    printf 'venues%02d\n' "${idx}"
  fi
}

venue_services() {
  local i
  for ((i = 1; i <= F11_LOAD_VENUES; ++i)); do
    venue_service_name "${i}"
  done
}

write_override() {
  local level="$1"
  local poll_ms="$2"
  local i

  cat >"${OVERRIDE_COMPOSE}" <<EOF
services:
  venues:
    environment:
      - "VENUES_ADAPTER_MODE=simulated"
      - "SIMULATED_VENUE_ID=venue01"
      - "CURVE_LEVEL=${level}"
      - "CURVE_QUALITY_GATING_ENABLED=false"
      - "VENUES_MD_LOOP_INTERVAL_MS=${poll_ms}"
      - "VENUES_SNAPSHOT_LOG_LATENCY=1"
  market_data:
    environment:
      - "MARKET_DATA_TRACE_EACH_MESSAGE=${F11_LOAD_TRACE_MARKET_DATA}"
EOF

  for ((i = 2; i <= F11_LOAD_VENUES; ++i)); do
    local svc
    local venue_id
    svc="$(venue_service_name "${i}")"
    venue_id="$(printf 'venue%02d' "${i}")"
    cat >>"${OVERRIDE_COMPOSE}" <<EOF
  ${svc}:
    build:
      context: ..
      dockerfile: docker/Dockerfile.service
      args:
        TARGET: venues
        BUILD_JOBS: \${BUILD_JOBS:-1}
    env_file: [ ./env/.env-example ]
    depends_on:
      topics-init:
        condition: service_completed_successfully
    environment:
      - "VENUES_ADAPTER_MODE=simulated"
      - "SIMULATED_VENUE_ID=${venue_id}"
      - "CURVE_LEVEL=${level}"
      - "CURVE_QUALITY_GATING_ENABLED=false"
      - "VENUES_MD_LOOP_INTERVAL_MS=${poll_ms}"
      - "VENUES_SNAPSHOT_LOG_LATENCY=1"
EOF
  done
}

dump_marketdata_diagnostics() {
  local level="$1"
  local diag_file="${ARTIFACTS_DIR}/${level}/marketdata_diagnostics.txt"
  {
    echo "=== docker compose ps ==="
    compose ps || true
    echo
    echo "=== redpanda group describe marketdata ==="
    compose exec -T redpanda rpk group describe marketdata || true
    echo
    echo "=== redpanda group describe matching ==="
    compose exec -T redpanda rpk group describe matching || true
    echo
    echo "=== redpanda group describe risk ==="
    compose exec -T redpanda rpk group describe risk || true
    echo
    echo "=== topic describe venue.snapshots ==="
    compose exec -T redpanda rpk topic describe venue.snapshots -p || true
    echo
    echo "=== topic describe venue.liquidity.fob ==="
    compose exec -T redpanda rpk topic describe venue.liquidity.fob -p || true
    echo
    echo "=== logs: market_data (tail 400) ==="
    compose logs --tail 400 market_data || true
    echo
    echo "=== logs: venues (tail 200) ==="
    compose logs --tail 200 venues || true
    echo
    echo "=== logs: matching (tail 200) ==="
    compose logs --tail 200 matching || true
    echo
    echo "=== logs: risk (tail 200) ==="
    compose logs --tail 200 risk || true
  } >"${diag_file}" 2>&1
  log "INFO: wrote diagnostics ${diag_file}"
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

docker_stats_sample() {
  local ids=("$@")
  if [[ "${#ids[@]}" -eq 0 ]]; then
    return 0
  fi
  docker stats --no-stream --format '{{.CPUPerc}} {{.MemUsage}}' "${ids[@]}" 2>/dev/null || true
}

extract_cpu_numeric() {
  local text="$1"
  tr -d '%' <<<"${text}" | tr -d ' ' | awk '{print $1 + 0.0}'
}

mem_to_mb() {
  local token="$1"
  awk -v token="${token}" '
    BEGIN {
      x = token
      gsub(/ /, "", x)
      if (x == "" || x == "0B") { print 0; exit }
      num = x
      gsub(/[^0-9.]/, "", num)
      unit = x
      gsub(/[0-9.]/, "", unit)
      unit = tolower(unit)
      if (unit == "b")      m = num / (1024.0 * 1024.0)
      else if (unit == "kib") m = num / 1024.0
      else if (unit == "kb")  m = num / 1000.0
      else if (unit == "mib") m = num
      else if (unit == "mb")  m = num
      else if (unit == "gib") m = num * 1024.0
      else if (unit == "gb")  m = num * 1000.0
      else if (unit == "tib") m = num * 1024.0 * 1024.0
      else if (unit == "tb")  m = num * 1000.0 * 1000.0
      else m = num
      printf "%.3f\n", m
    }'
}

percentile_95_from_file() {
  local file="$1"
  local lines
  lines="$(wc -l < "${file}" | tr -d ' ')"
  if [[ "${lines}" -le 0 ]]; then
    echo ""
    return
  fi
  local rank=$(( (lines * 95 + 99) / 100 ))
  sort -n "${file}" | sed -n "${rank}p"
}

collect_build_latency_samples() {
  local since="$1"
  local out_file="$2"
  : >"${out_file}"
  local svc
  for svc in $(venue_services); do
    compose logs --since "${since}" "${svc}" 2>/dev/null || true
  done | grep 'Published venue.liquidity.fob' \
      | sed -n 's/.*"build_latency_ms":"\{0,1\}\([0-9.][0-9.]*\)".*/\1/p' \
      >>"${out_file}" || true
}

collect_snapshot_latency_samples() {
  local since="$1"
  local out_file="$2"
  : >"${out_file}"
  local svc
  for svc in $(venue_services); do
    compose logs --since "${since}" "${svc}" 2>/dev/null || true
  done | grep 'Published venue.snapshot' \
      | sed -n 's/.*"snapshot_latency_ms":"\{0,1\}\([0-9.][0-9.]*\)".*/\1/p' \
      >>"${out_file}" || true
}

assert_le() {
  local value="$1"
  local limit="$2"
  local description="$3"
  if awk -v v="${value}" -v l="${limit}" 'BEGIN { exit !(v <= l) }'; then
    log "PASS: ${description} (value=${value}, limit=${limit})"
  else
    mark_fail "${description} (value=${value}, limit=${limit})"
  fi
}

assert_ge() {
  local value="$1"
  local limit="$2"
  local description="$3"
  if awk -v v="${value}" -v l="${limit}" 'BEGIN { exit !(v >= l) }'; then
    log "PASS: ${description} (value=${value}, limit=${limit})"
  else
    mark_fail "${description} (value=${value}, limit=${limit})"
  fi
}

run_level_load_test() {
  local level="$1"
  local p95_limit_ms="$2"
  local poll_ms="$3"
  local level_dir="${ARTIFACTS_DIR}/${level}"
  mkdir -p "${level_dir}"

  local since
  since="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  write_override "${level}" "${poll_ms}"
  compose up -d --force-recreate --no-deps $(venue_services)

  local warmup
  warmup="$(scaled_seconds "${F11_LOAD_WARMUP_SEC}")"
  local measure
  measure="$(scaled_seconds "${F11_LOAD_MEASURE_SEC}")"
  local sample_sec
  sample_sec="$(scaled_seconds "${F11_LOAD_SAMPLE_SEC}")"
  if [[ "${sample_sec}" -lt 1 ]]; then sample_sec=1; fi
  local samples=$((measure / sample_sec))
  if [[ "${samples}" -lt 1 ]]; then samples=1; fi

  log "Scenario ${level}: warmup=${warmup}s measure=${measure}s sample=${sample_sec}s venues=${F11_LOAD_VENUES}"
  sleep "${warmup}"

  local snap_before fob_before mds_snap_before mds_fob_before matching_fob_before risk_fob_before
  snap_before="$(topic_hwm_sum "venue.snapshots")"
  fob_before="$(topic_hwm_sum "venue.liquidity.fob")"
  mds_snap_before="$(group_offset_sum "marketdata" "venue.snapshots")"
  mds_fob_before="$(group_offset_sum "marketdata" "venue.liquidity.fob")"
  matching_fob_before="$(group_offset_sum "matching" "venue.liquidity.fob")"
  risk_fob_before="$(group_offset_sum "risk" "venue.liquidity.fob")"

  local max_lag=0
  local max_cpu=0
  local max_ram_mb=0
  local ids=()
  local svc
  for svc in market_data matching risk $(venue_services); do
    local id
    id="$(compose ps -q "${svc}" 2>/dev/null || true)"
    if [[ -n "${id}" ]]; then
      ids+=("${id}")
    fi
  done

  local i
  for ((i = 0; i < samples; ++i)); do
    local hwm_snap hwm_fob
    local off_mds_snap off_mds_fob off_matching_fob off_risk_fob
    local lag_mds_snap lag_mds_fob lag_matching_fob lag_risk_fob
    hwm_snap="$(topic_hwm_sum "venue.snapshots")"
    hwm_fob="$(topic_hwm_sum "venue.liquidity.fob")"
    off_mds_snap="$(group_offset_sum "marketdata" "venue.snapshots")"
    off_mds_fob="$(group_offset_sum "marketdata" "venue.liquidity.fob")"
    off_matching_fob="$(group_offset_sum "matching" "venue.liquidity.fob")"
    off_risk_fob="$(group_offset_sum "risk" "venue.liquidity.fob")"

    lag_mds_snap=$((hwm_snap - off_mds_snap))
    lag_mds_fob=$((hwm_fob - off_mds_fob))
    lag_matching_fob=$((hwm_fob - off_matching_fob))
    lag_risk_fob=$((hwm_fob - off_risk_fob))
    for lag in "${lag_mds_snap}" "${lag_mds_fob}" "${lag_matching_fob}" "${lag_risk_fob}"; do
      if [[ "${lag}" -gt "${max_lag}" ]]; then
        max_lag="${lag}"
      fi
    done

    while IFS= read -r stats_line; do
      [[ -z "${stats_line}" ]] && continue
      local cpu_token mem_used cpu_val mem_mb
      cpu_token="$(awk '{print $1}' <<<"${stats_line}")"
      mem_used="$(awk '{print $2}' <<<"${stats_line}")"
      cpu_val="$(extract_cpu_numeric "${cpu_token}")"
      mem_mb="$(mem_to_mb "${mem_used}")"
      if awk -v a="${cpu_val}" -v b="${max_cpu}" 'BEGIN { exit !(a > b) }'; then
        max_cpu="${cpu_val}"
      fi
      if awk -v a="${mem_mb}" -v b="${max_ram_mb}" 'BEGIN { exit !(a > b) }'; then
        max_ram_mb="${mem_mb}"
      fi
    done < <(docker_stats_sample "${ids[@]}")

    sleep "${sample_sec}"
  done

  local snap_after fob_after mds_snap_after mds_fob_after matching_fob_after risk_fob_after
  snap_after="$(topic_hwm_sum "venue.snapshots")"
  fob_after="$(topic_hwm_sum "venue.liquidity.fob")"
  mds_snap_after="$(group_offset_sum "marketdata" "venue.snapshots")"
  mds_fob_after="$(group_offset_sum "marketdata" "venue.liquidity.fob")"
  matching_fob_after="$(group_offset_sum "matching" "venue.liquidity.fob")"
  risk_fob_after="$(group_offset_sum "risk" "venue.liquidity.fob")"

  local snap_delta fob_delta
  snap_delta=$((snap_after - snap_before))
  fob_delta=$((fob_after - fob_before))
  local snap_tps fob_tps
  snap_tps="$(awk -v d="${snap_delta}" -v s="${measure}" 'BEGIN { printf "%.3f", (s > 0 ? d / s : 0.0) }')"
  fob_tps="$(awk -v d="${fob_delta}" -v s="${measure}" 'BEGIN { printf "%.3f", (s > 0 ? d / s : 0.0) }')"

  local latency_file="${level_dir}/build_latency_ms.txt"
  collect_build_latency_samples "${since}" "${latency_file}"
  local p95_ms
  p95_ms="$(percentile_95_from_file "${latency_file}")"
  if [[ -z "${p95_ms}" ]]; then
    mark_fail "${level}: build latency samples are empty"
    p95_ms=0
  fi

  local snapshot_latency_file="${level_dir}/snapshot_latency_ms.txt"
  collect_snapshot_latency_samples "${since}" "${snapshot_latency_file}"
  local snapshot_p95_ms
  snapshot_p95_ms="$(percentile_95_from_file "${snapshot_latency_file}")"
  if [[ -z "${snapshot_p95_ms}" ]]; then
    mark_fail "${level}: snapshot latency samples are empty"
    snapshot_p95_ms=0
  fi

  {
    echo "level=${level}"
    echo "venues=${F11_LOAD_VENUES}"
    echo "requested_snapshots_per_sec_per_venue=${F11_LOAD_SNAPSHOTS_PER_SEC_PER_VENUE}"
    echo "poll_interval_ms=${poll_ms}"
    echo "measure_seconds=${measure}"
    echo "topic_snapshots_before=${snap_before}"
    echo "topic_snapshots_after=${snap_after}"
    echo "topic_fob_before=${fob_before}"
    echo "topic_fob_after=${fob_after}"
    echo "snapshot_delta=${snap_delta}"
    echo "fob_delta=${fob_delta}"
    echo "snapshot_tps=${snap_tps}"
    echo "fob_tps=${fob_tps}"
    echo "marketdata_snapshots_offset_before=${mds_snap_before}"
    echo "marketdata_snapshots_offset_after=${mds_snap_after}"
    echo "marketdata_fob_offset_before=${mds_fob_before}"
    echo "marketdata_fob_offset_after=${mds_fob_after}"
    echo "matching_fob_offset_before=${matching_fob_before}"
    echo "matching_fob_offset_after=${matching_fob_after}"
    echo "risk_fob_offset_before=${risk_fob_before}"
    echo "risk_fob_offset_after=${risk_fob_after}"
    echo "max_kafka_consumer_lag=${max_lag}"
    echo "p95_build_latency_ms=${p95_ms}"
    echo "p95_snapshot_latency_ms=${snapshot_p95_ms}"
    echo "max_cpu_pct=${max_cpu}"
    echo "max_ram_mb=${max_ram_mb}"
  } > "${level_dir}/summary.txt"

  assert_ge "${snap_tps}" "${F11_LOAD_MIN_TOTAL_TPS}" "${level}: total venue.snapshots throughput"
  assert_le "${max_lag}" "${F11_LOAD_LAG_LIMIT}" "${level}: max Kafka consumer lag"
  assert_le "${p95_ms}" "${p95_limit_ms}" "${level}: p95 FOB build latency"
  assert_le "${snapshot_p95_ms}" "${F11_LOAD_P95_SNAPSHOT_LIMIT_MS}" "${level}: p95 VenueSnapshot latency"
  assert_le "${max_cpu}" "${F11_LOAD_MAX_CPU_PCT}" "${level}: max CPU usage (%)"
  assert_le "${max_ram_mb}" "${F11_LOAD_MAX_RAM_MB}" "${level}: max RAM usage (MB)"

  local dumped_marketdata_diag=0
  if [[ "${mds_snap_after}" -le "${mds_snap_before}" ]]; then
    mark_fail "${level}: MarketData did not consume venue.snapshots"
    if [[ "${dumped_marketdata_diag}" -eq 0 ]]; then
      dump_marketdata_diagnostics "${level}"
      dumped_marketdata_diag=1
    fi
  else
    log "PASS: ${level}: MarketData consumed venue.snapshots (delta=$((mds_snap_after - mds_snap_before)))"
  fi
  if [[ "${mds_fob_after}" -le "${mds_fob_before}" ]]; then
    mark_fail "${level}: MarketData did not consume venue.liquidity.fob"
    if [[ "${dumped_marketdata_diag}" -eq 0 ]]; then
      dump_marketdata_diagnostics "${level}"
      dumped_marketdata_diag=1
    fi
  else
    log "PASS: ${level}: MarketData consumed venue.liquidity.fob (delta=$((mds_fob_after - mds_fob_before)))"
  fi
  if [[ "${matching_fob_after}" -le "${matching_fob_before}" ]]; then
    mark_fail "${level}: Matching did not consume venue.liquidity.fob"
  else
    log "PASS: ${level}: Matching consumed venue.liquidity.fob (delta=$((matching_fob_after - matching_fob_before)))"
  fi
  if [[ "${risk_fob_after}" -le "${risk_fob_before}" ]]; then
    mark_fail "${level}: Risk did not consume venue.liquidity.fob"
  else
    log "PASS: ${level}: Risk consumed venue.liquidity.fob (delta=$((risk_fob_after - risk_fob_before)))"
  fi
}

cleanup() {
  rm -f "${OVERRIDE_COMPOSE}"
}
trap cleanup EXIT

poll_ms="$(loop_interval_ms "${F11_LOAD_SNAPSHOTS_PER_SEC_PER_VENUE}")"
if [[ "${poll_ms}" -gt 50 ]]; then
  poll_ms=50
fi

log "Starting stand for F11-TEST-3: Connector+Normalizer+CurveBuilder+Kafka+MDS+Matching+Risk"
log "Parallelism: BUILD_JOBS=${BUILD_JOBS}, TEST_JOBS=${TEST_JOBS}, COMPOSE_PARALLEL_LIMIT=${COMPOSE_PARALLEL_LIMIT}"
log "Load config: venues=${F11_LOAD_VENUES}, snapshots/sec/venue=${F11_LOAD_SNAPSHOTS_PER_SEC_PER_VENUE}, poll_interval_ms=${poll_ms}"
log "Diagnostics: F11_LOAD_TRACE_MARKET_DATA=${F11_LOAD_TRACE_MARKET_DATA}"
docker compose -f "${BASE_COMPOSE}" down -v --remove-orphans >/dev/null 2>&1 || true
write_override "L2" "${poll_ms}"
retry_command 3 5 compose up -d --build clickhouse redpanda topics-init market_data matching risk $(venue_services)
if ! wait_broker_ready; then
  mark_fail "Broker redpanda did not become ready"
  exit 1
fi

run_level_load_test "L1" "${F11_LOAD_P95_L1_L2_LIMIT_MS}" "${poll_ms}"
run_level_load_test "L2" "${F11_LOAD_P95_L1_L2_LIMIT_MS}" "${poll_ms}"
run_level_load_test "L3" "${F11_LOAD_P95_L3_LIMIT_MS}" "${poll_ms}"

if [[ "${FAILS}" -ne 0 ]]; then
  log "FAILED with ${FAILS} assertion(s). Artifacts: ${ARTIFACTS_DIR}"
  exit 1
fi

log "PASSED. Artifacts: ${ARTIFACTS_DIR}"
