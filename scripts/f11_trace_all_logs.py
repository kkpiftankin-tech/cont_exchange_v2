#!/usr/bin/env python3
"""Follow and pretty-print F-11 logs from all Docker containers at once.

Examples:
  python3 scripts/f11_trace_all_logs.py
  python3 scripts/f11_trace_all_logs.py --no-follow --tail 200
  python3 scripts/f11_trace_all_logs.py --container cex-frontend-api --container infra-gateway-1
  python3 scripts/f11_trace_all_logs.py --all
"""

from __future__ import annotations

import argparse
import selectors
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable, List, Optional, Sequence, TextIO

from f11_trace_logs import extract_json, format_payload, is_relevant


@dataclass(frozen=True)
class StreamBinding:
    container: str
    stream_name: str


def list_containers(include_stopped: bool) -> List[str]:
    cmd = ["docker", "ps", "--format", "{{.Names}}"]
    if include_stopped:
        cmd.insert(2, "-a")

    try:
        result = subprocess.run(
            cmd,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        raise RuntimeError(str(exc)) from exc
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "docker ps failed")

    containers = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return sorted(dict.fromkeys(containers))


def start_log_process(
    container: str,
    follow: bool,
    tail: int,
    since: Optional[str],
) -> subprocess.Popen[str]:
    cmd = ["docker", "logs", f"--tail={max(0, tail)}"]
    if since:
        cmd.append(f"--since={since}")
    if follow:
        cmd.append("-f")
    cmd.append(container)
    try:
        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
    except OSError as exc:
        raise RuntimeError(
            f"failed to start docker logs for {container}: {exc}"
        ) from exc


def render_line(
    container: str,
    stream_name: str,
    raw_line: str,
    include_all: bool,
) -> Optional[str]:
    payload = extract_json(raw_line)
    if payload is not None:
        payload.setdefault("container", container)
        if include_all or is_relevant(payload):
            return format_payload(payload)
        return None

    stripped = raw_line.rstrip()
    if not stripped:
        return None
    if include_all or stream_name == "stderr":
        return f"[{container}:{stream_name}] {stripped}"
    return None


def register_stream(
    selector: selectors.BaseSelector,
    stream: Optional[TextIO],
    container: str,
    stream_name: str,
) -> None:
    if stream is None:
        return
    selector.register(stream, selectors.EVENT_READ, StreamBinding(container, stream_name))


def close_stream(
    selector: selectors.BaseSelector,
    stream: Optional[TextIO],
) -> None:
    if stream is None:
        return
    try:
        selector.unregister(stream)
    except Exception:
        pass
    stream.close()


def terminate_processes(processes: Sequence[subprocess.Popen[str]]) -> None:
    for process in processes:
        if process.poll() is not None:
            continue
        process.terminate()
    for process in processes:
        if process.poll() is not None:
            continue
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            process.kill()


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pretty-print F-11 logs from all Docker containers",
    )
    parser.add_argument(
        "--container",
        action="append",
        dest="containers",
        help="Only trace the given container; may be passed multiple times",
    )
    parser.add_argument(
        "--include-stopped",
        action="store_true",
        help="Also include stopped containers from docker ps -a",
    )
    parser.add_argument(
        "--tail",
        type=int,
        default=200,
        help="How many last log lines to fetch per container before following",
    )
    parser.add_argument(
        "--since",
        help="Pass through docker logs --since, for example 10m or 2026-04-25T10:00:00",
    )
    parser.add_argument(
        "--no-follow",
        action="store_true",
        help="Read the requested backlog and exit instead of following",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Do not filter unrelated logs and show raw non-JSON lines too",
    )
    args = parser.parse_args(argv)

    try:
        containers = list(args.containers or [])
        if not containers:
            containers = list_containers(include_stopped=args.include_stopped)
    except RuntimeError as exc:
        print(f"f11_trace_all_logs: {exc}", file=sys.stderr)
        return 1

    if not containers:
        print("f11_trace_all_logs: no Docker containers found", file=sys.stderr)
        return 1

    processes: List[subprocess.Popen[str]] = []
    selector = selectors.DefaultSelector()
    follow = not args.no_follow

    try:
        for container in containers:
            process = start_log_process(container, follow=follow, tail=args.tail, since=args.since)
            processes.append(process)
            register_stream(selector, process.stdout, container, "stdout")
            register_stream(selector, process.stderr, container, "stderr")

        while selector.get_map():
            ready = selector.select(timeout=0.25)
            if not ready:
                if all(process.poll() is not None for process in processes):
                    break
                continue

            for key, _ in ready:
                binding = key.data
                stream = key.fileobj
                raw_line = stream.readline()
                if raw_line == "":
                    close_stream(selector, stream)
                    continue

                rendered = render_line(
                    binding.container,
                    binding.stream_name,
                    raw_line,
                    include_all=args.all,
                )
                if rendered is None:
                    continue
                print(rendered, flush=True)
    except RuntimeError as exc:
        terminate_processes(processes)
        print(f"f11_trace_all_logs: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        terminate_processes(processes)
        return 130
    finally:
        for process in processes:
            close_stream(selector, process.stdout)
            close_stream(selector, process.stderr)
        selector.close()

    exit_code = 0
    for process in processes:
        rc = process.wait()
        if rc != 0:
            exit_code = rc if exit_code == 0 else exit_code
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
