#!/usr/bin/env python3
"""Audit markdown links across the repository.

Reports:
  - broken: link target does not exist
  - anchor-missing: file exists but #anchor not found in target
  - folder-no-trailing-slash: link points to a folder but the path has no trailing slash (cosmetic)
  - inline-code-pseudo-links: backticked file paths that look like links but aren't

Excludes incoming-docs/, legacy_mvp/, third_party/, .git/, build/, node_modules/.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
EXCLUDE_DIRS = {"incoming-docs", "legacy_mvp", "third_party", ".git", "build", "node_modules", ".obsidian"}

LINK_RE = re.compile(r"(?<!!)\[([^\]]+)\]\(([^)]+)\)")
INLINE_CODE_FILE_RE = re.compile(r"`([^`\n]+\.(?:md|yaml|yml|proto|sql|json|sh|py|cpp|hpp|h))`")

def is_external(target: str) -> bool:
    return target.startswith(("http://", "https://", "mailto:", "tel:"))

def slugify(text: str) -> str:
    # Approximate GitHub anchor slug rules
    text = text.lower()
    text = re.sub(r"[^\w\s\-Ѐ-ӿ]", "", text)  # keep cyrillic too
    text = text.strip().replace(" ", "-")
    return text

def collect_anchors(md_path: Path) -> set[str]:
    anchors: set[str] = set()
    try:
        for line in md_path.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^#{1,6}\s+(.+?)\s*$", line)
            if m:
                anchors.add(slugify(m.group(1)))
    except Exception:
        pass
    return anchors

def in_excluded_dir(p: Path) -> bool:
    return any(part in EXCLUDE_DIRS for part in p.parts)

def main() -> int:
    md_files = [p for p in REPO_ROOT.rglob("*.md") if not in_excluded_dir(p.relative_to(REPO_ROOT))]
    broken: list[tuple[str, int, str, str]] = []
    anchors_missing: list[tuple[str, int, str, str]] = []
    pseudo: list[tuple[str, int, str]] = []

    anchor_cache: dict[Path, set[str]] = {}

    for md in md_files:
        rel_md = md.relative_to(REPO_ROOT)
        try:
            content = md.read_text(encoding="utf-8")
        except Exception:
            continue
        for lineno, line in enumerate(content.splitlines(), 1):
            for m in LINK_RE.finditer(line):
                text, target = m.group(1), m.group(2).strip()
                if is_external(target) or target.startswith("#"):
                    continue
                # Strip query strings
                target_clean = target.split(" ")[0]
                if "#" in target_clean:
                    path_part, anchor = target_clean.split("#", 1)
                else:
                    path_part, anchor = target_clean, None
                if not path_part:
                    # link like (#anchor) → already skipped
                    continue
                resolved = (md.parent / path_part).resolve()
                if not resolved.exists():
                    broken.append((str(rel_md), lineno, text, target))
                    continue
                if anchor and resolved.is_file() and resolved.suffix == ".md":
                    if resolved not in anchor_cache:
                        anchor_cache[resolved] = collect_anchors(resolved)
                    if slugify(anchor) not in anchor_cache[resolved]:
                        anchors_missing.append((str(rel_md), lineno, text, target))
            # Skip code fences & inline code that we *want* (env vars, commands)
            # Detect only patterns that look like file references inside backticks
            for m in INLINE_CODE_FILE_RE.finditer(line):
                ref = m.group(1)
                # skip obvious non-link cases (CLI flags or single tokens)
                if ref.startswith("-") or " " in ref:
                    continue
                pseudo.append((str(rel_md), lineno, ref))

    print(f"# Markdown link audit — {len(md_files)} files scanned\n")
    print(f"## Broken links: {len(broken)}\n")
    for row in broken:
        print(f"- {row[0]}:{row[1]} — [{row[2]}]({row[3]})")
    print(f"\n## Missing anchors: {len(anchors_missing)}\n")
    for row in anchors_missing:
        print(f"- {row[0]}:{row[1]} — [{row[2]}]({row[3]})")
    print(f"\n## Inline-code pseudo-links (file-like backticks): {len(pseudo)}\n")
    for row in pseudo[:200]:
        print(f"- {row[0]}:{row[1]} — `{row[2]}`")
    if len(pseudo) > 200:
        print(f"... and {len(pseudo) - 200} more")
    return 0 if not broken else 1

if __name__ == "__main__":
    sys.exit(main())
