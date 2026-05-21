#!/usr/bin/env python3
"""Aggregate F-11 traces from all relevant running containers.

Examples:
  python3 scripts/f11_trace_all.py
  python3 scripts/f11_trace_all.py --tail 50 --containers infra-venues-1 cex-frontend-api
  python3 scripts/f11_trace_all.py --no-follow --all
"""

from __future__ import annotations

import argparse
import queue
import subprocess
import sys
import threading
from dataclasses import dataclass
from typing import Iterable, List, Optional, Sequence, Tuple

from f11_trace_logs import extract_json, format_payload, is_relevant


DEFAULT_CONTAINER_KEYWORDS: Sequence[str] = (
    "frontend-api",
    "venues",
    "matching",
    "market_data",
    "ledger",
    "venue_health",
    "risk",
    "order_flow",
    "gateway",
    "backtest",
)

EXCLUDED_CONTAINER_KEYWORDS: Sequence[str] = (
    "clickhouse",
    "redpanda",
    "prometheus",
    "console",
    "frontend-web",
    "observability",
)

PROJECT_CONTAINER_PREFIXES: Sequence[str] = ("infra-", "cex-")


@dataclass(frozen=True)
class LogEvent:
    container: str
    line: Optional[str]


def list_running_containers() -> List[str]:
    proc = subprocess.run(
        ["docker", "ps", "--format", "{{.Names}}"],
        check=True,
        capture_output=True,
        text=True,
    )
    return [line.strip() for line in proc.stdout.splitlines() if line.strip()]


def discover_default_containers(names: Sequence[str]) -> List[str]:
    selected: List[str] = []
    for name in names:
        lowered = name.lower()
        if any(keyword in lowered for keyword in EXCLUDED_CONTAINER_KEYWORDS):
            continue
        if any(keyword in lowered for keyword in DEFAULT_CONTAINER_KEYWORDS):
            selected.append(name)
    return selected


def discover_all_project_containers(names: Sequence[str]) -> List[str]:
    selected: List[str] = []
    for name in names:
        lowered = name.lower()
        if any(lowered.startswith(prefix) for prefix in PROJECT_CONTAINER_PREFIXES):
            selected.append(name)
    return selected


def build_docker_logs_command(container: str,
                              tail: int,
                              follow: bool,
                              since: Optional[str]) -> List[str]:
    cmd = ["docker", "logs", "--tail", str(max(0, tail))]
    if since:
        cmd.extend(["--since", since])
    if follow:
        cmd.append("-f")
    cmd.append(container)
    return cmd


def stream_container(container: str,
                     tail: int,
                     follow: bool,
                     since: Optional[str],
                     out_queue: "queue.Queue[LogEvent]") -> None:
    process = subprocess.Popen(
        build_docker_logs_command(container, tail, follow, since),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        universal_newlines=True,
    )

    assert process.stdout is not None
    try:
        for line in process.stdout:
            out_queue.put(LogEvent(container=container, line=line))
    finally:
        process.stdout.close()
        process.wait(timeout=5)
        out_queue.put(LogEvent(container=container, line=None))


def format_raw_line(container: str, line: str) -> str:
    return f"[{container}] {line.rstrip()}"


def parse_args(argv: Optional[Iterable[str]]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Aggregate F-11 traces from running containers")
    parser.add_argument(
        "--containers",
        nargs="*",
        help="Explicit container names. Default: auto-discover relevant app containers.",
    )
    parser.add_argument(
        "--tail",
        type=int,
        default=0,
        help="How many historical lines to request per container before following.",
    )
    parser.add_argument(
        "--since",
        help="Pass-through docker logs --since value, for example 10m or 2026-04-25T08:00:00.",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Print unrelated/raw lines too, not only structured F-11 events.",
    )
    parser.add_argument(
        "--no-follow",
        action="store_true",
        help="Read current logs once and exit.",
    )
    parser.add_argument(
        "--list-containers",
        action="store_true",
        help="Print auto-discovered containers and exit.",
    )
    parser.add_argument(
        "--all-project-containers",
        action="store_true",
        help="Attach to every running infra-/cex- container, not only F-11 app containers.",
    )
    parser.add_argument(
        "--no-flow",
        action="store_true",
        help="Do not print inferred sequence-diagram flow lines.",
    )
    return parser.parse_args(list(argv) if argv is not None else None)


def payload_sort_key(container: str,
                     line: str,
                     payload: Optional[dict],
                     index: int) -> Tuple[str, int, str]:
    if payload is None:
        return ("~", index, container)
    ts = str(payload.get("ts") or "")
    if not ts:
        return ("~", index, container)
    return (ts, index, container)


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    running = list_running_containers()
    if args.containers:
        containers = list(args.containers)
    elif args.all_project_containers:
        containers = discover_all_project_containers(running)
    else:
        containers = discover_default_containers(running)

    if args.list_containers:
        for name in containers:
            print(name)
        return 0

    if not containers:
        print("No relevant running containers found.", file=sys.stderr)
        return 1

    follow = not args.no_follow
    print(
        "Tracing containers: " + ", ".join(containers),
        file=sys.stderr,
        flush=True,
    )

    out_queue: "queue.Queue[LogEvent]" = queue.Queue()
    threads: List[threading.Thread] = []
    for container in containers:
        thread = threading.Thread(
            target=stream_container,
            args=(container, args.tail, follow, args.since, out_queue),
            daemon=True,
        )
        thread.start()
        threads.append(thread)

    finished = 0
    try:
        buffered: List[Tuple[Tuple[str, int, str], str]] = []
        next_index = 0
        while finished < len(threads):
            event = out_queue.get()
            if event.line is None:
                finished += 1
                continue

            payload = extract_json(event.line)
            if payload is None:
                if args.all:
                    rendered = format_raw_line(event.container, event.line)
                    if follow:
                        print(rendered, flush=True)
                    else:
                        buffered.append(
                            (payload_sort_key(event.container, event.line, None, next_index), rendered)
                        )
                        next_index += 1
                continue

            if not args.all and not is_relevant(payload):
                continue

            rendered = format_payload(
                payload,
                container=event.container,
                include_flow=not args.no_flow,
            )
            if follow:
                print(rendered, flush=True)
            else:
                buffered.append(
                    (payload_sort_key(event.container, event.line, payload, next_index), rendered)
                )
                next_index += 1
    except KeyboardInterrupt:
        return 130
    finally:
        for thread in threads:
            thread.join(timeout=0.2)

    if not follow:
        for _, rendered in sorted(buffered, key=lambda item: item[0]):
            print(rendered, flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
