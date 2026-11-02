#!/usr/bin/env python3
"""Доля коммитов с кодом, у которых есть CR-ссылка или `Change:`-трейлер
(IN-019/ADR-086, Этап 9, одна метрика из еженедельного списка — остальные
не вычислимы сейчас, см. docs/00-methodology/known-gaps.md "Этап 9").

    python3 tools/change_adoption_report.py            # вся история
    python3 tools/change_adoption_report.py --days 7    # последние 7 дней

"Коммит с кодом" — меняет хотя бы один файл вне docs/specs/tools/.claude
(cpp/, frontend/, contracts/proto/, infra/, Testing/, ...). CR-ссылка —
`CR-[A-Z0-9]+-\\d+` в сообщении коммита (формат IN-019 §4.8 id). Трейлер —
строка `Change: <класс>` в подвале сообщения (формат §4.9, класс S/spike).
Коммит с кодом без ни одного из двух — "непокрытый".

Не путать с `tools/consistency/check.py` C-11/C-13 (not-yet-applicable) —
те проверяют согласованность уже существующих CR-файлов/трейлеров с
реестром, это считает ОХВАТ (много ли их вообще заводят).
"""
from __future__ import annotations

import argparse
import re
import subprocess
import time

CR_RE = re.compile(r"\bCR-[A-Z0-9]+-\d+\b")
CHANGE_TRAILER_RE = re.compile(r"(?m)^Change:\s*\S+")

DOCS_ONLY_PREFIXES = (
    "docs/", "specs/", "tools/", ".claude/", "incoming-docs/", "CLAUDE.md",
)


def is_code_path(path: str) -> bool:
    return not path.startswith(DOCS_ONLY_PREFIXES)


def git_log(since_days: float) -> list[str]:
    cmd = ["git", "log", "--format=%H"]
    if since_days:
        cmd.append(f"--since={since_days} days ago")
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return [h for h in out.splitlines() if h]


def commit_info(sha: str) -> tuple[str, list[str]]:
    msg = subprocess.run(
        ["git", "show", "-s", "--format=%B", sha], capture_output=True, text=True, check=True
    ).stdout
    files = subprocess.run(
        ["git", "show", "--name-only", "--format=", sha], capture_output=True, text=True, check=True
    ).stdout.splitlines()
    return msg, [f for f in files if f]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=float, default=0.0, help="окно в днях (0 = вся история)")
    args = ap.parse_args()

    shas = git_log(args.days)
    code_commits = []
    for sha in shas:
        msg, files = commit_info(sha)
        if not any(is_code_path(f) for f in files):
            continue
        has_cr = bool(CR_RE.search(msg))
        has_trailer = bool(CHANGE_TRAILER_RE.search(msg))
        code_commits.append({
            "sha": sha[:8], "subject": msg.splitlines()[0] if msg else "",
            "has_cr": has_cr, "has_trailer": has_trailer,
        })

    total = len(code_commits)
    covered = sum(1 for c in code_commits if c["has_cr"] or c["has_trailer"])

    window = f"последние {args.days:g} дн." if args.days else "вся история"
    print(f"# Доля коммитов с кодом, у которых есть CR или Change-трейлер ({window})\n")
    print(f"Коммитов с кодом: {total}")
    print(f"С CR-ссылкой или `Change:`-трейлером: {covered} "
          f"({(100 * covered / total):.1f}%)" if total else "С CR-ссылкой или `Change:`-трейлером: 0")
    if total and covered < total:
        print(f"\nБез CR/трейлера ({total - covered}):\n")
        for c in code_commits:
            if not (c["has_cr"] or c["has_trailer"]):
                print(f"- {c['sha']} {c['subject']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
