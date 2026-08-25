#!/usr/bin/env python3
"""F-15 Backtest / Replay staging SLO check.

This script intentionally talks to the real Gateway and Backtest metrics
endpoint. It does not seed synthetic historical data. Run it only in an
environment where ClickHouse already contains the historical batches/fills/
marketdata_snapshots required by the requested date range.
"""

from __future__ import annotations

import concurrent.futures
import json
import os
import re
import sys
import time
import uuid
from dataclasses import dataclass
from typing import Any
from urllib import error, parse, request


FINAL_STATUSES = {"completed", "failed", "cancelled"}
DEFAULT_STRATEGY = [
    {
        "symbol": "BTCUSDT",
        "side": "buy",
        "pL": 58000,
        "pH": 62000,
        "qrate": 0.5,
        "qmax": 100,
        "executionwindow": 3600,
    }
]
DEFAULT_SOLVER_CONFIG = {
    "batchintervalms": 1000,
    "maxiterations": 128,
    "tolerance": 0.000001,
    "epsilonliquidity": 0.0,
    "feemodel": {"makerfeerate": 0.0002, "takerfeerate": 0.0005},
}
DEFAULT_RISK_LIMITS = {
    "maxnotional": 10000000,
    "maxposition": 1000,
    "maxleverage": 5,
    "maxorderrate": 1000,
    "whitelist": ["BTCUSDT"],
}
DEFAULT_FEE_MODEL = {"makerfeerate": 0.0002, "takerfeerate": 0.0005}


@dataclass
class ReplayRun:
    sessionid: str
    status: str
    progressbatches: int
    totalbatches: int
    elapsed_seconds: float
    summary: dict[str, Any]


class F15SloError(RuntimeError):
    pass


def env(name: str, default: str = "") -> str:
    return os.environ.get(name, default).strip()


def env_int(name: str, default: int) -> int:
    raw = env(name)
    if not raw:
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise F15SloError(f"{name} must be an integer, got {raw!r}") from exc


def env_float(name: str, default: float) -> float:
    raw = env(name)
    if not raw:
        return default
    try:
        return float(raw)
    except ValueError as exc:
        raise F15SloError(f"{name} must be a number, got {raw!r}") from exc


def json_env(name: str, default: Any) -> Any:
    raw = env(name)
    if not raw:
        return default
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise F15SloError(f"{name} must contain valid JSON") from exc


def require_env(name: str) -> str:
    value = env(name)
    if not value:
        raise F15SloError(
            f"{name} is required. Use a date range backed by real historical "
            "fills, batchresults and marketdata_snapshots."
        )
    return value


def api_base() -> str:
    explicit = env("F15_API_BASE_URL")
    if explicit:
        return explicit.rstrip("/")
    return f"{env('F15_GATEWAY_URL', 'http://localhost:8080').rstrip('/')}/api/v1"


def headers() -> dict[str, str]:
    out = {"Content-Type": "application/json"}
    token = env("F15_AUTH_TOKEN")
    if token:
        out["Authorization"] = f"Bearer {token}"
    return out


def http_json(method: str, path: str, payload: Any | None = None) -> Any:
    url = f"{api_base()}/{path.lstrip('/')}"
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    req = request.Request(url, data=body, headers=headers(), method=method)
    try:
        with request.urlopen(req, timeout=env_float("F15_HTTP_TIMEOUT_SECONDS", 20.0)) as resp:
            raw = resp.read().decode("utf-8")
            return json.loads(raw) if raw else {}
    except error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        raise F15SloError(f"{method} {url} failed: HTTP {exc.code}: {raw}") from exc
    except error.URLError as exc:
        raise F15SloError(f"{method} {url} failed: {exc.reason}") from exc


def fetch_text(url: str) -> str:
    req = request.Request(url, headers=headers(), method="GET")
    with request.urlopen(req, timeout=env_float("F15_HTTP_TIMEOUT_SECONDS", 20.0)) as resp:
        return resp.read().decode("utf-8")


def build_payload(run_index: int) -> dict[str, Any]:
    template = json_env("F15_REPLAY_PAYLOAD_JSON", None)
    if template is not None:
        payload = dict(template)
    else:
        instruments = [item for item in env("F15_INSTRUMENTS", "BTCUSDT").split(",") if item]
        payload = {
            "name": f"{env('F15_SESSION_NAME_PREFIX', 'F15 SLO')} {uuid.uuid4()}",
            "userid": env("F15_USER_ID", "f15-slo-user"),
            "instruments": instruments,
            "daterangefrom": require_env("F15_DATE_RANGE_FROM"),
            "daterangeto": require_env("F15_DATE_RANGE_TO"),
            "strategy": json_env("F15_STRATEGY_JSON", DEFAULT_STRATEGY),
            "solverconfig": json_env("F15_SOLVER_CONFIG_JSON", DEFAULT_SOLVER_CONFIG),
            "risklimits": json_env("F15_RISK_LIMITS_JSON", DEFAULT_RISK_LIMITS),
            "feemodel": json_env("F15_FEE_MODEL_JSON", DEFAULT_FEE_MODEL),
            "rewardmode": env("F15_REWARD_MODE", "incrementalPnL"),
            "randomseed": env_int("F15_RANDOM_SEED", 42) + run_index,
        }
    payload.setdefault("name", f"F15 SLO {uuid.uuid4()}")
    return payload


def normalize_session(raw: Any) -> dict[str, Any]:
    if isinstance(raw, dict) and isinstance(raw.get("session"), dict):
        raw = raw["session"]
    if not isinstance(raw, dict):
        return {}
    return raw


def as_int(raw: Any, default: int = 0) -> int:
    try:
        return int(raw)
    except (TypeError, ValueError):
        return default


def run_one_replay(run_index: int) -> ReplayRun:
    start = time.monotonic()
    created = normalize_session(http_json("POST", "/replay/sessions", build_payload(run_index)))
    sessionid = str(created.get("sessionid") or created.get("session_id") or "")
    if not sessionid:
        raise F15SloError(f"Replay create response did not include sessionid: {created}")

    timeout_seconds = env_float("F15_POLL_TIMEOUT_SECONDS", env_float("F15_EXPECT_SECONDS", 60.0) + 120.0)
    poll_interval = env_float("F15_POLL_INTERVAL_SECONDS", 2.0)
    deadline = time.monotonic() + timeout_seconds
    current = created
    while time.monotonic() < deadline:
        current = normalize_session(http_json("GET", f"/replay/sessions/{parse.quote(sessionid)}"))
        status = str(current.get("status", "")).lower()
        if status in FINAL_STATUSES:
            break
        time.sleep(poll_interval)
    else:
        raise F15SloError(f"Replay {sessionid} did not finish within {timeout_seconds:.1f}s")

    status = str(current.get("status", "")).lower()
    elapsed = time.monotonic() - start
    summary = http_json("GET", f"/replay/sessions/{parse.quote(sessionid)}/summary")
    return ReplayRun(
        sessionid=sessionid,
        status=status,
        progressbatches=as_int(current.get("progressbatches") or current.get("progress_batches")),
        totalbatches=as_int(current.get("totalbatches") or current.get("total_batches")),
        elapsed_seconds=elapsed,
        summary=summary if isinstance(summary, dict) else {},
    )


PROM_LINE = re.compile(r"^([A-Za-z_:][A-Za-z0-9_:]*)(?:\{([^}]*)\})?\s+([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)$")
PROM_LABEL = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)="((?:\\.|[^"])*)"')


def parse_prometheus(text: str) -> dict[tuple[str, tuple[tuple[str, str], ...]], float]:
    parsed: dict[tuple[str, tuple[tuple[str, str], ...]], float] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = PROM_LINE.match(line)
        if not match:
            continue
        name, raw_labels, raw_value = match.groups()
        labels: dict[str, str] = {}
        if raw_labels:
            for label_match in PROM_LABEL.finditer(raw_labels):
                labels[label_match.group(1)] = label_match.group(2).replace(r"\"", '"')
        parsed[(name, tuple(sorted(labels.items())))] = float(raw_value)
    return parsed


def metric_delta(
    before: dict[tuple[str, tuple[tuple[str, str], ...]], float],
    after: dict[tuple[str, tuple[tuple[str, str], ...]], float],
    name: str,
    label_filter: dict[str, str],
) -> float:
    total = 0.0
    for key, after_value in after.items():
        metric_name, label_items = key
        if metric_name != name:
            continue
        labels = dict(label_items)
        if all(labels.get(k) == v for k, v in label_filter.items()):
            total += after_value - before.get(key, 0.0)
    return total


def histogram_delta(
    before: dict[tuple[str, tuple[tuple[str, str], ...]], float],
    after: dict[tuple[str, tuple[tuple[str, str], ...]], float],
    base_name: str,
    table_substring: str,
    le: str,
) -> tuple[float, float]:
    bucket = 0.0
    count = 0.0
    for key, after_value in after.items():
        metric_name, label_items = key
        labels = dict(label_items)
        if table_substring not in labels.get("table", ""):
            continue
        if metric_name == f"{base_name}_bucket" and labels.get("le") == le:
            bucket += after_value - before.get(key, 0.0)
        if metric_name == f"{base_name}_count":
            count += after_value - before.get(key, 0.0)
    return bucket, count


def fetch_metrics_snapshot() -> dict[tuple[str, tuple[tuple[str, str], ...]], float]:
    metrics_url = env("F15_BACKTEST_METRICS_URL", env("F15_METRICS_URL", "http://localhost:8087/metrics"))
    return parse_prometheus(fetch_text(metrics_url))


def summary_value(summary: dict[str, Any], *keys: str) -> Any:
    source = summary.get("summary") if isinstance(summary.get("summary"), dict) else summary
    for key in keys:
        if key in source:
            return source[key]
    return None


def validate_runs(runs: list[ReplayRun], group_elapsed: float) -> None:
    expected_batches = env_int("F15_EXPECT_BATCHES", 1000)
    expected_seconds = env_float("F15_EXPECT_SECONDS", 60.0)
    if group_elapsed > expected_seconds:
        raise F15SloError(
            f"SLO failed: {len(runs)} replay session(s) took {group_elapsed:.2f}s, "
            f"limit is {expected_seconds:.2f}s"
        )

    for run in runs:
        if run.status != "completed":
            raise F15SloError(f"Replay {run.sessionid} finished with status={run.status}")
        total = as_int(summary_value(run.summary, "totalbatches", "totalBatches", "total_batches"), run.totalbatches)
        processed = as_int(
            summary_value(run.summary, "processedbatches", "processedBatches", "processed_batches"),
            run.progressbatches,
        )
        if total < expected_batches:
            raise F15SloError(
                f"Replay {run.sessionid} processed only totalbatches={total}, "
                f"expected at least {expected_batches}"
            )
        if processed <= 0:
            raise F15SloError(f"Replay {run.sessionid} summary has no processed batches")


def validate_metrics(
    before: dict[tuple[str, tuple[tuple[str, str], ...]], float],
    after: dict[tuple[str, tuple[tuple[str, str], ...]], float],
) -> None:
    if env("F15_SKIP_METRICS", "0") == "1":
        return

    threshold = env_float("F15_AGENTLOG_INSERT_P95_MS", 50.0)
    le = str(int(threshold)) if threshold.is_integer() else str(threshold)
    bucket, count = histogram_delta(
        before,
        after,
        "backtest_replay_clickhouse_insert_latency_ms",
        "replay_agentlogs",
        le,
    )
    if count <= 0:
        raise F15SloError("No backtest_replay_clickhouse_insert_latency_ms samples for replay_agentlogs")
    ratio = bucket / count
    if ratio < 0.95:
        raise F15SloError(
            f"AgentLog insert latency p95 SLO failed: only {ratio:.2%} "
            f"of inserts were <= {threshold:g}ms"
        )

    kafka_ok_delta = metric_delta(
        before,
        after,
        "backtest_replay_kafka_publish_total",
        {"topic": "replay.results", "result": "ok"},
    )
    min_kafka_events = env_int("F15_MIN_KAFKA_OK_EVENTS", 1)
    if kafka_ok_delta < min_kafka_events:
        raise F15SloError(
            f"Expected at least {min_kafka_events} replay.results Kafka ok publish events, "
            f"got {kafka_ok_delta:g}"
        )


def main() -> int:
    concurrency = env_int("F15_CONCURRENT_SESSIONS", 1)
    if concurrency <= 0:
        raise F15SloError("F15_CONCURRENT_SESSIONS must be > 0")

    metrics_before = fetch_metrics_snapshot() if env("F15_SKIP_METRICS", "0") != "1" else {}
    group_start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(run_one_replay, i) for i in range(concurrency)]
        runs = [future.result() for future in concurrent.futures.as_completed(futures)]
    group_elapsed = time.monotonic() - group_start
    metrics_after = fetch_metrics_snapshot() if env("F15_SKIP_METRICS", "0") != "1" else {}

    validate_runs(runs, group_elapsed)
    validate_metrics(metrics_before, metrics_after)

    print(
        json.dumps(
            {
                "ok": True,
                "concurrency": concurrency,
                "elapsed_seconds": round(group_elapsed, 3),
                "sessions": [
                    {
                        "sessionid": run.sessionid,
                        "status": run.status,
                        "elapsed_seconds": round(run.elapsed_seconds, 3),
                        "progressbatches": run.progressbatches,
                        "totalbatches": run.totalbatches,
                    }
                    for run in sorted(runs, key=lambda item: item.sessionid)
                ],
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except F15SloError as exc:
        print(f"[F15-SLO] {exc}", file=sys.stderr)
        raise SystemExit(1)
