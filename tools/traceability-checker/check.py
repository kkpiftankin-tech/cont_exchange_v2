#!/usr/bin/env python3
"""
Проверяет traceability:
- каждая фича из specs/domain/feature-component-map.yaml имеет docsPath в docs/02-system/features/F-XX-*/
- каждый компонент из docs/05-components/ есть в specs/domain/code-map.yaml
- каждый codePath из code-map существует на диске
- каждый protoFile, упомянутый в feature.yaml, существует
"""

from pathlib import Path
import sys

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml")
    sys.exit(2)

ROOT = Path.cwd()


def load_yaml(path: Path) -> dict:
    if not path.exists():
        return {}
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def main() -> int:
    errors: list[str] = []

    feature_map = load_yaml(ROOT / "specs" / "domain" / "feature-component-map.yaml")
    code_map = load_yaml(ROOT / "specs" / "domain" / "code-map.yaml")
    traceability = load_yaml(ROOT / "specs" / "domain" / "traceability.yaml")

    # 1. Каждая фича из feature-component-map должна иметь docs/02-system/features/<id>-*/
    docs_features = ROOT / "docs" / "02-system" / "features"
    feature_dirs = {p.name for p in docs_features.iterdir() if p.is_dir()} if docs_features.exists() else set()

    for fid, finfo in (feature_map.get("features") or {}).items():
        docs_path = finfo.get("docsPath")
        if not docs_path:
            errors.append(f"Feature {fid}: no docsPath")
            continue
        full = ROOT / docs_path
        if not full.exists():
            errors.append(f"Feature {fid}: docsPath does not exist: {docs_path}")

    # 2. Каждый codePath из code-map должен существовать
    for cid, cinfo in (code_map.get("codeMap") or {}).items():
        code_path = cinfo.get("codePath")
        if code_path:
            full = ROOT / code_path
            if not full.exists():
                errors.append(f"Component {cid}: codePath does not exist: {code_path}")

        for key in ("keyFiles", "files"):
            files = cinfo.get(key, [])
            if isinstance(files, dict):
                # nested {important: [...], sources: [...]}
                flat = []
                for v in files.values():
                    if isinstance(v, list):
                        flat.extend(v)
                files = flat
            for f in files:
                if isinstance(f, str):
                    if not (ROOT / f).exists():
                        errors.append(f"Component {cid}: {key} missing file: {f}")

    # 3. Каждый файл из proto-contracts в feature.yaml должен существовать
    for fid, finfo in (feature_map.get("features") or {}).items():
        for pf in finfo.get("protoContracts", []) or []:
            if not (ROOT / pf).exists():
                errors.append(f"Feature {fid}: protoContract missing: {pf}")
        for cp in finfo.get("codePaths", []) or []:
            if not (ROOT / cp).exists():
                errors.append(f"Feature {fid}: codePath missing: {cp}")

    # 4. traceability.yaml: codePaths должны существовать
    for fid, finfo in (traceability.get("traceability") or {}).items():
        for cp in finfo.get("codePaths", []) or []:
            if not (ROOT / cp).exists():
                errors.append(f"Traceability {fid}: codePath missing: {cp}")
        for pf in finfo.get("protoFiles", []) or []:
            if not (ROOT / pf).exists():
                errors.append(f"Traceability {fid}: protoFile missing: {pf}")

    if errors:
        print("Traceability check errors:")
        for e in errors:
            print(f"  - {e}")
        return 1

    print("Traceability check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
