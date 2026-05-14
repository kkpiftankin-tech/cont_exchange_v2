#!/usr/bin/env python3
"""
Проверяет, что каждый файл из contracts/proto/**/*.proto учтён в
specs/contracts/proto-map.yaml, и что каждая группа в proto-map имеет
заполненные features, components, entities.

Возвращает exit code != 0 при любой ошибке.
"""

from pathlib import Path
import sys

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml")
    sys.exit(2)

ROOT = Path.cwd()
PROTO_ROOT = ROOT / "contracts" / "proto"
PROTO_MAP_PATH = ROOT / "specs" / "contracts" / "proto-map.yaml"


def main() -> int:
    errors = []

    if not PROTO_ROOT.exists():
        errors.append(f"contracts/proto not found: {PROTO_ROOT}")
    if not PROTO_MAP_PATH.exists():
        errors.append(f"specs/contracts/proto-map.yaml not found: {PROTO_MAP_PATH}")

    if errors:
        for e in errors:
            print(f"ERROR: {e}")
        return 1

    proto_files = sorted(
        str(p.relative_to(ROOT)) for p in PROTO_ROOT.rglob("*.proto")
    )

    proto_map = yaml.safe_load(PROTO_MAP_PATH.read_text(encoding="utf-8")) or {}

    mapped = set()
    for group, item in (proto_map.get("protoContracts") or {}).items():
        for f in item.get("files", []) or []:
            mapped.add(f)
        if not item.get("features"):
            errors.append(f"{group}: no features mapped")
        if not item.get("components"):
            errors.append(f"{group}: no components mapped")
        if not item.get("entities"):
            errors.append(f"{group}: no entities mapped")

    for f in proto_files:
        if f not in mapped:
            errors.append(f"Unmapped proto file: {f}")

    for f in mapped:
        full = ROOT / f
        if not full.exists():
            errors.append(f"proto-map references missing file: {f}")

    if errors:
        print("Proto contract map errors:")
        for e in errors:
            print(f"  - {e}")
        return 1

    print(f"Proto contract map check passed ({len(proto_files)} files).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
