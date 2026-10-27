#!/usr/bin/env python3
"""tools/consistency/check.py — fitness functions C-01..C-17 (IN-019 §5).

Этап 2 (ADR-086/IN-019), сужен по решению владельца 2026-10-27:

  - Реализованы правила, не требующие артефактов будущих этапов:
    C-01, C-04, C-05 (частично), C-08, C-14, C-15, C-16.
  - Остальные (C-02, C-03, C-06, C-07, C-09, C-10, C-11, C-12, C-13, C-17)
    требуют артефактов Этапов 3/4 (capabilities в feature.yaml, ui-map.yaml,
    OpenAPI BFF, CR-карточки, исполняемые примеры CALC, verified-даты) —
    возвращаются как status=not-yet-applicable с причиной, не удаляются
    молча из отчёта.
  - C-05 реализован БЕЗ проверки "у implemented есть проходящий пример"
    (исполняемые примеры CALC — Этап 4, §4.5); остальная часть правила
    (шапка, статус из перечня, якоря code/tests существуют) — проверяется.

Работает НАД specs/registry/registry.yaml (tools/registry/build.py), не
читает docs напрямую. specs/domain/feature-component-map.yaml и
traceability.yaml остаются источником для tools/feature-yaml-checker и
tools/traceability-checker — решение "обернуть, не заменять" (не Этап 2).

Запуск:
  python3 tools/registry/build.py --write   # сначала обновить реестр
  python3 tools/consistency/check.py [--rule C-01] [--format md|json]

Код выхода: 1 при ERROR без waiver, 0 иначе (WARN не валит exit code).
"""
from __future__ import annotations

import argparse
import datetime
import json
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parent.parent.parent
REGISTRY_PATH = ROOT / "specs" / "registry" / "registry.yaml"
WAIVERS_PATH = ROOT / "specs" / "waivers.yaml"
TABLE_DOCS_DIR = ROOT / "docs" / "07-data"

CALC_STATUS_ENUM = {"implemented", "partial", "planned", "drift", "proposed", "calibrated"}
UC_STATUS_ENUM = {"draft", "active", "superseded", "deprecated"}
FEATURE_LIFECYCLE_ENUM = {"planned", "active", "deprecated"}
ADR_STATUS_ENUM = {"proposed", "accepted", "superseded", "deprecated", "rejected"}

NOT_YET_APPLICABLE = {
    "C-02": "требует capabilities[] в feature.yaml — Этап 4 (IN-019 §4.4)",
    "C-03": "требует capabilities[].adrs — Этап 4",
    "C-06": "требует capabilities[].status.code — Этап 4",
    "C-07": "требует docs/frontend/ui-map.yaml и OpenAPI BFF — Этап 3",
    "C-09": "требует docs/frontend/ui-map.yaml — Этап 3",
    "C-10": "требует docs/frontend/ui-map.yaml — Этап 3",
    "C-11": "требует CR-трейлеров / capabilities[].codePaths — Этап 4",
    "C-12": "требует verified-дат на поясняющих артефактах — Этап 6/7",
    "C-13": "требует docs/changes/CR-*.md — Этап 4",
    "C-17": "требует capabilities[].acceptanceCriteria + tests — Этап 4",
}


def load_registry() -> dict:
    if not REGISTRY_PATH.exists():
        print(f"ERROR: {REGISTRY_PATH.relative_to(ROOT)} не найден — запусти "
              f"tools/registry/build.py --write", file=sys.stderr)
        sys.exit(2)
    return yaml.safe_load(REGISTRY_PATH.read_text(encoding="utf-8")) or {}


def load_waivers() -> list[dict]:
    if not WAIVERS_PATH.exists():
        return []
    return yaml.safe_load(WAIVERS_PATH.read_text(encoding="utf-8")) or []


def active_waiver(waivers: list[dict], rule: str, scope: str = "") -> dict | None:
    today = datetime.date.today()
    for w in waivers:
        if w.get("rule") != rule:
            continue
        if w.get("scope") and scope and w["scope"] not in scope and scope not in w["scope"]:
            continue
        expires = w.get("expires")
        if expires:
            try:
                exp_date = datetime.date.fromisoformat(str(expires))
                if exp_date < today:
                    continue  # просрочен — не действует (ловит C-16)
            except ValueError:
                continue
        if w.get("owner") and w.get("expires"):
            return w
    return None


def rule_c01(reg: dict, waivers: list[dict]) -> list[dict]:
    """у фичи непустой parentRequirements; ID существуют."""
    findings = []
    req_ids = {n["id"] for n in reg["nodes"]["requirement"]}
    for feat in reg["nodes"]["feature"]:
        fid = feat["id"]
        reqs = feat.get("parentRequirements") or []
        if not reqs:
            findings.append({
                "rule": "C-01", "severity": "warn", "node": fid,
                "message": f"{fid}: parentRequirements пуст (warn до Этапа 8)",
            })
            continue
        for rid in reqs:
            if rid not in req_ids:
                findings.append({
                    "rule": "C-01", "severity": "error", "node": fid,
                    "message": f"{fid}: parentRequirements ссылается на несуществующий {rid}",
                })
    return findings


def rule_c04(reg: dict, waivers: list[dict]) -> list[dict]:
    """у UC active есть системная и сервисная SEQ (файлы существуют)."""
    findings = []
    for uc in reg["nodes"]["uc"]:
        if uc.get("status") != "active":
            continue
        uc_dir = ROOT / uc["ucDir"]
        for field, label in (("systemSequence", "system-sequence"), ("serviceSequence", "service-sequence")):
            val = uc.get(field)
            if not val:
                findings.append({
                    "rule": "C-04", "severity": "error", "node": uc["id"],
                    "message": f"{uc['id']} (active): нет поля {label} в frontmatter",
                })
                continue
            target = (uc_dir / val).resolve()
            if not target.exists():
                findings.append({
                    "rule": "C-04", "severity": "error", "node": uc["id"],
                    "message": f"{uc['id']} (active): {label}={val} не существует",
                })
    return findings


def rule_c05(reg: dict, waivers: list[dict]) -> list[dict]:
    """CALC: валидная шапка, статус из перечня, якоря code/tests существуют.
    НЕ проверяет "проходящий пример" — Этап 4."""
    findings = []
    for calc in reg["nodes"]["calc"]:
        cid = calc["id"]
        status = calc.get("status")
        if status not in CALC_STATUS_ENUM:
            findings.append({
                "rule": "C-05", "severity": "error", "node": cid,
                "message": f"{cid}: status={status!r} не из перечня {sorted(CALC_STATUS_ENUM)}",
            })
        for kind in ("code", "tests"):
            for anchor in calc.get(kind) or []:
                if not isinstance(anchor, str):
                    findings.append({
                        "rule": "C-05", "severity": "error", "node": cid,
                        "message": f"{cid}: {kind}-якорь не строка (сломанный YAML — "
                                   f"неэкранированное ':' внутри элемента списка): {anchor!r}",
                    })
                    continue
                # "path#anchor (комментарий)" / "path (комментарий)" — путь это
                # первый пробельно-отделённый токен, до "#" если есть.
                file_part = anchor.split()[0].split("#")[0] if anchor.split() else ""
                if not file_part:
                    continue
                if not (ROOT / file_part).exists():
                    findings.append({
                        "rule": "C-05", "severity": "error", "node": cid,
                        "message": f"{cid}: {kind}-якорь не существует на диске: {file_part}",
                    })
    return findings


def rule_c08(reg: dict, waivers: list[dict]) -> list[dict]:
    """инвентарь таблиц init.sql = таблицы в docs/07-data/.

    Как у kafka-topic проверки в feature-yaml-checker: таблица считается
    документированной, если есть ЛИБО выделенный файл с её именем, ЛИБО
    она упомянута в обратных кавычках `table_name` в любом docs/07-data/*.md
    (bundled-doc, напр. data-flow.md/data-overview.md/oltp-schema.md
    описывают несколько таблиц в одном файле)."""
    findings = []
    dedicated = set()
    bundled_text = ""
    if TABLE_DOCS_DIR.exists():
        for f in TABLE_DOCS_DIR.glob("*.md"):
            dedicated.add(f.stem.replace("-", "_"))
            try:
                bundled_text += f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                pass
    for t in reg["nodes"]["table"]:
        name = t["tableName"]
        has_dedicated = name in dedicated or name.replace("_", "-") in {d.replace("_", "-") for d in dedicated}
        has_mention = re.search(rf"[`#\s]{re.escape(name)}[`\s(:]", bundled_text) is not None
        if not has_dedicated and not has_mention:
            findings.append({
                "rule": "C-08", "severity": "error", "node": t["id"],
                "message": f"{t['id']}: таблица {name} ({t['store']}) не документирована "
                           f"в docs/07-data/ (ни выделенного файла, ни упоминания `{name}`)",
            })
    return findings


def rule_c14(reg: dict, waivers: list[dict]) -> list[dict]:
    """статусы только из перечней."""
    findings = []
    for feat in reg["nodes"]["feature"]:
        lc = feat.get("lifecycle")
        if lc is not None and lc not in FEATURE_LIFECYCLE_ENUM:
            findings.append({
                "rule": "C-14", "severity": "error", "node": feat["id"],
                "message": f"{feat['id']}: lifecycle={lc!r} не из перечня {sorted(FEATURE_LIFECYCLE_ENUM)}",
            })
    for uc in reg["nodes"]["uc"]:
        st = uc.get("status")
        if st is not None and st not in UC_STATUS_ENUM:
            findings.append({
                "rule": "C-14", "severity": "error", "node": uc["id"],
                "message": f"{uc['id']}: status={st!r} не из перечня {sorted(UC_STATUS_ENUM)}",
            })
    for calc in reg["nodes"]["calc"]:
        st = calc.get("status")
        if st is not None and st not in CALC_STATUS_ENUM:
            findings.append({
                "rule": "C-14", "severity": "error", "node": calc["id"],
                "message": f"{calc['id']}: status={st!r} не из перечня {sorted(CALC_STATUS_ENUM)}",
            })
    for adr in reg["nodes"]["adr"]:
        st = adr.get("status")
        if st is not None and st not in ADR_STATUS_ENUM:
            findings.append({
                "rule": "C-14", "severity": "error", "node": adr["id"],
                "message": f"{adr['id']}: status={st!r} не из перечня {sorted(ADR_STATUS_ENUM)}",
            })
    return findings


def rule_c15(reg: dict, waivers: list[dict]) -> list[dict]:
    """ID уникальны (UC, CALC — AC/CR/UI пока не отдельные узлы реестра);
    ID в шапке = имени файла (CALC)."""
    findings = []
    for node_type in ("uc", "calc", "feature", "adr"):
        seen: dict[str, str] = {}
        for n in reg["nodes"][node_type]:
            nid = n["id"]
            if nid in seen:
                findings.append({
                    "rule": "C-15", "severity": "error", "node": nid,
                    "message": f"{nid}: дублирующийся ID ({node_type}) — {seen[nid]} и {n['path']}",
                })
            seen[nid] = n["path"]
    for calc in reg["nodes"]["calc"]:
        if calc["id"] != calc["filenameId"]:
            findings.append({
                "rule": "C-15", "severity": "error", "node": calc["id"],
                "message": f"{calc['path']}: id={calc['id']!r} != имени файла {calc['filenameId']!r}",
            })
    return findings


def rule_c16(reg: dict, waivers: list[dict]) -> list[dict]:
    """нет просроченных исключений в specs/waivers.yaml."""
    findings = []
    today = datetime.date.today()
    for w in waivers:
        expires = w.get("expires")
        if not expires:
            findings.append({
                "rule": "C-16", "severity": "error", "node": w.get("rule", "?"),
                "message": f"waiver без expires: {w}",
            })
            continue
        try:
            exp_date = datetime.date.fromisoformat(str(expires))
        except ValueError:
            findings.append({
                "rule": "C-16", "severity": "error", "node": w.get("rule", "?"),
                "message": f"waiver с невалидным expires: {w}",
            })
            continue
        if exp_date < today:
            findings.append({
                "rule": "C-16", "severity": "error", "node": w.get("rule", "?"),
                "message": f"просроченный waiver (expires={expires}): {w}",
            })
        if not w.get("owner"):
            findings.append({
                "rule": "C-16", "severity": "error", "node": w.get("rule", "?"),
                "message": f"waiver без owner: {w}",
            })
    return findings


RULES = {
    "C-01": rule_c01,
    "C-04": rule_c04,
    "C-05": rule_c05,
    "C-08": rule_c08,
    "C-14": rule_c14,
    "C-15": rule_c15,
    "C-16": rule_c16,
}


def apply_waivers(findings: list[dict], waivers: list[dict]) -> list[dict]:
    out = []
    for f in findings:
        if f["severity"] == "error":
            w = active_waiver(waivers, f["rule"], f.get("node", ""))
            if w:
                f = dict(f, severity="waived", waiver=w)
        out.append(f)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rule", help="запустить только одно правило, например C-05")
    ap.add_argument("--format", choices=["md", "json"], default="md")
    ap.add_argument("--levels", help="(зарезервировано, не используется в сужённом Этапе 2)")
    ap.add_argument("--scope", help="(зарезервировано, не используется в сужённом Этапе 2)")
    args = ap.parse_args()

    reg = load_registry()
    waivers = load_waivers()

    rules_to_run = {args.rule: RULES[args.rule]} if args.rule else RULES
    if args.rule and args.rule not in RULES:
        if args.rule in NOT_YET_APPLICABLE:
            print(f"{args.rule}: not-yet-applicable — {NOT_YET_APPLICABLE[args.rule]}")
            return 0
        print(f"ERROR: unknown rule {args.rule}", file=sys.stderr)
        return 2

    findings: list[dict] = []
    for rid, fn in rules_to_run.items():
        findings.extend(fn(reg, waivers))
    findings = apply_waivers(findings, waivers)

    errors = [f for f in findings if f["severity"] == "error"]
    warns = [f for f in findings if f["severity"] == "warn"]
    waived = [f for f in findings if f["severity"] == "waived"]

    if args.format == "json":
        print(json.dumps({
            "findings": findings,
            "not_yet_applicable": NOT_YET_APPLICABLE if not args.rule else {},
            "summary": {"errors": len(errors), "warns": len(warns), "waived": len(waived)},
        }, ensure_ascii=False, indent=2))
    else:
        print("# tools/consistency/check.py — отчёт\n")
        if errors:
            print(f"## ERROR ({len(errors)})\n")
            for f in errors:
                print(f"- **{f['rule']}** {f['message']}")
            print()
        if warns:
            print(f"## WARN ({len(warns)})\n")
            for f in warns:
                print(f"- **{f['rule']}** {f['message']}")
            print()
        if waived:
            print(f"## WAIVED ({len(waived)})\n")
            for f in waived:
                print(f"- **{f['rule']}** {f['message']} (waiver: {f['waiver'].get('reason')}, "
                      f"owner={f['waiver'].get('owner')}, expires={f['waiver'].get('expires')})")
            print()
        if not args.rule:
            print(f"## not-yet-applicable ({len(NOT_YET_APPLICABLE)})\n")
            for rid, reason in NOT_YET_APPLICABLE.items():
                print(f"- **{rid}**: {reason}")
            print()
        print(f"Итого: {len(errors)} error, {len(warns)} warn, {len(waived)} waived"
              + ("" if args.rule else f", {len(NOT_YET_APPLICABLE)} not-yet-applicable"))

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
