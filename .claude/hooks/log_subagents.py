#!/usr/bin/env python3
"""Журнал вызовов субагентов для хуков SessionStart / SubagentStart / SubagentStop.

Пишет по одной JSON-строке на событие в .claude/logs/agents.jsonl (каталог
logs/ игнорируется git). SessionStart нужен, чтобы в сводке (tools/agents_report.py)
были видны и сессии без единого субагента — иначе "золотую середину" нечем
калибровать (см. incoming-docs, конспект "Золотая середина", §6.3).

Ничего не печатает и никогда не блокирует работу: при любой ошибке
завершается с кодом 0. Регистрация — .claude/settings.json hooks.SessionStart /
SubagentStart / SubagentStop.
"""
import json
import os
import pathlib
import sys
import time


def main() -> int:
    try:
        data = json.load(sys.stdin)
    except Exception:
        return 0

    root = pathlib.Path(os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or ".")
    log = root / ".claude" / "logs" / "agents.jsonl"
    event = data.get("hook_event_name")

    rec = {
        "ts": round(time.time(), 3),
        "event": event,
        "session": data.get("session_id"),
        "agent_type": data.get("agent_type"),
        "agent_id": data.get("agent_id"),
    }
    if event == "SessionStart":
        rec["source"] = data.get("source")
    if event == "SubagentStop":
        rec["result_chars"] = len(data.get("last_assistant_message") or "")
        rec["transcript"] = data.get("agent_transcript_path")

    try:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
