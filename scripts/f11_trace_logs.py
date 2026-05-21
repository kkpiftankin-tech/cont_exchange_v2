#!/usr/bin/env python3
"""Pretty-printer for F-11 external-venues logs.

Examples:
  python3 scripts/f11_trace_logs.py --file /tmp/venues.log
  python3 scripts/f11_trace_logs.py --file /tmp/venues.log --follow
  docker logs -f cex-frontend-api | python3 scripts/f11_trace_logs.py
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Tuple


SOURCE_HINTS: List[Tuple[re.Pattern[str], str]] = [
    (re.compile(r"Admin UI .* venue", re.IGNORECASE), "frontend/api/server.js"),
    (re.compile(r"Persisted venue_config|Updated venue_config|Deactivated venue", re.IGNORECASE),
     "cpp/venues/src/main.cpp"),
    (re.compile(r"Initializing CEX venue adapter|Connected CEX venue adapter|Subscribed CEX market-data channels|Fetched raw CEX snapshot|Consumed raw CEX", re.IGNORECASE),
     "cpp/venues/src/infra/cex_ws_rest_adapter.cpp"),
    (re.compile(r"Initializing DEX venue adapter|Connected DEX venue adapter|Subscribed DEX market-data channels|Fetched raw DEX snapshot|Consumed raw DEX|Sent DEX venue order command", re.IGNORECASE),
     "cpp/venues/src/infra/dex_amm_rpc_adapter.cpp"),
    (re.compile(r"Published marketdata\.raw|Venues execution consumer received intent|Produced execution report", re.IGNORECASE),
     "cpp/venues/src/app/venues_loop.cpp"),
    (re.compile(r"Skipped duplicate raw venue snapshot|Normalized raw venue snapshot|Published venue\.snapshot", re.IGNORECASE),
     "cpp/venues/src/app/snapshot_producer.cpp"),
    (re.compile(r"Stored venue snapshot in ClickHouse"), "cpp/venues/src/infra/snapshot_clickhouse_writer.cpp"),
    (re.compile(r"Venue curve candidate|forced L1 fallback|degraded to OFF|Adjusted venue tau by 24h turnover cap|Published venue\.liquidity\.fob|Published venue\.synthetic|Recorded L3 impact calibration sample", re.IGNORECASE),
     "cpp/venues/src/app/liquidity_curve_producer.cpp"),
    (re.compile(r"Published venue\.health"), "cpp/venues/src/infra/venue_observability_producer.cpp"),
    (re.compile(r"MarketData received venue\.(snapshot|liquidity\.fob)|MarketData received execution\.venue", re.IGNORECASE),
     "cpp/market_data/src/infra/kafka_consumer.cpp"),
    (re.compile(r"Stored liquidity curve"), "cpp/market_data/src/app/market_data_uc.cpp"),
    (re.compile(r"Stored venue\.liquidity\.fob in ClickHouse"), "cpp/market_data/src/infra/clickhouse/clickhouse_liquidity_curve_storage.cpp"),
    (re.compile(r"Updated external liquidity curve|Planner venue comparisons for order leg|Built execution intent from external fill|Produced batch\.outputs", re.IGNORECASE),
     "cpp/matching/src/app/matching_loop.cpp"),
    (re.compile(r"Published execution intent"), "cpp/matching/src/infra/kafka/execution_intents_producer.cpp"),
    (re.compile(r"Risk consumed "), "cpp/risk/src/infra/kafka_consumer.cpp"),
    (re.compile(r"Ledger applied execution report|Ledger stored execution intent|Ledger applied batch", re.IGNORECASE),
     "cpp/ledger/src/infra/kafka_consumers.cpp"),
    (re.compile(r"Aggregating venue health event|Skipping already aggregated venue health event", re.IGNORECASE),
     "cpp/venue_health/src/infra/kafka/kafka_consumer.cpp"),
]


COMMON_KEYS = {
    "ts",
    "level",
    "log_level",
    "msg",
    "service",
    "component",
    "participant",
    "stage",
    "topic",
    "source_file",
}


@dataclass(frozen=True)
class SequenceFlow:
    source: str
    target: str
    message: str


DETAIL_PRIORITY: List[Tuple[re.Pattern[str], Sequence[str]]] = [
    (re.compile(r"venue_config", re.IGNORECASE),
     ("venue", "routing_mode", "adapter_mode", "curve_level", "synthetic_enabled")),
    (re.compile(r"marketdata\.raw order_book", re.IGNORECASE),
     ("venue", "symbol", "venue_symbol", "sequence", "status", "bid_levels", "ask_levels",
      "best_bid", "best_ask", "payload_bytes")),
    (re.compile(r"marketdata\.raw symbol_meta", re.IGNORECASE),
     ("venue", "symbol", "venue_symbol", "maker_fee", "taker_fee", "tick_size", "lot_size",
      "min_notional", "min_qty", "max_qty")),
    (re.compile(r"marketdata\.raw ticker", re.IGNORECASE),
     ("venue", "symbol", "venue_symbol", "bid", "ask", "mid", "spread", "base_volume_24h",
      "quote_volume_24h")),
    (re.compile(r"raw CEX depth delta|raw CEX snapshot", re.IGNORECASE),
     ("venue", "symbol", "venue_symbol", "status", "sequence", "first_update_id",
      "final_update_id", "bid_deltas", "ask_deltas", "bid_levels", "ask_levels",
      "best_bid", "best_ask", "volume_24h", "result")),
    (re.compile(r"raw CEX trade|raw CEX ticker", re.IGNORECASE),
     ("venue", "symbol", "trade_qty", "volume_24h", "volume_authoritative", "event")),
    (re.compile(r"raw DEX pool state", re.IGNORECASE),
     ("venue", "symbol", "pool_address", "block_number", "tick", "liquidity",
      "reserve_base", "reserve_quote", "ticks_count", "finalized")),
    (re.compile(r"raw DEX swaps|raw DEX snapshot", re.IGNORECASE),
     ("venue", "symbol", "status", "sequence", "swap_events", "events_received",
      "new_swap_events", "swap_events_cached", "volume_24h", "best_bid", "best_ask",
      "mid_price", "spread")),
    (re.compile(r"Normalized raw venue snapshot|Published venue\.snapshot", re.IGNORECASE),
     ("venue", "symbol", "status", "sequence", "age_ms", "bid_levels", "ask_levels",
      "best_bid", "best_ask", "mid_price", "spread", "maker_fee", "taker_fee",
      "tick_size", "lot_size", "volume_24h", "snapshot_latency_ms", "payload_bytes",
      "storage_saved")),
    (re.compile(r"Venue curve candidate|venue\.liquidity\.fob|L1 fallback|tau by 24h", re.IGNORECASE),
     ("venue", "symbol", "requested_level", "max_level", "candidate_level", "level",
      "quality_action", "degradation_reason", "reason", "confidence", "min_confidence",
      "epsilon1", "epsilon2", "epsilon3", "epsilon1_degrade", "epsilon1_disable",
      "epsilon2_degrade", "epsilon2_disable", "epsilon3_degrade", "epsilon3_disable",
      "min_l3_confidence", "min_l2_confidence", "min_l1_confidence", "tau_ms",
      "base_tau_ms", "q_max", "volume_24h_qty", "raw_hourly_rate", "hourly_turnover_cap",
      "tau_adjustment_reason", "amm_path", "amm_direct_vs_virtual_price_bps",
      "amm_direct_vs_virtual_cost_rel", "bid_q_points", "ask_q_points", "curve_id",
      "snapshot_id")),
    (re.compile(r"venue\.synthetic", re.IGNORECASE),
     ("venue", "symbol", "side", "synthetic_id", "curve_id", "snapshot_id",
      "p_l", "p_h", "q_rate", "q_max", "liquidity_source", "published", "stored")),
    (re.compile(r"venue\.health|Aggregating venue health event", re.IGNORECASE),
     ("venue", "status", "routing", "breaker", "health_score", "reason")),
    (re.compile(r"Published execution intent|execution consumer received intent|execution report|Send.*order command", re.IGNORECASE),
     ("venue", "symbol", "intent_id", "report_id", "venue_order_id", "order_id", "batch_id",
      "venue_symbol", "side", "target_qty", "limit_price", "filled_qty", "remaining_qty",
      "average_price", "status", "accepted", "error_code", "http_code")),
    (re.compile(r"batch\.outputs|Ledger applied batch", re.IGNORECASE),
     ("batch_id", "fills", "clear_prices", "executed_rates", "active_before", "active_after",
      "solve_time_ms", "batch_cycle_time_ms", "execution_intents_attempted",
      "execution_intents_published")),
    (re.compile(r"liquidity curve.*ClickHouse|snapshot in ClickHouse", re.IGNORECASE),
     ("table", "venue", "symbol", "curve_id", "snapshot_id", "level", "confidence",
      "event_time_ms", "status", "best_bid", "best_ask", "volume_24h", "ts_ms")),
]


def _pairs_to_payload(pairs: Sequence[Tuple[str, object]]) -> Dict[str, object]:
    payload: Dict[str, object] = {}
    duplicate_counts: Dict[str, int] = {}

    for key, value in pairs:
        if key not in payload:
            payload[key] = value
            continue

        duplicate_counts[key] = duplicate_counts.get(key, 1) + 1
        if key == "level" and "log_level" not in payload:
            payload["log_level"] = payload["level"]
            payload["level"] = value
            continue

        payload[f"{key}__dup{duplicate_counts[key]}"] = value
        payload[key] = value

    return payload


def extract_json(line: str) -> Optional[Dict[str, object]]:
    stripped = line.strip()
    if not stripped:
        return None

    candidates = [stripped]
    if "{" in stripped and "}" in stripped:
        candidates.append(stripped[stripped.find("{"): stripped.rfind("}") + 1])

    for candidate in candidates:
        try:
            payload = json.loads(candidate, object_pairs_hook=_pairs_to_payload)
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            return payload
    return None


def infer_source_file(payload: Dict[str, object]) -> str:
    source_file = str(payload.get("source_file") or "").strip()
    if source_file:
        return source_file

    msg = str(payload.get("msg") or "")
    for pattern, hint in SOURCE_HINTS:
        if pattern.search(msg):
            return hint
    return "unknown"


def infer_participant(payload: Dict[str, object]) -> str:
    participant = str(payload.get("participant") or "").strip()
    if participant:
        return participant

    service = str(payload.get("service") or "").strip()
    component = str(payload.get("component") or "").strip()
    if service == "venues" and component == "external_venues_connector":
        return "External Venues Connector"
    if service == "venues" and component == "venue_market_data_normalizer":
        return "Venue Market Data Normalizer"
    if service == "venues" and component == "venue_liquidity_curve_builder":
        return "Venue Liquidity Curve Builder"
    if service == "venues" and component == "venue_execution_adapter":
        return "Venue Execution Adapter"
    if service == "matching" and component == "execution_planning":
        return "Execution Planning"
    if service == "matching" and component == "matching_backend":
        return "Matching Backend"
    if service == "market_data":
        return "Market Data Service"
    if service == "risk":
        return "Risk Manager"
    if service == "ledger":
        return "Settlement & Ledger"
    if service == "venue_health":
        return "Venue Health & Routing Service"
    if component == "snapshot_clickhouse_writer" or "clickhouse" in component:
        return "ClickHouse"
    if service:
        return service
    return "unknown"


def infer_sequence_flow(payload: Dict[str, object]) -> Optional[SequenceFlow]:
    msg = str(payload.get("msg") or "")
    topic = str(payload.get("topic") or "")
    participant = infer_participant(payload)
    service = str(payload.get("service") or "")
    component = str(payload.get("component") or "")

    if participant == "Admin UI":
        return SequenceFlow("Admin UI", "PostgreSQL venue_config", msg)

    if "venue_config" in msg.lower() and service == "venues":
        return SequenceFlow("PostgreSQL venue_config", "External Venues Connector", msg)

    if topic == "marketdata.raw" or participant == "External Venues Connector":
        return SequenceFlow("External Venues Connector", "Venue Market Data Normalizer", msg)

    if topic == "venue.snapshots" and participant == "Venue Market Data Normalizer":
        return SequenceFlow("Venue Market Data Normalizer", "Kafka venue.snapshots", msg)

    if topic == "venue.snapshots" and participant == "Market Data Service":
        return SequenceFlow("Kafka venue.snapshots", "Market Data Service", msg)

    if topic == "venue.snapshots" and participant == "ClickHouse":
        return SequenceFlow("Kafka venue.snapshots", "ClickHouse", msg)

    if topic == "venue.liquidity.fob" and participant == "Venue Liquidity Curve Builder":
        return SequenceFlow("Venue Liquidity Curve Builder", "Kafka venue.liquidity.fob", msg)

    if topic == "venue.synthetic" and participant == "Venue Liquidity Curve Builder":
        return SequenceFlow("Venue Liquidity Curve Builder", "Kafka venue.synthetic", msg)

    if topic == "venue.health" and participant in {
        "Venue Health & Routing Service",
        "Venue Health & Routing",
    } and service == "venues":
        return SequenceFlow("Venue Health & Routing", "Kafka venue.health", msg)

    if participant == "Venue Health & Routing Service" and service == "venue_health":
        return SequenceFlow("Kafka venue.health", "Venue Health & Routing", msg)

    if topic == "venue.liquidity.fob" and participant == "Market Data Service":
        return SequenceFlow("Kafka venue.liquidity.fob", "Market Data Service", msg)

    if topic == "venue.liquidity.fob" and participant == "Risk Manager":
        return SequenceFlow("Kafka venue.liquidity.fob", "Risk Manager", msg)

    if participant == "Matching Backend":
        if "execution intent" in msg.lower():
            return SequenceFlow("Execution Planning", "Kafka execution.intents", msg)
        return SequenceFlow("Kafka venue.liquidity.fob", "Matching Backend", msg)

    if topic == "execution.intents" and participant == "Venue Execution Adapter":
        return SequenceFlow("Kafka execution.intents", "Venue Execution Adapter", msg)

    if topic == "execution.venue" and participant == "Venue Execution Adapter":
        return SequenceFlow("Venue Execution Adapter", "Kafka execution.venue", msg)

    if topic == "execution.venue" and participant == "Settlement & Ledger":
        return SequenceFlow("Kafka execution.venue", "Settlement & Ledger", msg)

    if topic == "execution.venue" and participant == "Risk Manager":
        return SequenceFlow("Kafka execution.venue", "Risk Manager", msg)

    if topic == "execution.venue" and participant == "Market Data Service":
        return SequenceFlow("Kafka execution.venue", "Market Data Service", msg)

    if component == "snapshot_clickhouse_writer":
        return SequenceFlow("Venue Market Data Normalizer", "ClickHouse", msg)

    if "clickhouse_liquidity_curve_storage" in component:
        return SequenceFlow("Venue Liquidity Curve Builder", "ClickHouse", msg)

    return None


def is_relevant(payload: Dict[str, object]) -> bool:
    msg = str(payload.get("msg") or "")
    service = str(payload.get("service") or "")
    topic = str(payload.get("topic") or "")
    component = str(payload.get("component") or "")

    if service in {
        "frontend-api",
        "venues",
        "matching",
        "market_data",
        "risk",
        "ledger",
        "venue_health",
    }:
        return True

    if topic in {
        "marketdata.raw",
        "venue.snapshots",
        "venue.liquidity.fob",
        "venue.synthetic",
        "venue.health",
        "execution.venue",
        "execution.intents",
        "batch.outputs",
        "venue_config",
    }:
        return True

    if component in {
        "external_venues_connector",
        "venue_market_data_normalizer",
        "venue_liquidity_curve_builder",
        "venue_execution_adapter",
        "execution_planning",
        "matching_backend",
        "snapshot_clickhouse_writer",
        "clickhouse_liquidity_curve_storage",
    }:
        return True

    return bool(msg and any(pattern.search(msg) for pattern, _ in SOURCE_HINTS))


def select_priority_keys(payload: Dict[str, object]) -> Sequence[str]:
    msg = str(payload.get("msg") or "")
    for pattern, keys in DETAIL_PRIORITY:
        if pattern.search(msg):
            return keys
    return (
        "venue",
        "symbol",
        "status",
        "topic",
        "reason",
        "curve_id",
        "snapshot_id",
        "intent_id",
        "batch_id",
    )


def format_kv(key: str, value: object) -> str:
    return f"{key}={value}"


def ordered_details(payload: Dict[str, object]) -> Tuple[List[str], List[str]]:
    preferred = []
    seen = set()
    for key in select_priority_keys(payload):
        value = payload.get(key)
        if value in (None, "", "-"):
            continue
        preferred.append(format_kv(key, value))
        seen.add(key)

    extras = []
    for key in sorted(payload):
        if key in COMMON_KEYS or key in seen:
            continue
        value = payload.get(key)
        if value in (None, "", "-"):
            continue
        extras.append(format_kv(key, value))
    return preferred, extras


def format_payload(payload: Dict[str, object],
                   container: Optional[str] = None,
                   include_flow: bool = True) -> str:
    ts = str(payload.get("ts") or "-")
    level = str(payload.get("log_level") or payload.get("level") or "-")
    msg = str(payload.get("msg") or "-")
    participant = infer_participant(payload)
    stage = str(payload.get("stage") or "-")
    topic = str(payload.get("topic") or "-")
    source_file = infer_source_file(payload)
    preferred, extras = ordered_details(payload)
    flow = infer_sequence_flow(payload) if include_flow else None

    scope = participant if container is None else f"{container} | {participant}"
    lines = [f"[{ts}] {level:<5} [{scope}] {msg}"]
    if flow is not None:
        lines.append(f"  flow: {flow.source} -> {flow.target} message={flow.message}")
    meta_bits = []
    if stage not in ("", "-"):
        meta_bits.append(f"stage={stage}")
    if topic not in ("", "-"):
        meta_bits.append(f"topic={topic}")
    if meta_bits:
        lines.append("  " + " ".join(meta_bits))
    if preferred:
        lines.append("  " + " ".join(preferred))
    if extras:
        lines.append("  extra: " + " ".join(extras))
    lines.append(f"  source: {source_file}")
    return "\n".join(lines)


def iter_lines(path: Optional[Path], follow: bool) -> Iterator[str]:
    if path is None:
        while True:
            line = sys.stdin.readline()
            if line:
                yield line
                continue
            if not follow:
                break
            time.sleep(0.25)
        return

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        if follow:
            handle.seek(0, 2)
        while True:
            line = handle.readline()
            if line:
                yield line
                continue
            if not follow:
                break
            time.sleep(0.25)


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Pretty-print F-11 external-venues logs")
    parser.add_argument("--file", type=Path, help="Read logs from file instead of stdin")
    parser.add_argument("--follow", action="store_true", help="Keep tailing the input")
    parser.add_argument("--all", action="store_true", help="Do not filter unrelated logs")
    args = parser.parse_args(argv)

    for raw_line in iter_lines(args.file, args.follow):
        payload = extract_json(raw_line)
        if payload is None:
            if args.all:
                print(raw_line.rstrip())
            continue
        if not args.all and not is_relevant(payload):
            continue
        print(format_payload(payload), flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
