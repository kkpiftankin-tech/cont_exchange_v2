#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMPOSE="${ROOT_DIR}/infra/docker-compose.dev.yml"
OVERRIDE_COMPOSE="${ROOT_DIR}/Testing/.f15_test2.override.yml"
ARTIFACTS_DIR="${ROOT_DIR}/artifacts/f15_test2"
F15_QUICK_MODE="${F15_QUICK_MODE:-1}"

mkdir -p "${ARTIFACTS_DIR}"
rm -rf "${ARTIFACTS_DIR:?}"/*

log() {
  printf '[F15-TEST-2] %s\n' "$*"
}

compose() {
  docker compose -f "${BASE_COMPOSE}" -f "${OVERRIDE_COMPOSE}" "$@"
}

write_override() {
  cat >"${OVERRIDE_COMPOSE}" <<'EOF'
services:
  postgres:
    image: postgres:16-alpine
    environment:
      POSTGRES_DB: replay
      POSTGRES_USER: replay
      POSTGRES_PASSWORD: replay
    ports:
      - "5432:5432"
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U replay -d replay"]
      interval: 5s
      timeout: 3s
      retries: 20
EOF
}

wait_healthy() {
  local service="$1"
  local max_tries="${2:-90}"
  for _ in $(seq 1 "${max_tries}"); do
    local cid status
    cid="$(compose ps -q "${service}" 2>/dev/null | head -n1 || true)"
    status=""
    if [[ -n "${cid}" ]]; then
      status="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "${cid}" 2>/dev/null || true)"
    fi
    if [[ "${status}" == "healthy" || "${status}" == "running" ]]; then
      return 0
    fi
    sleep 2
  done
  return 1
}

run_case() {
  local id="$1"
  local description="$2"
  shift 2
  local out="${ARTIFACTS_DIR}/${id}.log"
  log "RUN ${id}: ${description}"
  if "$@" >"${out}" 2>&1; then
    log "PASS ${id}"
  else
    log "FAIL ${id} (см. ${out})"
    cat "${out}"
    exit 1
  fi
}

cleanup() {
  rm -f "${OVERRIDE_COMPOSE}"
}
trap cleanup EXIT

write_override

log "Поднимаю test-stand: Backtest + Matching + Risk + Ledger(shadow) + ClickHouse + PostgreSQL + Kafka"
compose down -v --remove-orphans || true
if [[ "${F15_QUICK_MODE}" == "1" ]]; then
  compose up -d \
    clickhouse redpanda topics-init postgres matching risk ledger backtest
else
  compose up -d --build \
    clickhouse redpanda topics-init postgres matching risk ledger backtest
fi

wait_healthy clickhouse 90 || { log "clickhouse not healthy"; exit 1; }
wait_healthy redpanda 90 || { log "redpanda not healthy"; exit 1; }
wait_healthy postgres 90 || { log "postgres not healthy"; exit 1; }
wait_healthy matching 90 || { log "matching not running"; exit 1; }
wait_healthy risk 90 || { log "risk not running"; exit 1; }
wait_healthy ledger 90 || { log "ledger not running"; exit 1; }
wait_healthy backtest 90 || { log "backtest not running"; exit 1; }

log "Собираю F15 integration binaries"
CPATH="/opt/homebrew/include:${CPATH:-}" \
cmake --build "${ROOT_DIR}/build_ninja" --target \
  backtest_run_replay_session_test \
  backtest_shadow_namespace_test \
  backtest_kafka_replay_event_publisher_test \
  backtest_cancel_replay_session_test \
  backtest_retry_replay_session_uc_test \
  backtest_run_replay_audit_batch_test \
  backtest_compare_replay_sessions_test \
  backtest_create_replay_session_test \
  -j8

# F15 scenarios
run_case "full_replay" \
  "полный replay + solver error + no data + progress stream + shadow isolation" \
  "${ROOT_DIR}/build_ninja/bin/backtest_run_replay_session_test"

run_case "shadow_isolation" \
  "shadow isolation (namespace init/isolation contract)" \
  "${ROOT_DIR}/build_ninja/bin/backtest_shadow_namespace_test"

run_case "progress_stream" \
  "progress stream/lifecycle через replay.results" \
  "${ROOT_DIR}/build_ninja/bin/backtest_kafka_replay_event_publisher_test"

run_case "cancel" \
  "cancel replay session" \
  "${ROOT_DIR}/build_ninja/bin/backtest_cancel_replay_session_test"

run_case "retry" \
  "retry replay session (включая permission_denied)" \
  "${ROOT_DIR}/build_ninja/bin/backtest_retry_replay_session_uc_test"

run_case "audit_mode" \
  "audit-mode replay batch" \
  "${ROOT_DIR}/build_ninja/bin/backtest_run_replay_audit_batch_test"

run_case "ab_compare" \
  "A/B compare replay sessions" \
  "${ROOT_DIR}/build_ninja/bin/backtest_compare_replay_sessions_test"

run_case "no_data" \
  "no data при создании replay session" \
  "${ROOT_DIR}/build_ninja/bin/backtest_create_replay_session_test"

log "F15-TEST-2 completed: все заданные e2e сценарии пройдены"
