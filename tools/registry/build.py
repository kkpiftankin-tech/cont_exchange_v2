#!/usr/bin/env python3
"""Строит specs/registry/registry.yaml — реестр узлов/рёбер docs-as-code.

Этап 2 (ADR-086/IN-019 §4.11), сужен по решению владельца 2026-10-27:
НЕ заменяет specs/domain/feature-component-map.yaml и traceability.yaml —
работает параллельно ("обернуть, не удалять сразу"), пока сам не покажет
паритет минимум 1 полный цикл.

Покрывает только узлы, нужные реализуемым сейчас правилам согласованности
(C-01, C-04, C-05, C-08, C-14, C-15 — см. tools/consistency/check.py):

  requirement  — docs/02-system/functional-requirements.md (FR-*)
  feature      — docs/02-system/features/*/feature.yaml
  uc           — docs/02-system/use-cases/*/use-case.md (frontmatter)
  calc         — docs/04-domain/calculations/CALC-*.md (frontmatter, кроме TEMPLATE)
  adr          — docs/03-architecture/adr/ADR-*.md (frontmatter)
  table        — infra/postgres/init.sql + infra/clickhouse/init.sql (CREATE TABLE)

НЕ покрывает (нет исходных артефактов на момент Этапа 2, см. known-gaps.md):
  capability, ac (как отдельный узел), seq, contract, openapi-path,
  ui-screen, ui-element, code, test, cr, exp, waiver — появятся по мере
  того, как Этапы 3/4 создадут сами артефакты.

Запуск:
  python3 tools/registry/build.py --write   # пишет specs/registry/registry.yaml
  python3 tools/registry/build.py           # печатает в stdout

Приёмка (IN-019 §Этап 2): повторный запуск без изменений исходников не
меняет registry.yaml (git diff --exit-code).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parent.parent.parent
FR_DOC = ROOT / "docs" / "02-system" / "functional-requirements.md"
FEATURES_DIR = ROOT / "docs" / "02-system" / "features"
UC_DIR = ROOT / "docs" / "02-system" / "use-cases"
CALC_DIR = ROOT / "docs" / "04-domain" / "calculations"
ADR_DIR = ROOT / "docs" / "03-architecture" / "adr"
PG_INIT = ROOT / "infra" / "postgres" / "init.sql"
CH_INIT = ROOT / "infra" / "clickhouse" / "init.sql"
OUT_PATH = ROOT / "specs" / "registry" / "registry.yaml"

FR_HEADING_RE = re.compile(r"^###\s+(FR-[A-Z0-9]+-\d+)\.\s*(.+)$", re.MULTILINE)
FR_BULLET_RE = re.compile(r"^-\s+\*\*(FR-[A-Z0-9]+-\d+)\.\*\*\s*(.+)$", re.MULTILINE)
ACCEPTANCE_RE = re.compile(r"Acceptance:\s*([^.\n]+)")
CREATE_TABLE_RE = re.compile(
    r"CREATE TABLE(?:\s+IF NOT EXISTS)?\s+(?:IF NOT EXISTS\s+)?`?\"?(\w+)`?\"?",
    re.IGNORECASE,
)


def load_frontmatter(path: Path) -> dict:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return {}
    m = re.match(r"^---\n(.*?)\n---\n", text, re.DOTALL)
    if not m:
        return {}
    try:
        return yaml.safe_load(m.group(1)) or {}
    except yaml.YAMLError:
        return {}


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def scan_requirements() -> list[dict]:
    nodes = []
    if not FR_DOC.exists():
        return nodes
    text = FR_DOC.read_text(encoding="utf-8")
    for m in FR_HEADING_RE.finditer(text):
        fid, title = m.group(1), m.group(2).strip()
        tail = text[m.end():m.end() + 400]
        acc = ACCEPTANCE_RE.search(tail)
        features = []
        if acc:
            features = [f.strip().rstrip(".") for f in re.findall(r"F-[A-Za-z0-9]+", acc.group(1))]
        nodes.append({
            "id": fid, "type": "requirement", "class": "normative",
            "title": title, "path": rel(FR_DOC), "features": features,
        })
    for m in FR_BULLET_RE.finditer(text):
        fid, title = m.group(1), m.group(2).strip()
        nodes.append({
            "id": fid, "type": "requirement", "class": "normative",
            "title": title[:120], "path": rel(FR_DOC), "features": ["F-05A"],
        })
    return nodes


def scan_features() -> list[dict]:
    nodes = []
    if not FEATURES_DIR.exists():
        return nodes
    for d in sorted(FEATURES_DIR.iterdir()):
        if not d.is_dir() or d.name == "_template":
            continue
        fy = d / "feature.yaml"
        if not fy.exists():
            continue
        try:
            data = yaml.safe_load(fy.read_text(encoding="utf-8")) or {}
        except yaml.YAMLError:
            continue
        f = data.get("feature") or {}
        fid = f.get("id")
        if not fid:
            continue
        parent_reqs = data.get("parentRequirements")
        if not isinstance(parent_reqs, list):
            parent_reqs = []
        nodes.append({
            "id": fid, "type": "feature", "class": "normative",
            "title": f.get("name", ""), "path": rel(fy),
            "lifecycle": f.get("lifecycle"), "status": f.get("status"),
            "parentRequirements": parent_reqs,
        })
    return nodes


def scan_ucs() -> list[dict]:
    nodes = []
    if not UC_DIR.exists():
        return nodes
    for d in sorted(UC_DIR.iterdir()):
        if not d.is_dir() or d.name == "_template":
            continue
        uc_file = d / "use-case.md"
        if not uc_file.exists():
            continue
        fm = load_frontmatter(uc_file)
        uid = fm.get("id")
        if not uid:
            continue
        nodes.append({
            "id": uid, "type": "uc", "class": "normative",
            "title": fm.get("title", ""), "path": rel(uc_file),
            "feature": fm.get("feature"), "status": fm.get("status"),
            "systemSequence": fm.get("system-sequence"),
            "serviceSequence": fm.get("service-sequence"),
            "ucDir": rel(d),
        })
    return nodes


def scan_calcs() -> list[dict]:
    nodes = []
    if not CALC_DIR.exists():
        return nodes
    for f in sorted(CALC_DIR.glob("CALC-*.md")):
        if f.name == "CALC-TEMPLATE.md":
            continue
        fm = load_frontmatter(f)
        cid = fm.get("id")
        if not cid:
            continue
        code = fm.get("code") or []
        tests = fm.get("tests") or []
        nodes.append({
            "id": cid, "type": "calc", "class": "normative",
            "title": fm.get("title", ""), "path": rel(f),
            "filenameId": f.stem, "feature": fm.get("feature"),
            "status": fm.get("status"), "level": fm.get("level"),
            "code": code if isinstance(code, list) else [code],
            "tests": tests if isinstance(tests, list) else [tests],
        })
    return nodes


def scan_adrs() -> list[dict]:
    nodes = []
    if not ADR_DIR.exists():
        return nodes
    for f in sorted(ADR_DIR.glob("ADR-*.md")):
        fm = load_frontmatter(f)
        aid = fm.get("id")
        if not aid:
            m = re.match(r"(ADR-\d+)", f.stem)
            aid = m.group(1) if m else None
        if not aid:
            continue
        nodes.append({
            "id": aid, "type": "adr", "class": "normative",
            "title": fm.get("title", f.stem), "path": rel(f),
            "status": fm.get("status"),
        })
    return nodes


def scan_tables() -> list[dict]:
    nodes = []
    for sql_path, store in ((PG_INIT, "postgresql"), (CH_INIT, "clickhouse")):
        if not sql_path.exists():
            continue
        lines = [ln for ln in sql_path.read_text(encoding="utf-8").splitlines()
                 if not ln.lstrip().startswith("--")]
        text = "\n".join(lines)
        for m in CREATE_TABLE_RE.finditer(text):
            name = m.group(1)
            nodes.append({
                "id": f"{store}:{name}", "type": "table", "class": "normative",
                "title": name, "path": rel(sql_path), "store": store,
                "tableName": name,
            })
    return nodes


def build() -> dict:
    nodes = {
        "requirement": scan_requirements(),
        "feature": scan_features(),
        "uc": scan_ucs(),
        "calc": scan_calcs(),
        "adr": scan_adrs(),
        "table": scan_tables(),
    }
    edges = []
    req_ids = {n["id"] for n in nodes["requirement"]}
    for feat in nodes["feature"]:
        for rid in feat["parentRequirements"]:
            edges.append({"from": rid, "to": feat["id"], "type": "requirement->feature",
                          "resolved": rid in req_ids})
    feat_ids = {n["id"] for n in nodes["feature"]}
    for uc in nodes["uc"]:
        if uc.get("feature"):
            edges.append({"from": uc["feature"], "to": uc["id"], "type": "feature->uc",
                          "resolved": uc["feature"] in feat_ids})
    for calc in nodes["calc"]:
        if calc.get("feature"):
            edges.append({"from": calc["feature"], "to": calc["id"], "type": "feature->calc",
                          "resolved": calc["feature"] in feat_ids})
    return {
        "generated_by": "tools/registry/build.py",
        "note": "Сгенерировано, не редактировать руками. Этап 2 (ADR-086/IN-019), сужен — см. docstring build.py.",
        "nodes": nodes,
        "edges": edges,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    data = build()
    text = yaml.safe_dump(data, allow_unicode=True, sort_keys=False, default_flow_style=False)
    if args.write:
        OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
        OUT_PATH.write_text(text, encoding="utf-8")
        print(f"written: {OUT_PATH.relative_to(ROOT)}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
