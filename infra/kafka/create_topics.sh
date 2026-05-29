#!/usr/bin/env bash
set -euo pipefail

BROKER="${BROKER:-redpanda:9092}"

echo "Creating topics on broker: ${BROKER}"

# Partitions: dev default 3 (one-broker cluster). Increase in prod.
P="${P:-3}"

# Idempotent topic creation:
# - treat TOPIC_ALREADY_EXISTS as success
# - fail fast on any other broker / auth / config errors
create_topic() {
  local topic="$1"
  local retention_ms="$2"
  local output=""

  if output="$(rpk topic create "${topic}" -p "${P}" -r 1 --brokers "${BROKER}" --config "retention.ms=${retention_ms}" 2>&1)"; then
    printf '%s\n' "${output}"
    return 0
  fi

  printf '%s\n' "${output}"
  if grep -q "TOPIC_ALREADY_EXISTS" <<<"${output}" || grep -q "topic_already_exists" <<<"${output}"; then
    echo "Topic '${topic}' already exists, continuing."
    return 0
  fi

  echo "Failed to create topic '${topic}'." >&2
  return 1
}

retention_for_topic() {
  local topic="$1"
  local default_ms="$2"
  local key
  key="$(tr '[:lower:].-' '[:upper:]__' <<<"${topic}")"
  local var_name="RETENTION_MS_${key}"
  local configured="${!var_name:-}"
  if [[ -n "${configured}" ]]; then
    echo "${configured}"
    return
  fi
  echo "${default_ms}"
}

# Retention examples:
# - marketdata.raw: high volume => short retention
# - orders.normalized: keep for debugging/replay
# - batch.outputs: keep longer for audit
# - fills: per-fill external events for downstream consumers
# - venue.liquidity.fob: liquidity curves, keep moderate retention
# - venue.synthetic: compatibility SyntheticFlowOrder stream for matching
create_topic marketdata.raw "$(retention_for_topic marketdata.raw 3600000)"
create_topic orders.normalized "$(retention_for_topic orders.normalized 604800000)"
create_topic batch.outputs "$(retention_for_topic batch.outputs 604800000)"
create_topic fills "$(retention_for_topic fills 604800000)"
create_topic execution.intents "$(retention_for_topic execution.intents 604800000)" # key = hedge_flow_id
create_topic execution.venue "$(retention_for_topic execution.venue 604800000)" # key = hedge_flow_id
create_topic execution.reports "$(retention_for_topic execution.reports 604800000)"
create_topic risk.alerts "$(retention_for_topic risk.alerts 2592000000)"

create_topic venue.liquidity.fob "$(retention_for_topic venue.liquidity.fob 86400000)" # key = {venue_id}|{instrument_symbol}
create_topic venue.health "$(retention_for_topic venue.health 604800000)" # key = {venue_id}
create_topic venue.synthetic "$(retention_for_topic venue.synthetic 86400000)" # key = {venue_id}|{instrument_symbol}
create_topic venue.snapshots "$(retention_for_topic venue.snapshots 3600000)" # key = {venue_id}|{instrument_symbol}

create_topic replay.commands "$(retention_for_topic replay.commands 604800000)"
create_topic replay.results "$(retention_for_topic replay.results 604800000)"

# F-20 Live Venue Simulator. Strict isolation from live execution (ADR-015):
# sim ExecutionReports never share the live execution.venue topic.
create_topic sim.config "$(retention_for_topic sim.config 604800000)" # key = sim_session_id (SimConfigEvent; VenueSimRouter hot reload)
create_topic sim.execution.venue "$(retention_for_topic sim.execution.venue 604800000)" # key = hedge_flow_id (sim ExecutionReport, same contract as live)
create_topic sim.execution.annotations "$(retention_for_topic sim.execution.annotations 604800000)" # key = report_id (SimExecutionAnnotation sidecar)
create_topic sim.alerts "$(retention_for_topic sim.alerts 2592000000)" # key = sim_session_id (SimAlert)

create_topic logs "$(retention_for_topic logs 604800000)"
create_topic metrics "$(retention_for_topic metrics 604800000)"

echo "Done."
