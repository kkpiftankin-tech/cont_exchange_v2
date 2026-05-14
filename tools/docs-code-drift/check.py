#!/usr/bin/env python3
"""
Сравнивает изменённые файлы (git diff) с документацией:
- если изменены cpp/** или contracts/proto/** или infra/**, должны быть
  обновлены соответствующие docs/specs;
- если изменён proto-файл, он должен присутствовать в proto-map.yaml;
- если изменён cpp-файл, путь должен быть упомянут в code-map.yaml.

Пишет отчёт в llm/reports/latest-doc-code-drift.md и возвращает exit code != 0
при обнаружении блокирующего drift.
"""

from pathlib import Path
import subprocess
import sys

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml")
    sys.exit(2)

ROOT = Path.cwd()
REPORT = ROOT / "llm" / "reports" / "latest-doc-code-drift.md"

CODE_PREFIXES = (
    "cpp/",
    "contracts/proto/",
    "infra/",
    "docker/",
)

DOC_PREFIXES = (
    "docs/",
    "specs/",
    "llm/",
)

LEGACY_PREFIX = "legacy_mvp/"


def git_changed_files() -> list[str]:
    """Возвращает список изменённых файлов между HEAD~1 и HEAD, либо staged."""
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", "HEAD~1", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        files = [x.strip() for x in out.splitlines() if x.strip()]
        if files:
            return files
    except Exception:
        pass
    try:
        out = subprocess.check_output(
            ["git", "diff", "--name-only", "--cached"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return [x.strip() for x in out.splitlines() if x.strip()]
    except Exception:
        return []


def load_yaml(path: Path) -> dict:
    if not path.exists():
        return {}
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def main() -> int:
    changed = git_changed_files()
    # Legacy не блокируем
    changed = [p for p in changed if not p.startswith(LEGACY_PREFIX)]

    code_changed = [p for p in changed if p.startswith(CODE_PREFIXES)]
    docs_changed = [p for p in changed if p.startswith(DOC_PREFIXES)]

    code_map = load_yaml(ROOT / "specs" / "domain" / "code-map.yaml")
    proto_map = load_yaml(ROOT / "specs" / "contracts" / "proto-map.yaml")

    findings: list[str] = []

    if code_changed and not docs_changed:
        findings.append(
            "Code changed, but no docs/specs/llm files were updated."
        )

    # Все упомянутые в code-map и proto-map пути
    code_map_str = yaml.safe_dump(code_map, allow_unicode=True)
    proto_map_str = yaml.safe_dump(proto_map, allow_unicode=True)

    for path in code_changed:
        if path.startswith("contracts/proto/") and path.endswith(".proto"):
            if path not in proto_map_str:
                findings.append(
                    f"Changed proto file not in specs/contracts/proto-map.yaml: {path}"
                )

    for path in code_changed:
        if path.startswith("cpp/") and path.endswith((".hpp", ".cpp", ".h", ".cc")):
            if path not in code_map_str:
                findings.append(
                    f"Changed cpp file may not be mapped in specs/domain/code-map.yaml: {path}"
                )

    REPORT.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "# Docs-Code Drift Report",
        "",
        "## Changed code files",
    ]
    if code_changed:
        lines.extend(f"- {p}" for p in code_changed)
    else:
        lines.append("- (none)")

    lines.extend(["", "## Changed docs/spec files"])
    if docs_changed:
        lines.extend(f"- {p}" for p in docs_changed)
    else:
        lines.append("- (none)")

    lines.extend(["", "## Findings"])
    if findings:
        lines.extend(f"- {f}" for f in findings)
        lines.extend(
            [
                "",
                "## Required action",
                "Update docs/specs/traceability, or add `drift-waiver: <reason>` to PR description with reviewer sign-off.",
            ]
        )
        REPORT.write_text("\n".join(lines), encoding="utf-8")
        print(REPORT.read_text(encoding="utf-8"))
        return 1

    lines.append("- No blocking drift detected.")
    REPORT.write_text("\n".join(lines), encoding="utf-8")
    print(REPORT.read_text(encoding="utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
