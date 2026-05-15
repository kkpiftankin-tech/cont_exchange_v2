#!/usr/bin/env bash
set -uo pipefail

BROKER="${BROKER:-redpanda:9092}"

echo "Creating topics on broker: ${BROKER}"

# Partitions: dev default 3 (one-broker cluster). Increase in prod.
P="${P:-3}"

# Helper: create topic; if it already exists, log and continue (idempotent).
# Without this, re-running the init container fails with TOPIC_ALREADY_EXISTS.
create_topic() {
  local name=$1; shift
  local out
  out=$(rpk topic create "${name}" "$@" 2>&1) || true
  if echo "${out}" | grep -q "TOPIC_ALREADY_EXISTS"; then
    echo "${name} already exists — skipping"
  elif echo "${out}" | grep -q "OK"; then
    echo "${name} created"
  else
    echo "${name} FAILED: ${out}" >&2
    return 1
  fi
}

# Retention examples:
# - marketdata.raw: high volume => short retention
# - orders.normalized: keep for debugging/replay
# - batch.outputs: keep longer for audit
create_topic marketdata.raw       -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=3600000
create_topic orders.normalized    -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=604800000
create_topic batch.outputs        -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=604800000
create_topic execution.intents    -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=86400000
create_topic execution.reports    -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=604800000
create_topic risk.alerts          -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=2592000000

echo "Done."
