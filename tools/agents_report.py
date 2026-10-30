#!/usr/bin/env python3
"""Сводка по журналу вызовов субагентов (.claude/logs/agents.jsonl).

    python3 tools/agents_report.py            # вся история
    python3 tools/agents_report.py --days 7    # последние 7 дней
    python3 tools/agents_report.py --log path.jsonl

Печатает Markdown: вызовы по типам агентов (число, сессии, медиана
длительности, средний объём ответа, среднее число вызовов инструментов по
транскрипту) и распределение числа вызовов на сессию. Это данные для
еженедельной калибровки .claude/rules/agent-routing.md (см.
incoming-docs, конспект "Золотая середина...", §6.3, таблица показателей):
какие агенты зовутся чаще, чем нужно, какие — никогда, есть ли сессии с
изменённым денежным контуром без единого вызова code-reviewer.
"""
import argparse
import collections
import json
import pathlib
import statistics
import time


def load(log: pathlib.Path, since: float) -> list:
    rows = []
    if not log.exists():
        return rows
    for line in log.read_text(encoding="utf-8", errors="ignore").splitlines():
        try:
            r = json.loads(line)
        except Exception:
            continue
        if r.get("ts", 0) >= since:
            rows.append(r)
    return rows


def tool_uses(transcript) -> int:
    """Число вызовов инструментов в транскрипте субагента (по возможности)."""
    if not transcript:
        return -1
    p = pathlib.Path(transcript)
    if not p.exists():
        return -1
    n = 0
    for line in p.read_text(encoding="utf-8", errors="ignore").splitlines():
        try:
            obj = json.loads(line)
        except Exception:
            continue
        content = (obj.get("message") or {}).get("content")
        if obj.get("type") == "assistant" and isinstance(content, list):
            n += sum(1 for c in content if isinstance(c, dict) and c.get("type") == "tool_use")
    return n


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default=".claude/logs/agents.jsonl")
    ap.add_argument("--days", type=float, default=0.0)
    a = ap.parse_args()

    since = time.time() - a.days * 86400 if a.days else 0.0
    rows = load(pathlib.Path(a.log), since)

    starts = {r["agent_id"]: r for r in rows if r.get("event") == "SubagentStart" and r.get("agent_id")}
    stops = [r for r in rows if r.get("event") == "SubagentStop"]

    by_type = collections.defaultdict(list)
    for s in stops:
        st = starts.get(s.get("agent_id"))
        dur = s["ts"] - st["ts"] if st else None
        by_type[s.get("agent_type") or "?"].append({
            "session": s.get("session"), "dur": dur,
            "chars": s.get("result_chars", 0), "tools": tool_uses(s.get("transcript")),
        })

    sessions = collections.Counter(s.get("session") for s in stops)
    for r in rows:  # сессии без единого субагента видны по событию SessionStart
        if r.get("event") == "SessionStart" and r.get("session") not in sessions:
            sessions[r.get("session")] = 0

    print(f"# Вызовы субагентов: {len(stops)} за {len(sessions)} сессий\n")
    print("| агент | вызовов | сессий | медиана, с | ответ, симв. | инструментов |")
    print("|---|---:|---:|---:|---:|---:|")
    for t, xs in sorted(by_type.items(), key=lambda kv: -len(kv[1])):
        durs = [x["dur"] for x in xs if x["dur"] is not None]
        tools = [x["tools"] for x in xs if x["tools"] >= 0]
        med = f"{statistics.median(durs):.0f}" if durs else "—"
        tl = f"{statistics.mean(tools):.1f}" if tools else "—"
        ch = f"{statistics.mean(x['chars'] for x in xs):.0f}"
        print(f"| {t} | {len(xs)} | {len({x['session'] for x in xs})} | {med} | {ch} | {tl} |")

    buckets = collections.Counter()
    for n in sessions.values():
        buckets["0" if n == 0 else "1" if n == 1 else "2" if n == 2
                 else "3-5" if n <= 5 else "6+"] += 1
    print("\n| вызовов на сессию | сессий |\n|---|---:|")
    for b in ["0", "1", "2", "3-5", "6+"]:
        print(f"| {b} | {buckets.get(b, 0)} |")


if __name__ == "__main__":
    main()
