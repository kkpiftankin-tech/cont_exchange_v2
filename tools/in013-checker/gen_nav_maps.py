#!/usr/bin/env python3
"""
IN-013: генерирует Navigation Map blocks для feature READMEs + UC use-case.md.
Data-driven: читает feature.yaml + scan directories + builds tables.

Idempotent: пропускает файлы, где `🧭 Navigation Map` уже есть.

Usage:
    python3 /tmp/gen_nav_maps.py [--dry-run]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("Need PyYAML: pip install pyyaml")
    sys.exit(1)

ROOT = Path("/Users/aleksandrpiftankin/Documents/projects/cont_exchange_v2.0")
FEATURES_DIR = ROOT / "docs/02-system/features"
UCS_DIR = ROOT / "docs/02-system/use-cases"
SVC_SEQS_DIR = ROOT / "docs/05-components/sequences"
COMPS_DIR = ROOT / "docs/05-components"


def find_l1_sequence(fid_num: str, uc_id: str) -> Path | None:
    """Find L1 service sequence for given UC."""
    # Standard naming
    cand = SVC_SEQS_DIR / f"SEQ-F{fid_num}-{uc_id}-services.md"
    if cand.exists():
        return cand
    # F-11 / F-12 / F-15 alternative naming: SEQ-F{N}-{NN}-*-services.md matching UC number
    uc_num = uc_id.split("-")[-1]  # e.g., UC-F11-03 → 03
    for cand in SVC_SEQS_DIR.glob(f"SEQ-F{fid_num}-{uc_num}-*-services.md"):
        return cand
    return None


def find_l0_sequence(uc_dir: Path) -> Path | None:
    """Find L0 system sequence."""
    seqs_dir = uc_dir / "sequences"
    if not seqs_dir.exists():
        return None
    cand = list(seqs_dir.glob("SEQ-*-system.md"))
    return cand[0] if cand else None


def find_component_overview(component: str) -> Path | None:
    """Find component overview.md, trying canonical + kebab naming."""
    for variant in (component, component.replace("_", "-"), component.replace("-", "_")):
        cand = COMPS_DIR / variant / "overview.md"
        if cand.exists():
            return cand
    return None


def find_component_l2_sequences(component: str) -> list[Path]:
    """List L2 sequences for given component."""
    for variant in (component, component.replace("_", "-"), component.replace("-", "_")):
        d = COMPS_DIR / variant / "sequences"
        if d.exists():
            return sorted(d.glob("SEQ-*.md"))
    return []


def discover_ucs_for_feature(fid_num: str) -> list[tuple[str, Path]]:
    """Returns list of (uc_id, uc_dir) for the feature."""
    ucs = []
    for uc_dir in sorted(UCS_DIR.glob(f"UC-F{fid_num}-*/")):
        if not (uc_dir / "use-case.md").exists():
            continue
        # Extract UC-ID from name: UC-F04-01-run-batch-clearing → UC-F04-01
        parts = uc_dir.name.split("-")
        uc_id = "-".join(parts[:3])  # UC-F04-01
        ucs.append((uc_id, uc_dir))
    return ucs


def rel(target: Path, src: Path) -> str:
    """Relative path from src to target."""
    return str(Path("/" + str(target.relative_to(ROOT))).relative_to(
        Path("/" + str(src.parent.relative_to(ROOT)))
    )).replace("..", "..")


def rel2(target: Path, base: Path) -> str:
    """Markdown-friendly relative path."""
    import os.path
    return os.path.relpath(target, base).replace("\\", "/")


# ---------------------------------------------------------------------------
# Generators
# ---------------------------------------------------------------------------

def gen_feature_nav_map(feature_yaml: dict, ucs: list, readme: Path) -> str:
    """Build the Navigation Map block for a feature README."""
    fid = feature_yaml["feature"]["id"]
    components = feature_yaml.get("primaryComponents", [])

    out = []
    out.append("## 🧭 Navigation Map (IN-013 drill-down)")
    out.append("")
    out.append("Эта секция — **карта документации сверху вниз** для фичи.")
    out.append("Каждый уровень имеет свой ответ на «что/как», и каждая ссылка")
    out.append("ведёт на следующий уровень детализации.")
    out.append("")
    out.append("```text")
    out.append("   ┌─ Уровень ──────────────┬─ Артефакт ─────────────────────────────────┐")
    out.append("☁️ L0 │ Что система делает    │ Эта страница + L0 system sequence(s) ниже  │")
    out.append("🌊 L1 │ Какие функции у фичи?  │ Use Cases (таблица ниже)                   │")
    out.append("   │ Какие сервисы участвуют?│ L1 service sequences (per-UC)              │")
    out.append("🐟 L2 │ Из каких классов       │ Component overviews + L2 sequences         │")
    out.append("   │ состоит сервис?        │                                            │")
    out.append("💻 src │ Код                    │ cpp/<component>/src/...                    │")
    out.append("   └────────────────────────┴────────────────────────────────────────────┘")
    out.append("```")
    out.append("")

    # Use Cases table
    out.append("## 📋 Use Cases (L1 🌊)")
    out.append("")
    if not ucs:
        out.append("> Use Cases пока не определены (feature в статусе planned/draft).")
        out.append("")
    else:
        out.append("| UC | Имя | L0 sequence ☁️ | L1 sequence 🌊 |")
        out.append("| --- | --- | --- | --- |")
        for uc_id, uc_dir in ucs:
            uc_name = uc_dir.name.replace(uc_id + "-", "").replace("-", " ").title()
            uc_link = rel2(uc_dir / "use-case.md", readme.parent)
            l0 = find_l0_sequence(uc_dir)
            l0_link = f"[{l0.stem}]({rel2(l0, readme.parent)})" if l0 else "—"
            fid_num = uc_id.split("-")[1].lstrip("F")  # UC-F04-01 → 04
            l1 = find_l1_sequence(fid_num, uc_id)
            l1_link = f"[{l1.stem}]({rel2(l1, readme.parent)})" if l1 else "—"
            out.append(f"| [{uc_id}]({uc_link}) | {uc_name} | {l0_link} | {l1_link} |")
        out.append("")

    # Components Involved table
    out.append("## 🏗 Components Involved")
    out.append("")
    out.append("| Component | Drill-down → component overview / L2 sequences |")
    out.append("| --- | --- |")
    for comp in components:
        ov = find_component_overview(comp)
        if ov:
            ov_link = f"[{comp}]({rel2(ov, readme.parent)})"
        else:
            ov_link = f"`{comp}` (overview pending)"
        l2_seqs = find_component_l2_sequences(comp)
        l2_links = ", ".join(f"[{s.stem}]({rel2(s, readme.parent)})" for s in l2_seqs[:3])
        if not l2_links:
            l2_links = "(L2 sequences pending)"
        out.append(f"| {ov_link} | {l2_links} |")
    out.append("")
    out.append("> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.")
    out.append("")

    return "\n".join(out)


def gen_uc_nav_block(uc_dir: Path, fid_num: str) -> str:
    """Build the Navigation block for a UC use-case.md."""
    uc_id = "-".join(uc_dir.name.split("-")[:3])
    feature_dir = FEATURES_DIR / next(
        (d.name for d in FEATURES_DIR.iterdir()
         if d.is_dir() and d.name.startswith(f"F-{fid_num}-")),
        ""
    )

    l0 = find_l0_sequence(uc_dir)
    l1 = find_l1_sequence(fid_num, uc_id)

    uc_md = uc_dir / "use-case.md"

    lines = []
    lines.append("## 🧭 Navigation (IN-013)")
    lines.append("")
    lines.append("| Уровень | Где |")
    lines.append("| --- | --- |")
    if feature_dir.exists():
        lines.append(f"| ⬆️ Parent feature L0 ☁️ | [{feature_dir.name}]({rel2(feature_dir, uc_md.parent)}/) |")
    if l0:
        lines.append(f"| ☁️ L0 system sequence | [{l0.stem}]({rel2(l0, uc_md.parent)}) — system как чёрный ящик |")
    if l1:
        lines.append(f"| 🌊 L1 service sequence | [{l1.stem}]({rel2(l1, uc_md.parent)}) — взаимодействие сервисов |")
    lines.append(f"| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |")
    lines.append(f"| 💻 Source code | [`cpp/`]({rel2(ROOT / 'cpp', uc_md.parent)}/) |")
    lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Insert helpers
# ---------------------------------------------------------------------------

NAV_MAP_MARK = "🧭 Navigation Map"
NAV_BLOCK_MARK = "🧭 Navigation (IN-013)"


def insert_after_h1_and_status(content: str, block: str) -> str:
    """Insert block after H1 + optional > status blockquote."""
    lines = content.split("\n")
    insert_idx = 0
    # Find H1
    for i, l in enumerate(lines):
        if l.startswith("# "):
            insert_idx = i + 1
            break
    # Skip blank line(s) after H1
    while insert_idx < len(lines) and lines[insert_idx].strip() == "":
        insert_idx += 1
    # Skip blockquote lines (> ...)
    while insert_idx < len(lines) and lines[insert_idx].startswith(">"):
        insert_idx += 1
    # Skip blank line after blockquote
    while insert_idx < len(lines) and lines[insert_idx].strip() == "":
        insert_idx += 1

    head = lines[:insert_idx]
    tail = lines[insert_idx:]
    return "\n".join(head) + "\n" + block + "\n" + "\n".join(tail)


def insert_uc_nav_after_h1(content: str, block: str) -> str:
    """Insert UC nav block right after H1."""
    lines = content.split("\n")
    insert_idx = 0
    for i, l in enumerate(lines):
        if l.startswith("# "):
            insert_idx = i + 1
            break
    while insert_idx < len(lines) and lines[insert_idx].strip() == "":
        insert_idx += 1
    head = lines[:insert_idx]
    tail = lines[insert_idx:]
    return "\n".join(head) + "\n" + block + "\n" + "\n".join(tail)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    n_features_updated = 0
    n_ucs_updated = 0
    n_skipped = 0

    for fdir in sorted(FEATURES_DIR.glob("F-*/")):
        if fdir.name == "_template":
            continue
        yaml_path = fdir / "feature.yaml"
        readme = fdir / "README.md"
        if not yaml_path.exists() or not readme.exists():
            continue

        # Feature ID number (e.g. F-04 → 04)
        fid_num = fdir.name.split("-")[1]

        # Skip if already has Navigation Map
        readme_content = readme.read_text()
        if NAV_MAP_MARK in readme_content:
            print(f"  SKIP {fdir.name}/README.md (already has nav)")
            n_skipped += 1
        else:
            try:
                feature_yaml = yaml.safe_load(yaml_path.read_text())
            except Exception as e:
                print(f"  ERROR {fdir.name}/feature.yaml: {e}")
                continue
            ucs = discover_ucs_for_feature(fid_num)
            block = gen_feature_nav_map(feature_yaml, ucs, readme)
            new_content = insert_after_h1_and_status(readme_content, block)
            if not args.dry_run:
                readme.write_text(new_content)
            print(f"  + {fdir.name}/README.md ({len(ucs)} UCs, {len(feature_yaml.get('primaryComponents', []))} components)")
            n_features_updated += 1

        # Process UC files
        for uc_id, uc_dir in discover_ucs_for_feature(fid_num):
            uc_md = uc_dir / "use-case.md"
            if not uc_md.exists():
                continue
            uc_content = uc_md.read_text()
            if NAV_BLOCK_MARK in uc_content:
                # already has nav, skip
                continue
            block = gen_uc_nav_block(uc_dir, fid_num)
            new_content = insert_uc_nav_after_h1(uc_content, block)
            if not args.dry_run:
                uc_md.write_text(new_content)
            print(f"    + {uc_dir.name}/use-case.md")
            n_ucs_updated += 1

    print()
    print(f"=== Summary ===")
    print(f"Feature READMEs updated: {n_features_updated}")
    print(f"UC use-case.md updated:  {n_ucs_updated}")
    print(f"Skipped (already done):  {n_skipped}")


if __name__ == "__main__":
    main()
