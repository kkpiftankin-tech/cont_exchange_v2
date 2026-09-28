#!/usr/bin/env python3
"""Stop-хук «денежный контур»: обязательный минимум проверки (CLAUDE.md §9, §17).

Если в рабочем дереве изменены файлы денежного контура, а субагент
`code-reviewer` в этой сессии ещё не отработал, хук один раз на каждый новый
набор изменений не даёт ходу завершиться (код 2) и объясняет причину через
stderr. Во всех остальных случаях — код 0. Это гарантия (хук), а не просьба
(CLAUDE.md текст) — см. incoming-docs, конспект "Золотая середина", Утв. 9.

Зависит от журнала .claude/logs/agents.jsonl (хук log_subagents.py).
MONEY — отправная точка; расширяй по мере появления новых денежных путей.
"""
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

MONEY = re.compile(
    r"^(cpp/ledger/"
    r"|cpp/risk/"
    r"|cpp/matching/src/(domain|app)/"
    r"|cpp/venues/"
    r"|cpp/backtest/.*(ledger|pnl)"
    r"|cpp/market_data/src/app/hedge_pnl"
    r"|cpp/common/(include/cex/common/|src/).*decimal"
    r"|contracts/proto/.*(ledger|fill|execution|hedge|risk|batch)"
    r"|infra/postgres/init\.sql)",
    re.IGNORECASE,
)


def changed_files(root: pathlib.Path) -> list:
    out = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=root, capture_output=True, text=True, timeout=5,
    ).stdout
    files = set()
    for line in out.splitlines():
        if len(line) > 3:
            files.add(line[3:].split(" -> ")[-1].strip().strip('"'))
    return sorted(files)


def reviewer_ran(log: pathlib.Path, session: str) -> bool:
    if not log.exists():
        return False
    for line in log.read_text(encoding="utf-8", errors="ignore").splitlines():
        try:
            r = json.loads(line)
        except Exception:
            continue
        if (r.get("event") == "SubagentStop" and r.get("session") == session
                and r.get("agent_type") == "code-reviewer"):
            return True
    return False


def main() -> int:
    try:
        data = json.load(sys.stdin)
    except Exception:
        return 0
    if data.get("stop_hook_active"):
        return 0

    root = pathlib.Path(os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or ".")
    try:
        money = [f for f in changed_files(root) if MONEY.search(f) and not f.endswith(".md")]
    except Exception:
        return 0
    if not money:
        return 0

    session = data.get("session_id") or "nosession"
    logdir = root / ".claude" / "logs"
    if reviewer_ran(logdir / "agents.jsonl", session):
        return 0

    stamp_src = "\n".join(
        f"{f}:{(root / f).stat().st_mtime if (root / f).exists() else 0}" for f in money
    )
    stamp = hashlib.sha1(stamp_src.encode()).hexdigest()[:12]
    marker = logdir / f"money-guard-{session[:12]}.txt"
    try:
        logdir.mkdir(parents=True, exist_ok=True)
        if marker.exists() and marker.read_text().strip() == stamp:
            return 0
        marker.write_text(stamp)
    except Exception:
        pass

    shown = "\n  ".join(money[:10]) + ("\n  ..." if len(money) > 10 else "")
    print(
        "Изменены файлы денежного контура:\n  " + shown +
        "\nЗапусти субагента code-reviewer (money invariants) по этому diff, "
        "или явно объясни пользователю, почему проверка не нужна "
        "(docs-only / revert / уже проверено в этой сессии).",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
