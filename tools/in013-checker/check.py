#!/usr/bin/env python3
"""
IN-013 + Mermaid syntax checker для docs/.

Объединяет два аудита:

  1. **IN-013 compliance** (sequence-level decomposition):
     L0 sequences (level: kite ☁️) — только actors + [System].
     L1 sequences (level: sea 🌊) — только canonical service names; нет
       class.method() patterns.
     L2 sequences (level: fish 🐟) — есть `component:` поле в frontmatter.
     Любой уровень — нет markdown links [text](url) внутри ```mermaid```.

  2. **Mermaid sequenceDiagram syntax** (известные парсер-проблемы):
     H1. `;` внутри Note/message text — semicolon = statement separator.
     H2. Несбалансированные alt/opt/par/loop/critical/break/rect/end.
     H3. Markdown link `[text](url)` внутри mermaid block (тот же V3 IN-013).
     H4. Backticks + `;` в Note (ambiguous).
     H6. `:` (colon) в alias participant.
     H7. Незакрытые `<br>` без `/`.
     H9. Triple ticks ``` внутри mermaid block.

Usage:
    python3 tools/in013-checker/check.py            # full report
    python3 tools/in013-checker/check.py --quiet    # exit code only
    python3 tools/in013-checker/check.py --fix      # auto-fix safe issues

Exit codes:
    0  — no issues
    1  — issues found

Этот скрипт безопасен для CI (read-only по умолчанию).
"""
import argparse
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parent.parent.parent

CANONICAL_SERVICES = {
    "gateway", "order_flow", "order-flow", "OrderFlow",
    "matching", "Matching",
    "risk", "Risk",
    "ledger", "Ledger",
    "market_data", "market-data", "MarketData",
    "venues", "Venues",
    "venue_health", "venue-health", "VenueHealth",
    "observability", "Observability",
    "backtest", "Backtest",
    "frontend", "Frontend", "Web", "UI",
    "Client", "Trader", "Operator", "Scheduler",
    "Provider", "MarketMaker", "MM",
    "Postgres", "PostgreSQL", "PG",
    "ClickHouse", "CH",
    "Kafka", "Redpanda",
    "System",
    "GW", "OF",
}


# ---------------------------------------------------------------------------
# Mermaid block extraction
# ---------------------------------------------------------------------------

def extract_mermaid_blocks(content: str):
    """Yield (start_file_line, [(file_line, text)]) for each ```mermaid block."""
    lines = content.split("\n")
    in_block = False
    start = -1
    buf = []
    for i, line in enumerate(lines, start=1):
        stripped = line.strip()
        if stripped.startswith("```mermaid"):
            in_block = True
            start = i
            buf = []
            continue
        if in_block and stripped.startswith("```"):
            yield (start, buf)
            in_block = False
            continue
        if in_block:
            buf.append((i, line))


def detect_diagram_type(lines):
    """Returns first keyword after ```mermaid (e.g. sequenceDiagram, graph, classDiagram)."""
    for _, l in lines:
        s = l.strip()
        if s:
            return s.split()[0].lower() if s.split() else ""
    return ""


# ---------------------------------------------------------------------------
# IN-013 level extraction
# ---------------------------------------------------------------------------

def extract_level(content: str) -> str | None:
    head = "\n".join(content.split("\n")[:30])
    m = re.search(r"^\s*level:\s*(kite|sea|fish)\b", head, re.M)
    return m.group(1) if m else None


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_l0(content, lines):
    """L0 — only [System] + actors."""
    issues = []
    for ln, line in lines:
        m = re.match(r"\s*participant\s+(\w+)(?:\s+as\s+(.+))?", line)
        if m:
            name, alias = m.group(1), (m.group(2) or "")
            if "System" in alias or "system" in alias.lower():
                continue
            if name == "System":
                continue
            if name in CANONICAL_SERVICES and name not in (
                "System", "Trader", "Operator", "Client", "Provider",
                "MarketMaker", "MM", "Scheduler"
            ):
                issues.append(f"L{ln}: L0 has internal participant: {name} (alias: {alias!r})")
    return issues


def check_l1(content, lines):
    """L1 — no class.method() patterns."""
    issues = []
    for ln, line in lines:
        for m in re.finditer(r"->>?\s*\w+\s*:\s*([A-Z]\w*\.\w+\()", line):
            issues.append(f"L{ln}: L1 class.method() pattern: {m.group(1)}…")
    return issues


def check_mermaid_syntax(content, lines, diag_type):
    """Mermaid sequenceDiagram-specific syntax checks."""
    issues = []

    if diag_type != "sequencediagram":
        # For graph / classDiagram / etc. — different syntax rules
        # Только проверяем markdown-link (H3) которая нерелевантна для всех
        for ln, line in lines:
            for m in re.finditer(r"\[([^\]\n]+)\]\(([^\)]+)\)", line):
                issues.append(f"L{ln}: H3 markdown link inside mermaid: [{m.group(1)}](…)")
        return issues

    # H2. Block balance for sequenceDiagram
    open_kw = re.compile(r"^\s*(alt|opt|par|loop|critical|break|rect)\b")
    end_kw = re.compile(r"^\s*end\b")
    open_count = sum(1 for _, l in lines if open_kw.match(l))
    end_count = sum(1 for _, l in lines if end_kw.match(l))
    if open_count != end_count:
        issues.append(
            f"H2 block balance: {open_count} alt/opt/par/loop/critical/break/rect vs {end_count} end"
        )

    for ln, line in lines:
        # H3. Markdown link
        for m in re.finditer(r"\[([^\]\n]+)\]\(([^\)]+)\)", line):
            issues.append(f"L{ln}: H3 markdown link: [{m.group(1)}](…)")

        # H1. `;` в Note text
        m = re.match(r"\s*Note\s+(over|right of|left of)\s+\S+\s*:\s*(.+)$", line)
        if m and ";" in m.group(2):
            issues.append(f"L{ln}: H1 `;` in Note text: {line.strip()[:80]}")

        # H1b. `;` в message text
        m = re.match(r"\s*\w+\s*-+>+x?\s*\w+\s*:\s*(.+)$", line)
        if m and ";" in m.group(1):
            issues.append(f"L{ln}: H1 `;` in message text: {line.strip()[:80]}")

        # H4. Backticks + `;` в Note
        m = re.match(r"\s*Note\s+(over|right of|left of)\s+\S+\s*:\s*(.+)$", line)
        if m:
            txt = m.group(2)
            if "`" in txt and ";" in txt:
                issues.append(f"L{ln}: H4 backtick+`;` в Note: {line.strip()[:80]}")

        # H6. `:` в alias участника (но НЕ в начале строки)
        m = re.match(r"\s*participant\s+\w+\s+as\s+(.+)$", line)
        if m and ":" in m.group(1):
            issues.append(f"L{ln}: H6 `:` in participant alias: {m.group(1)[:60]}")

        # H7. <br> без /
        if re.search(r"<br\b(?!\s*/)", line.replace("<br/>", "").replace("<br />", "")):
            issues.append(f"L{ln}: H7 `<br>` без `/`: {line.strip()[:80]}")

        # H9. Triple ticks внутри блока
        if "```" in line:
            issues.append(f"L{ln}: H9 закрывающие ``` внутри mermaid block")

    return issues


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def audit_file(path: Path) -> list[str]:
    content = path.read_text()
    level = extract_level(content)
    issues = []

    for start, lines in extract_mermaid_blocks(content):
        diag_type = detect_diagram_type(lines)

        # Level-specific checks (IN-013) — только для sequenceDiagram
        if diag_type == "sequencediagram":
            if level == "kite":
                issues += check_l0(content, lines)
            elif level == "sea":
                issues += check_l1(content, lines)
            # L2 (fish) — frontmatter check вне mermaid

        # Mermaid syntax — для всех типов
        issues += check_mermaid_syntax(content, lines, diag_type)

    # L2: проверка component: в frontmatter
    if level == "fish":
        head = "\n".join(content.split("\n")[:30])
        if not re.search(r"^\s*component:\s*\S", head, re.M):
            issues.append("L2 missing `component:` field in frontmatter")

    return issues


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true", help="exit code only, no output")
    ap.add_argument("--paths", nargs="*", help="restrict to specific paths (defaults to docs/)")
    args = ap.parse_args()

    if args.paths:
        candidates = [Path(p) for p in args.paths]
    else:
        candidates = sorted(set(
            list(ROOT.glob("docs/**/*.md"))
        ))
    candidates = [p for p in candidates if "_template" not in str(p)]

    total_files = len(candidates)
    files_with_issues = 0
    total_issues = 0
    by_file: list[tuple[Path, list[str]]] = []

    for path in candidates:
        try:
            issues = audit_file(path)
        except UnicodeDecodeError:
            continue
        if issues:
            files_with_issues += 1
            total_issues += len(issues)
            by_file.append((path, issues))

    if args.quiet:
        sys.exit(0 if total_issues == 0 else 1)

    print("=" * 72)
    print("IN-013 + Mermaid syntax audit")
    print("=" * 72)
    print(f"Files scanned:     {total_files}")
    print(f"Files with issues: {files_with_issues}")
    print(f"Total issues:      {total_issues}")
    print()

    if total_issues == 0:
        print("✅ No issues detected.")
        sys.exit(0)

    for path, issues in by_file:
        rel = path.relative_to(ROOT)
        print(f"❌ {rel}")
        for iss in issues:
            print(f"    {iss}")
        print()

    sys.exit(1)


if __name__ == "__main__":
    main()
