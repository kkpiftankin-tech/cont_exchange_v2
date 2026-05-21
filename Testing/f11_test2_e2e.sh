#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMPOSE="${ROOT_DIR}/infra/docker-compose.dev.yml"
OVERRIDE_COMPOSE="${ROOT_DIR}/Testing/.f11_test2.override.yml"
ARTIFACTS_DIR="${ROOT_DIR}/artifacts/f11_test2"

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
# Multiplier for waits/timeouts in slower CI environments.
F11_TIMEOUT_SCALE="${F11_TIMEOUT_SCALE:-1}"
# Optional frontend-api coverage in F11-TEST-2.
# 1 = build/start frontend-api and run its assertions, 0 = skip this contour.
F11_FRONTEND_API_CHECK="${F11_FRONTEND_API_CHECK:-0}"
# Optional gate for flaky DEX MarketData ingestion assertion in CI.
# 1 = enforce, 0 = skip enforcement (keep informational logging).
F11_DEX_MDS_INGESTION_CHECK="${F11_DEX_MDS_INGESTION_CHECK:-1}"
export BUILD_JOBS
export TEST_JOBS
export COMPOSE_PARALLEL_LIMIT
export VERBOSE
export F11_TIMEOUT_SCALE
export F11_FRONTEND_API_CHECK
export F11_DEX_MDS_INGESTION_CHECK

mkdir -p "${ARTIFACTS_DIR}"
rm -rf "${ARTIFACTS_DIR:?}"/*

FAILS=0

compose() {
  if [[ "${VERBOSE}" == "1" ]]; then
    printf '[F11-TEST-2] RUN: docker compose -f %q -f %q' "${BASE_COMPOSE}" "${OVERRIDE_COMPOSE}" >&2
    for arg in "$@"; do
      printf ' %q' "${arg}" >&2
    done
    printf '\n' >&2
  fi
  docker compose -f "${BASE_COMPOSE}" -f "${OVERRIDE_COMPOSE}" "$@"
}

log() {
  printf '[F11-TEST-2] %s\n' "$*"
}

mark_fail() {
  printf '[F11-TEST-2][FAIL] %s\n' "$*" >&2
  FAILS=$((FAILS + 1))
}

frontend_api_enabled() {
  [[ "${F11_FRONTEND_API_CHECK}" == "1" ]]
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

assert_log_contains() {
  local file="$1"
  local pattern="$2"
  local description="$3"
  if [[ -f "${file}" ]] && grep -q "${pattern}" "${file}"; then
    log "PASS: ${description}"
  else
    mark_fail "${description} (pattern='${pattern}', file='${file}')"
  fi
}

assert_log_contains_any() {
  local file="$1"
  local description="$2"
  shift 2
  if [[ ! -f "${file}" ]]; then
    mark_fail "${description} (missing file='${file}')"
    return
  fi
  local pattern
  for pattern in "$@"; do
    if grep -q "${pattern}" "${file}"; then
      log "PASS: ${description} (pattern='${pattern}')"
      return
    fi
  done
  mark_fail "${description} (no patterns matched in file='${file}')"
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

assert_equals() {
  local expected="$1"
  local actual="$2"
  local description="$3"
  if [[ "${actual}" == "${expected}" ]]; then
    log "PASS: ${description} (value=${actual})"
  else
    mark_fail "${description} (expected='${expected}', actual='${actual}')"
  fi
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

write_override() {
  local mock_mode="$1"
  shift

  cat >"${OVERRIDE_COMPOSE}" <<EOF
services:
  venues:
    environment:
EOF

  for kv in "$@"; do
    printf '      - "%s"\n' "${kv}" >>"${OVERRIDE_COMPOSE}"
  done

  cat >>"${OVERRIDE_COMPOSE}" <<EOF
  matching:
    environment:
      - "MATCHING_DISABLE_EXTERNAL_VENUES=0"
  dex-mock:
    image: python:3.11-alpine
    command: ["python","-u","/app/f11_dex_mock.py"]
    environment:
      - "DEX_MOCK_MODE=${mock_mode}"
      - "DEX_MOCK_PORT=18081"
    volumes:
      - "${ROOT_DIR}/Testing/f11_dex_mock.py:/app/f11_dex_mock.py:ro"
EOF

  if frontend_api_enabled; then
    cat >>"${OVERRIDE_COMPOSE}" <<EOF
  frontend-api:
    build:
      context: "${ROOT_DIR}/frontend/api"
      dockerfile: Dockerfile
    environment:
      - "PORT=8090"
      - "LEDGER_GRPC_ADDR=ledger:50053"
      - "VENUES_HTTP_ADDR=http://venues:8087"
    ports:
      - "18090:8090"
EOF
  fi
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

collect_logs() {
  local since="$1"
  local scenario_dir="$2"
  local services=(venues venue_health market_data matching risk dex-mock)
  if frontend_api_enabled; then
    services+=(frontend-api)
  fi
  for svc in "${services[@]}"; do
    compose logs --since "${since}" "${svc}" >"${scenario_dir}/${svc}.log" 2>&1 || true
  done
}

wait_frontend_api_ready() {
  local tries
  tries="$(scaled_seconds 60)"
  for _ in $(seq 1 "${tries}"); do
    if curl -fsS "http://localhost:18090/healthz" >/dev/null 2>&1; then
      return 0
    fi
    sleep 2
  done
  return 1
}

wait_frontend_venues_ready() {
  local tries
  tries="$(scaled_seconds 60)"
  for _ in $(seq 1 "${tries}"); do
    local payload
    payload="$(curl -fsS "http://localhost:18090/api/venues" 2>/dev/null || true)"
    local compact
    compact="$(tr -d '[:space:]' <<<"${payload}")"
    if [[ -n "${compact}" ]] && grep -q '"source":"venues-api"' <<<"${compact}"; then
      return 0
    fi
    sleep 2
  done
  return 1
}

wait_gateway_ready() {
  local tries
  tries="$(scaled_seconds 60)"
  for _ in $(seq 1 "${tries}"); do
    if curl -fsS "http://localhost:8088/healthz" >/dev/null 2>&1; then
      return 0
    fi
    sleep 2
  done
  return 1
}

assert_frontend_venues_from_backend() {
  local payload
  payload="$(curl -fsS "http://localhost:18090/api/venues" 2>/dev/null || true)"
  local compact
  compact="$(tr -d '[:space:]' <<<"${payload}")"
  if [[ -n "${compact}" ]] && grep -q '"source":"venues-api"' <<<"${compact}"; then
    log "PASS: Frontend API /api/venues uses backend venues-api source"
  else
    mark_fail "Frontend API /api/venues did not confirm backend source=venues-api"
  fi
}

assert_frontend_path_from_backend() {
  local path="$1"
  local description="$2"
  local payload
  payload="$(curl -fsS "http://localhost:18090${path}" 2>/dev/null || true)"
  local compact
  compact="$(tr -d '[:space:]' <<<"${payload}")"
  if [[ -n "${compact}" ]] && grep -q '"source":"venues-api"' <<<"${compact}"; then
    log "PASS: ${description}"
  else
    mark_fail "${description} (path='${path}')"
  fi
}

frontend_path_items_count() {
  local path="$1"
  curl -fsS "http://localhost:18090${path}" 2>/dev/null \
    | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{try{const p=JSON.parse(s);const n=Array.isArray(p.items)?p.items.length:0;process.stdout.write(String(n));}catch{process.stdout.write("0");}})' \
    || echo "0"
}

assert_frontend_path_items_gt_zero() {
  local path="$1"
  local description="$2"
  local count
  count="$(frontend_path_items_count "${path}")"
  count="$(tr -cd '0-9' <<<"${count}")"
  if [[ -z "${count}" ]]; then count=0; fi
  if [[ "${count}" -gt 0 ]]; then
    log "PASS: ${description} (items=${count})"
  else
    mark_fail "${description} (path='${path}', items=${count})"
  fi
}

ledger_venue_total() {
  local venue="$1"
  local currency="$2"
  compose exec -T -e VENUE="${venue}" -e CURRENCY="${currency}" frontend-api node - <<'NODE' 2>/dev/null || echo "0"
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const path = require('path');

const protoRoot = '/app/proto';
const defs = protoLoader.loadSync(
  [path.join(protoRoot, 'fob/ledger/v1/ledger.proto'),
   path.join(protoRoot, 'fob/common/v1/common.proto')],
  {
    keepCase: true,
    longs: String,
    enums: String,
    defaults: true,
    oneofs: true,
    includeDirs: [protoRoot]
  }
);
const proto = grpc.loadPackageDefinition(defs).fob.ledger.v1;
const client = new proto.LedgerService('ledger:50053', grpc.credentials.createInsecure());
const venue = process.env.VENUE || '';
const currency = process.env.CURRENCY || '';
const request = {
  meta: {
    event_id: 'f11-e2e-ledger-balances',
    ts_event: { seconds: Math.floor(Date.now() / 1000), nanos: 0 },
    source: 'f11-e2e',
    correlation_id: 'f11-e2e',
    partition_key: ''
  },
  venue,
  currency
};
client.GetVenueBalances(request, (err, response) => {
  if (err) {
    console.log('0');
    process.exit(0);
    return;
  }
  const balances = Array.isArray(response?.balances) ? response.balances : [];
  const row = balances.find((b) => b?.venue === venue && b?.currency === currency) || balances[0];
  const units = Number(row?.total?.units || 0);
  const scale = Number(row?.total?.scale || 0);
  const value = units / Math.pow(10, scale);
  console.log(Number.isFinite(value) ? String(value) : '0');
  process.exit(0);
});
NODE
}

frontend_venue_field() {
  local venue="$1"
  local field="$2"
  compose exec -T -e VENUE="${venue}" -e FIELD="${field}" frontend-api node - <<'NODE' 2>/dev/null || true
const http = require('http');
const venue = process.env.VENUE || '';
const field = process.env.FIELD || '';

http.get('http://localhost:8090/api/venues', (res) => {
  let data = '';
  res.on('data', (chunk) => { data += chunk; });
  res.on('end', () => {
    try {
      const payload = JSON.parse(data);
      const item = (payload.items || []).find((x) => x.venueId === venue) || {};
      const value = field.split('.').filter(Boolean).reduce((acc, key) => (
        acc && Object.prototype.hasOwnProperty.call(acc, key) ? acc[key] : undefined
      ), item);
      process.stdout.write(value === undefined || value === null ? '' : String(value));
    } catch {
      process.stdout.write('');
    }
    process.exit(0);
  });
}).on('error', () => {
  process.stdout.write('');
  process.exit(0);
});
NODE
}

assert_float_changed() {
  local before="$1"
  local after="$2"
  local description="$3"
  local changed
  changed="$(awk -v a="${before}" -v b="${after}" 'BEGIN {d=b-a; if (d<0) d=-d; if (d>1e-12) print 1; else print 0;}')"
  if [[ "${changed}" == "1" ]]; then
    log "PASS: ${description} (before=${before}, after=${after})"
  else
    mark_fail "${description} (expected change, before=${before}, after=${after})"
  fi
}

clickhouse_count() {
  local query="$1"
  compose exec -T clickhouse clickhouse-client --query "${query}" 2>/dev/null | tr -d '\r' || echo 0
}

clickhouse_scalar() {
  local query="$1"
  compose exec -T clickhouse clickhouse-client --query "${query}" 2>/dev/null | head -n1 | tr -d '\r' || echo 0
}

liquidity_curves_count() {
  clickhouse_count "SELECT count() FROM default.liquidity_curves"
}

liquidity_curves_by_venue_count() {
  local venue="$1"
  clickhouse_count "SELECT count() FROM default.liquidity_curves WHERE venue_id='${venue}'"
}

wait_for_venue_curve_growth() {
  local venue="$1"
  local before="$2"
  local timeout_sec="$3"
  local after_num
  local before_num
  before_num="$(tr -cd '0-9' <<<"${before}")"
  if [[ -z "${before_num}" ]]; then before_num=0; fi
  after_num="${before_num}"
  for _ in $(seq 1 "${timeout_sec}"); do
    local after_raw
    after_raw="$(liquidity_curves_by_venue_count "${venue}")"
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

wait_for_liquidity_curves_growth() {
  local before="$1"
  local timeout_sec="$2"
  local after_num
  local before_num
  before_num="$(tr -cd '0-9' <<<"${before}")"
  if [[ -z "${before_num}" ]]; then before_num=0; fi
  after_num="${before_num}"
  for _ in $(seq 1 "${timeout_sec}"); do
    local after_raw
    after_raw="$(liquidity_curves_count)"
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

run_scenario() {
  local scenario="$1"
  local mock_mode="$2"
  local wait_sec="$3"
  shift 3

  local scenario_dir="${ARTIFACTS_DIR}/${scenario}"
  mkdir -p "${scenario_dir}"
  local since
  since="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  write_override "${mock_mode}" "$@"
  compose up -d --force-recreate --no-deps venues dex-mock

  sleep "$(scaled_seconds "${wait_sec}")"
  collect_logs "${since}" "${scenario_dir}"
}

cleanup() {
  rm -f "${OVERRIDE_COMPOSE}"
}
trap cleanup EXIT

if frontend_api_enabled; then
  log "Starting core stand: Kafka + ClickHouse + Venues + VenueHealth + MDS + Matching + Risk + Ledger + OrderFlow + Gateway + Frontend API"
else
  log "Starting core stand: Kafka + ClickHouse + Venues + VenueHealth + MDS + Matching + Risk + Ledger + OrderFlow + Gateway"
  log "Frontend API checks are disabled by F11_FRONTEND_API_CHECK=0"
fi
log "Parallelism: BUILD_JOBS=${BUILD_JOBS}, TEST_JOBS=${TEST_JOBS}, COMPOSE_PARALLEL_LIMIT=${COMPOSE_PARALLEL_LIMIT}"
log "Timeout scale: F11_TIMEOUT_SCALE=${F11_TIMEOUT_SCALE}"
docker compose -f "${BASE_COMPOSE}" down -v --remove-orphans || true
write_override "healthy" "VENUES_ADAPTER_MODE=simulated"
core_services=(clickhouse redpanda topics-init venues venue_health market_data matching risk ledger order_flow gateway)
if frontend_api_enabled; then
  core_services+=(frontend-api)
fi
retry_command 3 5 compose up -d --build "${core_services[@]}"
if ! wait_broker_ready; then
  mark_fail "Broker redpanda did not become ready"
  exit 1
fi
if ! wait_gateway_ready; then
  mark_fail "gateway did not become ready"
  exit 1
fi
if frontend_api_enabled; then
  if ! wait_frontend_api_ready; then
    mark_fail "frontend-api did not become ready"
    exit 1
  fi
  if ! wait_frontend_venues_ready; then
    mark_fail "frontend-api did not become ready to serve venues-api data"
    exit 1
  fi
fi

log "Scenario CEX"
cex_before_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
cex_before_topic_health="$(topic_hwm_sum "venue.health")"
cex_before_mds_curve_rows="$(liquidity_curves_by_venue_count "binance")"
cex_before_mds_snapshots="$(group_offset_sum "marketdata" "venue.snapshots")"
cex_before_venue_health_consume="$(group_offset_sum "venue_health" "venue.health")"
cex_before_matching_curve="$(group_offset_sum "matching" "venue.liquidity.fob")"
cex_before_matching_snapshots="$(group_offset_sum "matching" "venue.snapshots")"
cex_before_risk_curve="$(group_offset_sum "risk" "venue.liquidity.fob")"
run_scenario "cex" "healthy" 20 \
  "VENUES_ADAPTER_MODE=simulated" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=false"
cex_after_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
cex_after_topic_health="$(topic_hwm_sum "venue.health")"
cex_after_mds_curve_rows="$(wait_for_venue_curve_growth "binance" "${cex_before_mds_curve_rows}" "$(scaled_seconds 35)" || true)"
cex_after_mds_snapshots="$(wait_for_group_offset_growth "marketdata" "venue.snapshots" "${cex_before_mds_snapshots}" "$(scaled_seconds 35)" || true)"
cex_after_venue_health_consume="$(wait_for_group_offset_growth "venue_health" "venue.health" "${cex_before_venue_health_consume}" "$(scaled_seconds 35)" || true)"
cex_after_matching_curve="$(wait_for_group_offset_growth "matching" "venue.liquidity.fob" "${cex_before_matching_curve}" "$(scaled_seconds 35)" || true)"
cex_after_matching_snapshots="$(group_offset_sum "matching" "venue.snapshots")"
cex_after_risk_curve="$(wait_for_group_offset_growth "risk" "venue.liquidity.fob" "${cex_before_risk_curve}" "$(scaled_seconds 35)" || true)"
assert_delta_gt_zero "${cex_before_topic_curve}" "${cex_after_topic_curve}" "CEX: venue.liquidity.fob topic advanced"
assert_delta_gt_zero "${cex_before_topic_health}" "${cex_after_topic_health}" "CEX: venue.health topic advanced"
assert_delta_gt_zero "${cex_before_venue_health_consume}" "${cex_after_venue_health_consume}" "CEX: VenueHealth consumed venue.health"
assert_log_contains "${ARTIFACTS_DIR}/cex/venue_health.log" "Aggregating venue health event" "CEX: VenueHealth processed RAW health events"
assert_log_contains "${ARTIFACTS_DIR}/cex/venue_health.log" "Skipping already aggregated venue health event" "CEX: VenueHealth published and re-read AGGREGATED health events"
if frontend_api_enabled; then
  assert_frontend_venues_from_backend
  assert_frontend_path_from_backend "/api/venues/binance/snapshots" "CEX: Frontend API snapshots endpoint proxied from venues-api"
  assert_frontend_path_from_backend "/api/venues/binance/synthetics" "CEX: Frontend API synthetics endpoint proxied from venues-api"
fi
assert_delta_gt_zero "${cex_before_mds_snapshots}" "${cex_after_mds_snapshots}" "CEX: MarketData consumed venue.snapshots"
assert_delta_gt_zero "${cex_before_mds_curve_rows}" "${cex_after_mds_curve_rows}" "CEX: MDS stored binance liquidity curves in ClickHouse"
assert_delta_gt_zero "${cex_before_matching_curve}" "${cex_after_matching_curve}" "CEX: Matching consumed venue.liquidity.fob"
assert_delta_eq_zero "${cex_before_matching_snapshots}" "${cex_after_matching_snapshots}" "CEX: Matching does not consume venue.snapshots directly"
assert_delta_gt_zero "${cex_before_risk_curve}" "${cex_after_risk_curve}" "CEX: Risk consumed venue.liquidity.fob"
SNAP_COUNT="$(clickhouse_count "SELECT count() FROM backtest.venue_snapshots")"
if [[ "${SNAP_COUNT:-0}" -gt 0 ]]; then
  log "PASS: CEX snapshot persisted in ClickHouse (count=${SNAP_COUNT})"
else
  mark_fail "CEX: no snapshots in ClickHouse backtest.venue_snapshots"
fi
CURVE_META_COUNT="$(clickhouse_scalar "SELECT count() FROM default.liquidity_curves WHERE producer_version != '' AND model_config_version != '' AND epsilon1 >= 0 AND epsilon2 >= 0 AND epsilon3 >= 0")"
if [[ "${CURVE_META_COUNT:-0}" -gt 0 ]]; then
  log "PASS: Curve model versions and quality metrics persisted in ClickHouse (count=${CURVE_META_COUNT})"
else
  mark_fail "F11-20: liquidity_curves missing model versions and/or quality metrics"
fi

log "Scenario multi-venue (2 CEX + 1 DEX)"
multi_before_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
multi_before_binance_rows="$(liquidity_curves_by_venue_count "binance")"
multi_before_coinbase_rows="$(liquidity_curves_by_venue_count "coinbase")"
multi_before_uniswap_rows="$(liquidity_curves_by_venue_count "uniswap_v3")"
run_scenario "multi_venue" "healthy" 20 \
  "VENUES_ADAPTER_MODE=simulated_multi" \
  "SIM_MULTI_CEX_PRIMARY_VENUE_ID=binance" \
  "SIM_MULTI_CEX_SECONDARY_VENUE_ID=coinbase" \
  "SIM_MULTI_DEX_VENUE_ID=uniswap_v3" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=false"
multi_after_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
multi_after_binance_rows="$(wait_for_venue_curve_growth "binance" "${multi_before_binance_rows}" "$(scaled_seconds 35)" || true)"
multi_after_coinbase_rows="$(wait_for_venue_curve_growth "coinbase" "${multi_before_coinbase_rows}" "$(scaled_seconds 35)" || true)"
multi_after_uniswap_rows="$(wait_for_venue_curve_growth "uniswap_v3" "${multi_before_uniswap_rows}" "$(scaled_seconds 35)" || true)"
assert_delta_gt_zero "${multi_before_topic_curve}" "${multi_after_topic_curve}" "Multi-venue: venue.liquidity.fob topic advanced"
assert_delta_gt_zero "${multi_before_binance_rows}" "${multi_after_binance_rows}" "Multi-venue: Binance liquidity curve persisted"
assert_delta_gt_zero "${multi_before_coinbase_rows}" "${multi_after_coinbase_rows}" "Multi-venue: Coinbase liquidity curve persisted"
assert_delta_gt_zero "${multi_before_uniswap_rows}" "${multi_after_uniswap_rows}" "Multi-venue: Uniswap liquidity curve persisted"
if frontend_api_enabled; then
  assert_equals "binance" "$(frontend_venue_field "binance" "venueId")" "Multi-venue: Frontend exposes binance"
  assert_equals "coinbase" "$(frontend_venue_field "coinbase" "venueId")" "Multi-venue: Frontend exposes coinbase"
  assert_equals "uniswap_v3" "$(frontend_venue_field "uniswap_v3" "venueId")" "Multi-venue: Frontend exposes uniswap_v3"
  assert_frontend_path_items_gt_zero "/api/venues/binance/snapshots" "Multi-venue: binance snapshots visible in frontend API"
  assert_frontend_path_items_gt_zero "/api/venues/coinbase/snapshots" "Multi-venue: coinbase snapshots visible in frontend API"
  assert_frontend_path_items_gt_zero "/api/venues/uniswap_v3/snapshots" "Multi-venue: uniswap snapshots visible in frontend API"
fi

log "Scenario execution->ledger"
if frontend_api_enabled; then
  exec_before_topic_intents="$(topic_hwm_sum "execution.intents")"
  exec_before_topic_reports="$(topic_hwm_sum "execution.venue")"
  exec_before_venues_intents="$(group_offset_sum "venues" "execution.intents")"
  exec_before_ledger_intents="$(group_offset_sum "ledger-intents" "execution.intents")"
  exec_before_ledger_reports="$(group_offset_sum "ledger-exec" "execution.venue")"
  exec_before_risk_reports="$(group_offset_sum "risk" "execution.venue")"
  exec_before_ch_reports="$(clickhouse_count "SELECT count() FROM default.execution_venue")"
  exec_before_btc_total="$(ledger_venue_total "binance" "BTC")"
  exec_before_usdt_total="$(ledger_venue_total "binance" "USDT")"

  exec_order_resp="$(
    curl -fsS -X POST "http://localhost:18090/api/bid" \
      -H "Content-Type: application/json" \
      -d '{"from_currency":"USDT","to_currency":"BTC","amount_to_buy":0.02,"min_price":20000,"max_price":90000,"buy_speed":0.02}' \
      || true
  )"
  if grep -q '"id"' <<<"${exec_order_resp}"; then
    log "PASS: execution scenario order accepted by frontend-api/gateway"
  else
    mark_fail "execution scenario order was not accepted (response=${exec_order_resp})"
  fi

  exec_after_topic_intents="$(wait_for_group_offset_growth "venues" "execution.intents" "${exec_before_venues_intents}" "$(scaled_seconds 40)" || true)"
  exec_after_topic_reports="$(wait_for_group_offset_growth "ledger-exec" "execution.venue" "${exec_before_ledger_reports}" "$(scaled_seconds 40)" || true)"
  exec_after_hwm_intents="$(topic_hwm_sum "execution.intents")"
  exec_after_hwm_reports="$(topic_hwm_sum "execution.venue")"
  exec_after_ledger_intents="$(group_offset_sum "ledger-intents" "execution.intents")"
  exec_after_ledger_reports="$(group_offset_sum "ledger-exec" "execution.venue")"
  exec_after_risk_reports="$(wait_for_group_offset_growth "risk" "execution.venue" "${exec_before_risk_reports}" "$(scaled_seconds 40)" || true)"
  exec_after_ch_reports="$(clickhouse_count "SELECT count() FROM default.execution_venue")"
  exec_after_btc_total="$(ledger_venue_total "binance" "BTC")"
  exec_after_usdt_total="$(ledger_venue_total "binance" "USDT")"

  assert_delta_gt_zero "${exec_before_topic_intents}" "${exec_after_hwm_intents}" "Execution: execution.intents topic advanced"
  assert_delta_gt_zero "${exec_before_topic_reports}" "${exec_after_hwm_reports}" "Execution: execution.venue topic advanced"
  assert_delta_gt_zero "${exec_before_venues_intents}" "${exec_after_topic_intents}" "Execution: Venues consumed execution.intents"
  assert_delta_gt_zero "${exec_before_ledger_intents}" "${exec_after_ledger_intents}" "Execution: Ledger consumed execution.intents"
  assert_delta_gt_zero "${exec_before_ledger_reports}" "${exec_after_ledger_reports}" "Execution: Ledger consumed execution.venue"
  assert_delta_gt_zero "${exec_before_risk_reports}" "${exec_after_risk_reports}" "Execution: Risk consumed execution.venue"
  assert_delta_gt_zero "${exec_before_ch_reports}" "${exec_after_ch_reports}" "Execution: execution.venue persisted in ClickHouse"
  assert_float_changed "${exec_before_btc_total}" "${exec_after_btc_total}" "Execution: Ledger venue BTC balance changed"
  assert_float_changed "${exec_before_usdt_total}" "${exec_after_usdt_total}" "Execution: Ledger venue USDT balance changed"
  assert_log_contains "${ARTIFACTS_DIR}/cex/venues.log" "Produced execution report" "Execution: Venue adapter produced execution report"
  assert_log_contains "${ARTIFACTS_DIR}/cex/matching.log" "Selected venue and built execution intent for order leg" "Execution: Matching planner built execution intent"
  assert_log_contains "${ARTIFACTS_DIR}/cex/venues.log" "Recorded L3 impact calibration sample" "Execution: L3 calibration ingests execution reports"
else
  log "INFO: Skipping execution scenario that depends on frontend-api"
fi

log "Scenario synthetic_compat"
synthetic_before_topic="$(topic_hwm_sum "venue.synthetic")"
synthetic_before_risk_group="$(group_offset_sum "risk" "venue.synthetic")"
run_scenario "synthetic_compat" "healthy" 12 \
  "VENUES_ADAPTER_MODE=simulated" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=false" \
  "CURVE_SYNTHETIC_ENABLED=true" \
  "VENUE_SYNTHETIC_TOPIC=venue.synthetic"
synthetic_after_topic="$(topic_hwm_sum "venue.synthetic")"
synthetic_after_risk_group="$(wait_for_group_offset_growth "risk" "venue.synthetic" "${synthetic_before_risk_group}" "$(scaled_seconds 25)" || true)"
assert_delta_gt_zero "${synthetic_before_topic}" "${synthetic_after_topic}" "Synthetic: venue.synthetic topic advanced"
assert_delta_gt_zero "${synthetic_before_risk_group}" "${synthetic_after_risk_group}" "Synthetic: Risk consumed venue.synthetic"
if frontend_api_enabled; then
  assert_frontend_path_from_backend "/api/venues/binance/synthetics" "Synthetic: Frontend API synthetics endpoint proxied from venues-api"
  assert_frontend_path_items_gt_zero "/api/venues/binance/synthetics" "Synthetic: Frontend API exposes non-empty synthetics list"
fi

log "Scenario admin CRUD + hot reload"
if frontend_api_enabled; then
  admin_initial_active="$(frontend_venue_field "binance" "config.isActive")"
  assert_equals "true" "${admin_initial_active}" "Admin: binance starts active"

  if curl -fsS -X PUT "http://localhost:18090/api/venues/binance/config" \
    -H "Content-Type: application/json" \
    -d '{"stale_threshold_ms":7777,"circuit_breaker_errors":4}' >/dev/null 2>&1; then
    log "PASS: Admin: PUT /api/venues/binance/config accepted"
  else
    mark_fail "Admin: failed to update binance config via frontend API"
  fi
  sleep 2
  admin_after_put_stale="$(frontend_venue_field "binance" "config.staleThresholdMs")"
  admin_after_put_cb_errors="$(frontend_venue_field "binance" "config.circuitBreakerErrors")"
  assert_equals "7777" "${admin_after_put_stale}" "Admin: stale threshold hot reloaded"
  assert_equals "4" "${admin_after_put_cb_errors}" "Admin: circuit breaker errors hot reloaded"

  if curl -fsS -X POST "http://localhost:18090/api/venues/binance/disable" >/dev/null 2>&1; then
    log "PASS: Admin: disable command accepted"
  else
    mark_fail "Admin: failed to disable binance via frontend API"
  fi
  sleep 2
  admin_after_disable_active="$(frontend_venue_field "binance" "config.isActive")"
  assert_equals "false" "${admin_after_disable_active}" "Admin: disable deactivates venue"

  if curl -fsS -X POST "http://localhost:18090/api/venues/binance/enable" >/dev/null 2>&1; then
    log "PASS: Admin: enable command accepted"
  else
    mark_fail "Admin: failed to enable binance via frontend API"
  fi
  sleep 2
  admin_after_enable_active="$(frontend_venue_field "binance" "config.isActive")"
  assert_equals "true" "${admin_after_enable_active}" "Admin: enable activates venue"

  if curl -fsS -X DELETE "http://localhost:18090/api/venues/binance/config" >/dev/null 2>&1; then
    log "PASS: Admin: DELETE /api/venues/binance/config accepted"
  else
    mark_fail "Admin: failed to delete(deactivate) binance config via frontend API"
  fi
  sleep 2
  admin_after_delete_active="$(frontend_venue_field "binance" "config.isActive")"
  assert_equals "false" "${admin_after_delete_active}" "Admin: DELETE deactivates venue (soft delete)"

  if curl -fsS -X POST "http://localhost:18090/api/venues/binance/enable" >/dev/null 2>&1; then
    log "PASS: Admin: final enable restore accepted"
  else
    mark_fail "Admin: failed to restore binance active after DELETE scenario"
  fi
  sleep 2
  admin_after_restore_active="$(frontend_venue_field "binance" "config.isActive")"
  assert_equals "true" "${admin_after_restore_active}" "Admin: binance restored active for next scenarios"
else
  log "INFO: Skipping admin CRUD scenario that depends on frontend-api"
fi

log "Scenario DEX"
dex_before_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
dex_before_mds_fob="$(group_offset_sum "marketdata" "venue.liquidity.fob")"
dex_before_matching_curve="$(group_offset_sum "matching" "venue.liquidity.fob")"
dex_before_risk_curve="$(group_offset_sum "risk" "venue.liquidity.fob")"
dex_before_mds_snapshots="$(group_offset_sum "marketdata" "venue.snapshots")"
dex_before_curve_rows_total="$(liquidity_curves_count)"
run_scenario "dex" "healthy" 10 \
  "VENUES_ADAPTER_MODE=dex_amm_rpc" \
  "DEX_SYNC_MODE=polling" \
  "DEX_RPC_URL=http://dex-mock:18081" \
  "DEX_POOL_ADDRESS=0xpool" \
  "DEX_CHAIN_ID=eth-mainnet" \
  "DEX_POLLING_INTERVAL_FAST_MS=500" \
  "DEX_POLLING_INTERVAL_SLOW_MS=500" \
  "CURVE_LEVEL=L2" \
  "CURVE_QUALITY_GATING_ENABLED=false" \
  "CURVE_MIN_L3_CONFIDENCE=0.0" \
  "CURVE_MIN_L2_CONFIDENCE=0.0" \
  "CURVE_MIN_L1_CONFIDENCE=0.0" \
  "CURVE_AMM_DIRECT_ENABLED=true" \
  "CURVE_AMM_DIRECT_COMPARE_WITH_VLOB=true"
dex_after_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
dex_after_mds_fob="$(wait_for_group_offset_growth "marketdata" "venue.liquidity.fob" "${dex_before_mds_fob}" "$(scaled_seconds 30)" || true)"
dex_after_matching_curve="$(wait_for_group_offset_growth "matching" "venue.liquidity.fob" "${dex_before_matching_curve}" "$(scaled_seconds 20)" || true)"
dex_after_risk_curve="$(wait_for_group_offset_growth "risk" "venue.liquidity.fob" "${dex_before_risk_curve}" "$(scaled_seconds 20)" || true)"
dex_after_mds_snapshots="$(wait_for_group_offset_growth "marketdata" "venue.snapshots" "${dex_before_mds_snapshots}" "$(scaled_seconds 12)" || true)"
dex_after_curve_rows_total="$(wait_for_liquidity_curves_growth "${dex_before_curve_rows_total}" "$(scaled_seconds 30)" || true)"
assert_delta_gt_zero "${dex_before_topic_curve}" "${dex_after_topic_curve}" "DEX: venue.liquidity.fob topic advanced"
if [[ "${dex_after_mds_fob}" -gt "${dex_before_mds_fob}" ]]; then
  log "PASS: DEX: MarketData consumed venue.liquidity.fob (delta=$((dex_after_mds_fob - dex_before_mds_fob)))"
elif [[ "${dex_after_curve_rows_total}" -gt "${dex_before_curve_rows_total}" ]]; then
  log "PASS: DEX: MarketData ingestion confirmed via ClickHouse liquidity_curves growth (delta=$((dex_after_curve_rows_total - dex_before_curve_rows_total)))"
else
  if [[ "${F11_DEX_MDS_INGESTION_CHECK}" == "0" ]]; then
    log "INFO: DEX: skipped strict MarketData ingestion assertion by F11_DEX_MDS_INGESTION_CHECK=0 (offset before=${dex_before_mds_fob}, after=${dex_after_mds_fob}; curves before=${dex_before_curve_rows_total}, after=${dex_after_curve_rows_total})"
  else
    mark_fail "DEX: MarketData ingestion missing for venue.liquidity.fob (offset before=${dex_before_mds_fob}, after=${dex_after_mds_fob}; curves before=${dex_before_curve_rows_total}, after=${dex_after_curve_rows_total})"
  fi
fi
assert_delta_gt_zero "${dex_before_matching_curve}" "${dex_after_matching_curve}" "DEX: Matching consumed venue.liquidity.fob"
assert_delta_gt_zero "${dex_before_risk_curve}" "${dex_after_risk_curve}" "DEX: Risk consumed venue.liquidity.fob"
if [[ "${dex_after_mds_snapshots}" -gt "${dex_before_mds_snapshots}" ]]; then
  log "PASS: DEX optional path: MarketData consumed venue.snapshots (delta=$((dex_after_mds_snapshots - dex_before_mds_snapshots)))"
else
  log "INFO: DEX path without venue.snapshots growth is acceptable when AMM direct FOB path is used"
fi

log "Scenario disconnect"
disconnect_before_topic_health="$(topic_hwm_sum "venue.health")"
disconnect_before_matching_health="$(group_offset_sum "matching" "venue.health")"
disconnect_before_risk_health="$(group_offset_sum "risk" "venue.health")"
run_scenario "disconnect" "healthy" 12 \
  "VENUES_ADAPTER_MODE=dex_amm_rpc" \
  "DEX_SYNC_MODE=polling" \
  "DEX_RPC_URL=" \
  "DEX_POOL_ADDRESS=0xpool" \
  "DEX_CHAIN_ID=eth-mainnet"
disconnect_after_topic_health="$(topic_hwm_sum "venue.health")"
disconnect_after_matching_health="$(wait_for_group_offset_growth "matching" "venue.health" "${disconnect_before_matching_health}" "$(scaled_seconds 20)" || true)"
disconnect_after_risk_health="$(wait_for_group_offset_growth "risk" "venue.health" "${disconnect_before_risk_health}" "$(scaled_seconds 20)" || true)"
assert_delta_gt_zero "${disconnect_before_topic_health}" "${disconnect_after_topic_health}" "Disconnect: venue.health topic advanced"
assert_delta_gt_zero "${disconnect_before_matching_health}" "${disconnect_after_matching_health}" "Disconnect: Matching consumed venue.health"
assert_delta_gt_zero "${disconnect_before_risk_health}" "${disconnect_after_risk_health}" "Disconnect: Risk consumed venue.health"

log "Scenario stale"
stale_before_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
stale_before_topic_health="$(topic_hwm_sum "venue.health")"
stale_before_matching_health="$(group_offset_sum "matching" "venue.health")"
stale_before_risk_health="$(group_offset_sum "risk" "venue.health")"
run_scenario "stale" "healthy" 16 \
  "VENUES_ADAPTER_MODE=dex_amm_rpc" \
  "DEX_SYNC_MODE=polling" \
  "DEX_RPC_URL=http://dex-mock:18081" \
  "DEX_POOL_ADDRESS=0xpool" \
  "DEX_CHAIN_ID=eth-mainnet" \
  "DEX_POLLING_INTERVAL_FAST_MS=4000" \
  "DEX_POLLING_INTERVAL_SLOW_MS=4000" \
  "STALE_THRESHOLD_MS=500" \
  "CURVE_LEVEL=L3" \
  "CURVE_PUBLISH_STALE_L1_FALLBACK=false"
stale_after_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
stale_after_topic_health="$(topic_hwm_sum "venue.health")"
stale_after_matching_health="$(wait_for_group_offset_growth "matching" "venue.health" "${stale_before_matching_health}" "$(scaled_seconds 20)" || true)"
stale_after_risk_health="$(wait_for_group_offset_growth "risk" "venue.health" "${stale_before_risk_health}" "$(scaled_seconds 20)" || true)"
assert_delta_eq_zero "${stale_before_topic_curve}" "${stale_after_topic_curve}" "Stale: no new venue.liquidity.fob publication"
assert_delta_gt_zero "${stale_before_topic_health}" "${stale_after_topic_health}" "Stale: venue.health topic advanced"
assert_delta_gt_zero "${stale_before_matching_health}" "${stale_after_matching_health}" "Stale: Matching consumed venue.health"
assert_delta_gt_zero "${stale_before_risk_health}" "${stale_after_risk_health}" "Stale: Risk consumed venue.health"
assert_log_contains_any "${ARTIFACTS_DIR}/stale/matching.log" "Stale: Matching planner applied venue.health restrictions" \
  "Disabled venue for matching" \
  "Rejected planner venue input after health update"

log "Scenario CB"
cb_before_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
cb_before_topic_health="$(topic_hwm_sum "venue.health")"
cb_before_matching_health="$(group_offset_sum "matching" "venue.health")"
cb_before_risk_health="$(group_offset_sum "risk" "venue.health")"
run_scenario "cb" "fail" 12 \
  "VENUES_ADAPTER_MODE=dex_amm_rpc" \
  "DEX_SYNC_MODE=polling" \
  "DEX_RPC_URL=http://dex-mock:18081" \
  "DEX_POOL_ADDRESS=0xpool" \
  "DEX_CHAIN_ID=eth-mainnet" \
  "DEX_POLLING_INTERVAL_FAST_MS=300" \
  "DEX_POLLING_INTERVAL_SLOW_MS=300" \
  "CIRCUIT_BREAKER_ENABLED=true" \
  "CIRCUIT_BREAKER_ERRORS=2" \
  "CIRCUIT_BREAKER_WINDOW_S=10" \
  "CIRCUIT_BREAKER_COOLDOWN_S=30"
cb_after_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
cb_after_topic_health="$(topic_hwm_sum "venue.health")"
cb_after_matching_health="$(wait_for_group_offset_growth "matching" "venue.health" "${cb_before_matching_health}" "$(scaled_seconds 20)" || true)"
cb_after_risk_health="$(wait_for_group_offset_growth "risk" "venue.health" "${cb_before_risk_health}" "$(scaled_seconds 20)" || true)"
assert_delta_eq_zero "${cb_before_topic_curve}" "${cb_after_topic_curve}" "CB: no new venue.liquidity.fob publication while breaker trips"
assert_delta_gt_zero "${cb_before_topic_health}" "${cb_after_topic_health}" "CB: venue.health topic advanced"
assert_delta_gt_zero "${cb_before_matching_health}" "${cb_after_matching_health}" "CB: Matching consumed venue.health"
assert_delta_gt_zero "${cb_before_risk_health}" "${cb_after_risk_health}" "CB: Risk consumed venue.health"
assert_log_contains_any "${ARTIFACTS_DIR}/cb/matching.log" "CB: Matching planner disabled venue by health/circuit breaker" \
  "Disabled venue for matching" \
  "Rejected planner venue input after health update"

log "Scenario degraded"
degraded_before_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
degraded_before_matching_curve="$(group_offset_sum "matching" "venue.liquidity.fob")"
run_scenario "degraded" "healthy" 12 \
  "VENUES_ADAPTER_MODE=simulated" \
  "CURVE_LEVEL=L3" \
  "CURVE_MIN_L3_CONFIDENCE=0.99" \
  "CURVE_MIN_L2_CONFIDENCE=0.0" \
  "CURVE_MIN_L1_CONFIDENCE=0.0"
degraded_after_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
degraded_after_matching_curve="$(wait_for_group_offset_growth "matching" "venue.liquidity.fob" "${degraded_before_matching_curve}" "$(scaled_seconds 20)" || true)"
assert_delta_gt_zero "${degraded_before_topic_curve}" "${degraded_after_topic_curve}" "Degraded: venue.liquidity.fob topic advanced"
assert_delta_gt_zero "${degraded_before_matching_curve}" "${degraded_after_matching_curve}" "Degraded: Matching consumed degraded curve"

log "Scenario OFF"
off_before_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
off_before_matching_curve="$(group_offset_sum "matching" "venue.liquidity.fob")"
run_scenario "off" "healthy" 12 \
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
off_after_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
off_after_matching_curve="$(wait_for_group_offset_growth "matching" "venue.liquidity.fob" "${off_before_matching_curve}" "$(scaled_seconds 20)" || true)"
# Allow short transition window, then require no sustained publication in OFF mode.
sleep "$(scaled_seconds 8)"
off_late_topic_curve="$(topic_hwm_sum "venue.liquidity.fob")"
off_late_matching_curve="$(group_offset_sum "matching" "venue.liquidity.fob")"
assert_delta_eq_zero "${off_after_topic_curve}" "${off_late_topic_curve}" "OFF: no sustained venue.liquidity.fob publication in OFF mode"
assert_delta_eq_zero "${off_after_matching_curve}" "${off_late_matching_curve}" "OFF: Matching received no sustained venue.liquidity.fob in OFF mode"

if [[ "${FAILS}" -ne 0 ]]; then
  log "FAILED with ${FAILS} assertion(s). Artifacts: ${ARTIFACTS_DIR}"
  exit 1
fi

log "PASSED. Artifacts: ${ARTIFACTS_DIR}"
