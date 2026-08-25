#!/usr/bin/env sh
set -eu

log() {
  echo "[CI-TEST] $*"
}

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

cap_parallelism() {
  value="$1"
  cap="$2"
  if [ "${value}" -gt "${cap}" ]; then
    echo "${cap}"
    return
  fi
  echo "${value}"
}

retry_command() {
  attempts="$1"
  delay_sec="$2"
  shift 2

  attempt=1
  while true; do
    if "$@"; then
      return 0
    fi
    status=$?
    if [ "${attempt}" -ge "${attempts}" ]; then
      return "${status}"
    fi
    log "Retrying command after failure (${attempt}/${attempts}): $*"
    sleep "${delay_sec}"
    attempt=$((attempt + 1))
  done
}

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
CLICKHOUSE_IMAGE="${CLICKHOUSE_IMAGE:-clickhouse/clickhouse-server:24.8}"
BUILD_JOBS="${BUILD_JOBS:-$(detect_parallelism)}"
TEST_JOBS="${TEST_JOBS:-${BUILD_JOBS}}"
COMPOSE_PARALLEL_LIMIT="${COMPOSE_PARALLEL_LIMIT:-${BUILD_JOBS}}"
F11_BUILD_JOBS="${F11_BUILD_JOBS:-$(cap_parallelism "${BUILD_JOBS}" 2)}"
F11_COMPOSE_PARALLEL_LIMIT="${F11_COMPOSE_PARALLEL_LIMIT:-$(cap_parallelism "${COMPOSE_PARALLEL_LIMIT}" 2)}"
F11_VERBOSE="${F11_VERBOSE:-1}"
if [ -n "${RUN_F11_QUALITY_TESTS:-}" ]; then
  RUN_F11_QUALITY_TESTS_EXPLICIT=1
else
  RUN_F11_QUALITY_TESTS=1
fi
DOCKER_BUILD_PROGRESS="${DOCKER_BUILD_PROGRESS:-plain}"
KEEP_DOCKER_ENV="${KEEP_DOCKER_ENV:-0}"
if [ -n "${F11_TIMEOUT_SCALE:-}" ]; then
  :
elif [ -n "${bamboo_planKey:-}${BAMBOO_PLANKEY:-}${CI:-}" ]; then
  F11_TIMEOUT_SCALE=2
else
  F11_TIMEOUT_SCALE=1
fi

# Default policy:
# - CI/Bamboo: disable F11 integration tests by default.
# - Local runs: keep F11 integration tests enabled by default.
# Explicit RUN_F11_INTEGRATION_TESTS value always has priority.
if [ -n "${RUN_F11_INTEGRATION_TESTS:-}" ]; then
  RUN_F11_INTEGRATION_TESTS_EXPLICIT=1
elif [ -n "${bamboo_planKey:-}${BAMBOO_PLANKEY:-}${CI:-}" ]; then
  RUN_F11_INTEGRATION_TESTS=0
else
  RUN_F11_INTEGRATION_TESTS=1
fi

# Default policy:
# - CI/Bamboo: disable heavy F11 load tests by default.
# - Local runs: keep load tests enabled by default.
# Explicit RUN_F11_LOAD_TESTS value always has priority.
if [ -n "${RUN_F11_LOAD_TESTS:-}" ]; then
  RUN_F11_LOAD_TESTS_EXPLICIT=1
elif [ -n "${bamboo_planKey:-}${BAMBOO_PLANKEY:-}${CI:-}" ]; then
  RUN_F11_LOAD_TESTS=0
else
  RUN_F11_LOAD_TESTS=1
fi

# Branch-aware test policy.
# Bamboo populates ${bamboo.planRepository.branchName} (env: bamboo_planRepository_branchName).
# Map branch -> profile to optimize CI surface per branch:
#   main/master/release/*  -> full     (ctest + quality)
#   dev/develop            -> standard (ctest)
#   anything else          -> light    (ctest, all F11 suites off)
# Heavy F11 integration/load remain opt-in via RUN_F11_*=1 to avoid CI flakes.
# Explicit RUN_F11_*_EXPLICIT=1 (set above when caller provided the value) blocks profile override.
CI_BRANCH_NAME="${CI_BRANCH_NAME:-${bamboo_planRepository_branchName:-}}"

if [ -z "${CI_BRANCH_PROFILE:-}" ]; then
  case "${CI_BRANCH_NAME}" in
    main|master|release/*) CI_BRANCH_PROFILE=full ;;
    dev|develop)           CI_BRANCH_PROFILE=standard ;;
    *)                     CI_BRANCH_PROFILE=light ;;
  esac
fi

case "${CI_BRANCH_PROFILE}" in
  full)
    [ -z "${RUN_F11_QUALITY_TESTS_EXPLICIT:-}" ] && RUN_F11_QUALITY_TESTS=1
    ;;
  standard)
    [ -z "${RUN_F11_QUALITY_TESTS_EXPLICIT:-}" ] && RUN_F11_QUALITY_TESTS=0
    ;;
  light|*)
    [ -z "${RUN_F11_INTEGRATION_TESTS_EXPLICIT:-}" ] && RUN_F11_INTEGRATION_TESTS=0
    [ -z "${RUN_F11_QUALITY_TESTS_EXPLICIT:-}" ]     && RUN_F11_QUALITY_TESTS=0
    [ -z "${RUN_F11_LOAD_TESTS_EXPLICIT:-}" ]        && RUN_F11_LOAD_TESTS=0
    ;;
esac

if [ -f "${SCRIPT_DIR}/../docker/Dockerfile.service" ]; then
  ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
elif [ -f "./docker/Dockerfile.service" ]; then
  ROOT_DIR="$(pwd)"
elif [ -n "${bamboo_build_working_directory:-}" ] && [ -f "${bamboo_build_working_directory}/docker/Dockerfile.service" ]; then
  ROOT_DIR="${bamboo_build_working_directory}"
elif [ -n "${BAMBOO_BUILD_WORKING_DIRECTORY:-}" ] && [ -f "${BAMBOO_BUILD_WORKING_DIRECTORY}/docker/Dockerfile.service" ]; then
  ROOT_DIR="${BAMBOO_BUILD_WORKING_DIRECTORY}"
else
  echo "Cannot resolve repository root with docker/Dockerfile.service" >&2
  echo "SCRIPT_DIR=${SCRIPT_DIR}, PWD=$(pwd)" >&2
  exit 1
fi

JUNIT_OUTPUT="${JUNIT_OUTPUT:-${ROOT_DIR}/artifacts/test-results/ctest-junit.xml}"

cat <<EOF

==============================================================
 CI BRANCH-AWARE TEST PROFILE
--------------------------------------------------------------
 branch                    : ${CI_BRANCH_NAME:-unknown}
 profile                   : ${CI_BRANCH_PROFILE}
 ctest (build+unit)        : always on
 RUN_F11_INTEGRATION_TESTS : ${RUN_F11_INTEGRATION_TESTS}
 RUN_F11_QUALITY_TESTS     : ${RUN_F11_QUALITY_TESTS}
 RUN_F11_LOAD_TESTS        : ${RUN_F11_LOAD_TESTS}
 F11_TIMEOUT_SCALE         : ${F11_TIMEOUT_SCALE}
 bamboo_planKey            : ${bamboo_planKey:-}
 bamboo_buildNumber        : ${bamboo_buildNumber:-}
==============================================================

EOF

inject_junit_branch_properties() {
  junit_path="$1"
  if [ ! -f "${junit_path}" ]; then
    return 0
  fi
  tmp="${junit_path}.tmp"
  awk -v branch="${CI_BRANCH_NAME:-unknown}" \
      -v profile="${CI_BRANCH_PROFILE}" \
      -v run_int="${RUN_F11_INTEGRATION_TESTS}" \
      -v run_qual="${RUN_F11_QUALITY_TESTS}" \
      -v run_load="${RUN_F11_LOAD_TESTS}" \
      -v plan_key="${bamboo_planKey:-}" \
      -v build_num="${bamboo_buildNumber:-}" '
    BEGIN { injected = 0 }
    {
      print
      if (!injected && match($0, /<testsuite[^>]*>/)) {
        printf "    <properties>\n"
        printf "      <property name=\"ci.branch\" value=\"%s\"/>\n", branch
        printf "      <property name=\"ci.profile\" value=\"%s\"/>\n", profile
        printf "      <property name=\"ci.run_f11_integration\" value=\"%s\"/>\n", run_int
        printf "      <property name=\"ci.run_f11_quality\" value=\"%s\"/>\n", run_qual
        printf "      <property name=\"ci.run_f11_load\" value=\"%s\"/>\n", run_load
        printf "      <property name=\"ci.bamboo.plan_key\" value=\"%s\"/>\n", plan_key
        printf "      <property name=\"ci.bamboo.build_number\" value=\"%s\"/>\n", build_num
        printf "    </properties>\n"
        injected = 1
      }
    }
  ' "${junit_path}" > "${tmp}" && mv "${tmp}" "${junit_path}"
  log "Injected branch profile properties into ${junit_path}"
}

bamboo_key="${bamboo_planKey:-}"
if [ -z "${bamboo_key}" ]; then
  bamboo_key="${BAMBOO_PLANKEY:-local}"
fi

bamboo_number="${bamboo_buildNumber:-}"
if [ -z "${bamboo_number}" ]; then
  bamboo_number="${BAMBOO_BUILD_NUMBER:-0}"
fi
suffix_raw="${bamboo_key}-${bamboo_number}"
suffix="$(printf '%s' "${suffix_raw}" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9][^a-z0-9]*/-/g; s/^-*//; s/-*$//')"
if [ -z "${suffix}" ]; then
  suffix="local"
fi

NETWORK_NAME="${NETWORK_NAME:-cex-ci-${suffix}-net}"
CLICKHOUSE_CONTAINER="${CLICKHOUSE_CONTAINER:-cex-ci-${suffix}-clickhouse}"
BUILDER_IMAGE="${BUILDER_IMAGE:-cex-builder-test:${suffix}}"
TEST_CONTAINER="${TEST_CONTAINER:-cex-ci-${suffix}-tests}"
F11_RUNNER_IMAGE="${F11_RUNNER_IMAGE:-docker:27-cli}"
junit_dir="$(dirname "${JUNIT_OUTPUT}")"
junit_file="$(basename "${JUNIT_OUTPUT}")"
mkdir -p "${junit_dir}"

cleanup() {
  if ! command -v docker >/dev/null 2>&1; then
    return 0
  fi
  docker compose -f "${ROOT_DIR}/infra/docker-compose.dev.yml" down -v --remove-orphans >/dev/null 2>&1 || true
  docker rm -f "${TEST_CONTAINER}" >/dev/null 2>&1 || true
  docker rm -f "${CLICKHOUSE_CONTAINER}" >/dev/null 2>&1 || true
  docker network rm "${NETWORK_NAME}" >/dev/null 2>&1 || true
}

ensure_network() {
  if ! docker network inspect "${NETWORK_NAME}" >/dev/null 2>&1; then
    log "Creating docker network: ${NETWORK_NAME}"
    docker network create "${NETWORK_NAME}" >/dev/null
  fi
}

wait_for_clickhouse() {
  i=0
  while [ "${i}" -lt 60 ]; do
    if docker exec "${CLICKHOUSE_CONTAINER}" clickhouse-client --query 'SELECT 1' >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
    i=$((i + 1))
  done

  echo "clickhouse did not become ready in time" >&2
  docker logs "${CLICKHOUSE_CONTAINER}" || true
  return 1
}

if ! command -v docker >/dev/null 2>&1; then
  echo "docker cli is not available in PATH" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "docker daemon is not reachable (check /var/run/docker.sock mapping)" >&2
  exit 1
fi

if [ "${KEEP_DOCKER_ENV}" != "1" ]; then
  trap cleanup 0 HUP INT TERM
fi

log "Repository root: ${ROOT_DIR}"
log "Parallelism: BUILD_JOBS=${BUILD_JOBS}, TEST_JOBS=${TEST_JOBS}, COMPOSE_PARALLEL_LIMIT=${COMPOSE_PARALLEL_LIMIT}"
log "F11 Parallelism: BUILD_JOBS=${F11_BUILD_JOBS}, COMPOSE_PARALLEL_LIMIT=${F11_COMPOSE_PARALLEL_LIMIT}"
log "Verbosity: F11_VERBOSE=${F11_VERBOSE}, DOCKER_BUILD_PROGRESS=${DOCKER_BUILD_PROGRESS}"
export COMPOSE_PARALLEL_LIMIT
export BUILDKIT_PROGRESS="${DOCKER_BUILD_PROGRESS}"

ensure_network
log "Starting ClickHouse container: ${CLICKHOUSE_CONTAINER}"
docker rm -f "${CLICKHOUSE_CONTAINER}" >/dev/null 2>&1 || true
docker run -d \
  --name "${CLICKHOUSE_CONTAINER}" \
  --network "${NETWORK_NAME}" \
  --network-alias clickhouse \
  -e CLICKHOUSE_SKIP_USER_SETUP=1 \
  "${CLICKHOUSE_IMAGE}" >/dev/null

log "Waiting for ClickHouse readiness"
wait_for_clickhouse

log "Building builder image: ${BUILDER_IMAGE}"
retry_command 3 5 docker build \
  -f "${ROOT_DIR}/docker/Dockerfile.service" \
  --target builder \
  --build-arg BUILD_JOBS="${BUILD_JOBS}" \
  -t "${BUILDER_IMAGE}" \
  "${ROOT_DIR}"

log "Preparing test container: ${TEST_CONTAINER}"
ensure_network
docker rm -f "${TEST_CONTAINER}" >/dev/null 2>&1 || true
docker create \
  --name "${TEST_CONTAINER}" \
  --network "${NETWORK_NAME}" \
  -e CLICKHOUSE_TEST_HOST=clickhouse \
  -e CLICKHOUSE_TEST_PORT=8123 \
  -e CLICKHOUSE_TEST_URL=http://clickhouse:8123 \
  -e CLICKHOUSE_TEST_USER=default \
  -e CLICKHOUSE_TEST_PASSWORD= \
  "${BUILDER_IMAGE}" \
  ctest --test-dir /tmp/build --parallel "${TEST_JOBS}" --output-on-failure --output-junit "/tmp/${junit_file}" >/dev/null

test_status=0
log "Running ctest suite"
docker start -a "${TEST_CONTAINER}" || test_status=$?
docker cp "${TEST_CONTAINER}:/tmp/${junit_file}" "${JUNIT_OUTPUT}" >/dev/null 2>&1 || true
inject_junit_branch_properties "${JUNIT_OUTPUT}"

if [ "${test_status}" -ne 0 ]; then
  echo "Tests failed (exit code ${test_status})" >&2
  exit "${test_status}"
fi

echo "JUnit report written to: ${JUNIT_OUTPUT}"

if [ "${RUN_F11_INTEGRATION_TESTS}" = "1" ]; then
  log "Running F11 integration tests (F11-TEST-2)"
  docker compose -f "${ROOT_DIR}/infra/docker-compose.dev.yml" down -v --remove-orphans >/dev/null 2>&1 || true
  f11_status=0
  if command -v bash >/dev/null 2>&1; then
    log "Executing F11 integration script via local bash"
    (
      cd "${ROOT_DIR}" &&
      BUILD_JOBS="${F11_BUILD_JOBS}" TEST_JOBS="${TEST_JOBS}" COMPOSE_PARALLEL_LIMIT="${F11_COMPOSE_PARALLEL_LIMIT}" VERBOSE="${F11_VERBOSE}" F11_TIMEOUT_SCALE="${F11_TIMEOUT_SCALE}" F11_DEX_MDS_INGESTION_CHECK=0 bash Testing/f11_test2_e2e.sh
    ) || f11_status=$?
  else
    log "Local bash is unavailable, executing F11 integration script via ${F11_RUNNER_IMAGE}"
    docker run --rm \
      -v /var/run/docker.sock:/var/run/docker.sock \
      -v "${ROOT_DIR}:${ROOT_DIR}" \
      -w "${ROOT_DIR}" \
      "${F11_RUNNER_IMAGE}" \
      sh -lc "apk add --no-cache bash || true; BUILD_JOBS='${F11_BUILD_JOBS}' TEST_JOBS='${TEST_JOBS}' COMPOSE_PARALLEL_LIMIT='${F11_COMPOSE_PARALLEL_LIMIT}' VERBOSE='${F11_VERBOSE}' F11_TIMEOUT_SCALE='${F11_TIMEOUT_SCALE}' F11_DEX_MDS_INGESTION_CHECK='0' bash Testing/f11_test2_e2e.sh" \
      || f11_status=$?
  fi
  docker compose -f "${ROOT_DIR}/infra/docker-compose.dev.yml" down -v --remove-orphans >/dev/null 2>&1 || true

  if [ "${f11_status}" -ne 0 ]; then
    echo "F11 integration tests failed (exit code ${f11_status})" >&2
    exit "${f11_status}"
  fi

  echo "F11 integration tests passed."
else
  log "Skipping F11 integration tests by RUN_F11_INTEGRATION_TESTS=${RUN_F11_INTEGRATION_TESTS}"
fi

if [ "${RUN_F11_QUALITY_TESTS}" = "1" ]; then
  log "Running F11 quality tests (F11-TEST-4)"
  docker compose -f "${ROOT_DIR}/infra/docker-compose.dev.yml" down -v --remove-orphans >/dev/null 2>&1 || true
  f11_quality_status=0
  if command -v bash >/dev/null 2>&1; then
    log "Executing F11 quality script via local bash"
    (
      cd "${ROOT_DIR}" &&
      BUILD_JOBS="${F11_BUILD_JOBS}" TEST_JOBS="${TEST_JOBS}" COMPOSE_PARALLEL_LIMIT="${F11_COMPOSE_PARALLEL_LIMIT}" VERBOSE="${F11_VERBOSE}" F11_TIMEOUT_SCALE="${F11_TIMEOUT_SCALE}" bash Testing/f11_test4_quality.sh
    ) || f11_quality_status=$?
  else
    log "Local bash is unavailable, executing F11 quality script via ${F11_RUNNER_IMAGE}"
    docker run --rm \
      -v /var/run/docker.sock:/var/run/docker.sock \
      -v "${ROOT_DIR}:${ROOT_DIR}" \
      -w "${ROOT_DIR}" \
      "${F11_RUNNER_IMAGE}" \
      sh -lc "apk add --no-cache bash || true; BUILD_JOBS='${F11_BUILD_JOBS}' TEST_JOBS='${TEST_JOBS}' COMPOSE_PARALLEL_LIMIT='${F11_COMPOSE_PARALLEL_LIMIT}' VERBOSE='${F11_VERBOSE}' F11_TIMEOUT_SCALE='${F11_TIMEOUT_SCALE}' bash Testing/f11_test4_quality.sh" \
      || f11_quality_status=$?
  fi
  docker compose -f "${ROOT_DIR}/infra/docker-compose.dev.yml" down -v --remove-orphans >/dev/null 2>&1 || true

  if [ "${f11_quality_status}" -ne 0 ]; then
    echo "F11 quality tests failed (exit code ${f11_quality_status})" >&2
    exit "${f11_quality_status}"
  fi

  echo "F11 quality tests passed."
fi

if [ "${RUN_F11_LOAD_TESTS}" = "1" ]; then
  log "Running F11 load tests (F11-TEST-3)"
  docker compose -f "${ROOT_DIR}/infra/docker-compose.dev.yml" down -v --remove-orphans >/dev/null 2>&1 || true
  f11_load_status=0
  if command -v bash >/dev/null 2>&1; then
    log "Executing F11 load script via local bash"
    (
      cd "${ROOT_DIR}" &&
      BUILD_JOBS="${F11_BUILD_JOBS}" TEST_JOBS="${TEST_JOBS}" COMPOSE_PARALLEL_LIMIT="${F11_COMPOSE_PARALLEL_LIMIT}" VERBOSE="${F11_VERBOSE}" F11_TIMEOUT_SCALE="${F11_TIMEOUT_SCALE}" bash Testing/f11_test3_load.sh
    ) || f11_load_status=$?
  else
    log "Local bash is unavailable, executing F11 load script via ${F11_RUNNER_IMAGE}"
    docker run --rm \
      -v /var/run/docker.sock:/var/run/docker.sock \
      -v "${ROOT_DIR}:${ROOT_DIR}" \
      -w "${ROOT_DIR}" \
      "${F11_RUNNER_IMAGE}" \
      sh -lc "apk add --no-cache bash || true; BUILD_JOBS='${F11_BUILD_JOBS}' TEST_JOBS='${TEST_JOBS}' COMPOSE_PARALLEL_LIMIT='${F11_COMPOSE_PARALLEL_LIMIT}' VERBOSE='${F11_VERBOSE}' F11_TIMEOUT_SCALE='${F11_TIMEOUT_SCALE}' bash Testing/f11_test3_load.sh" \
      || f11_load_status=$?
  fi
  docker compose -f "${ROOT_DIR}/infra/docker-compose.dev.yml" down -v --remove-orphans >/dev/null 2>&1 || true

  if [ "${f11_load_status}" -ne 0 ]; then
    echo "F11 load tests failed (exit code ${f11_load_status})" >&2
    exit "${f11_load_status}"
  fi

  echo "F11 load tests passed."
fi
