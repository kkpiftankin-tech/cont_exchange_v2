#!/usr/bin/env python3
"""
Генерирует docs/generated/code-map.md из specs/domain/code-map.yaml.
Используется как обзорная таблица "где что лежит".
"""

from pathlib import Path
import sys

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml")
    sys.exit(2)

ROOT = Path.cwd()
OUTPUT = ROOT / "docs" / "generated" / "code-map.md"


def main() -> int:
    code_map = yaml.safe_load(
        (ROOT / "specs" / "domain" / "code-map.yaml").read_text(encoding="utf-8")
    ) or {}

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "# Code Map (auto-generated)",
        "",
        "> Этот файл генерируется из specs/domain/code-map.yaml. Не редактируйте вручную.",
        "",
        "| Component | Path | Role | Features | Proto contracts |",
        "|---|---|---|---|---|",
    ]

    for cid, cinfo in (code_map.get("codeMap") or {}).items():
        features = ", ".join(cinfo.get("features", []) or []) or "—"
        protos = (
            ", ".join(
                Path(p).name for p in (cinfo.get("protoContracts", []) or [])
            )
            or "—"
        )
        lines.append(
            f"| {cid} | `{cinfo.get('codePath', '?')}` | {cinfo.get('role', '?')} | {features} | {protos} |"
        )

    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Generated: {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
