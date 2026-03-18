#!/usr/bin/env bash
set -euo pipefail

BROKER="${BROKER:-redpanda:9092}"

echo "Creating topics on broker: ${BROKER}"

# Partitions: dev default 3 (one-broker cluster). Increase in prod.
P="${P:-3}"

# Retention examples:
# - marketdata.raw: high volume => short retention
# - orders.normalized: keep for debugging/replay
# - batch.outputs: keep longer for audit
rpk topic create marketdata.raw       -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=3600000
rpk topic create orders.normalized    -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=604800000
rpk topic create batch.outputs        -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=604800000
rpk topic create execution.intents    -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=86400000
rpk topic create execution.reports    -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=604800000
rpk topic create risk.alerts          -p "${P}" -r 1 --brokers "${BROKER}" --config retention.ms=2592000000

echo "Done."
