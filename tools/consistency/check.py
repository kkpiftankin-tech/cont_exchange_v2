#!/usr/bin/env python3
"""tools/consistency/check.py — fitness functions C-01..C-17 (IN-019 §5).

Этап 2 (ADR-086/IN-019), сужен по решению владельца 2026-10-27:

  - Реализованы правила, не требующие артефактов будущих этапов:
    C-01, C-04, C-05 (частично), C-08, C-14, C-15, C-16.
  - Остальные требовали артефактов Этапов 3/4 — возвращаются как
    status=not-yet-applicable с причиной, не удаляются молча из отчёта.
  - C-05 реализован БЕЗ проверки "у implemented есть проходящий пример"
    (исполняемые примеры CALC — Этап 4, §4.5); остальная часть правила
    (шапка, статус из перечня, якоря code/tests существуют) — проверяется.

Этап 4 п.1 (2026-10-27): C-02, C-03, C-06, C-17 реализованы — но реально
проверяют только фичи с заведённым `capabilities[]`. Сейчас это ТОЛЬКО
F-18, и только 3 из 28 AC-F18v2-* сгруппированы (пилот, не весь каталог
возможностей) — у остальных 18 фич и у 25 оставшихся AC F-18 правила
молча ничего не находят (пустой `capabilities[]` = нечего проверять, не
ошибка). C-07/09/10/11/12/13 остаются not-yet-applicable (Этап 3 п.3/
Этап 4 п.2/Этап 6-7 не начаты).

Работает НАД specs/registry/registry.yaml (tools/registry/build.py), не
читает docs напрямую. specs/domain/feature-component-map.yaml и
traceability.yaml остаются источником для tools/feature-yaml-checker и
tools/traceability-checker — решение "обернуть, не заменять" (не Этап 2).

Этап 8 (2026-10-27): `--scope F-XX` фильтрует findings по feature id — для
feature/uc/calc-центричных правил (C-01/02/03/04/05/06/14/15/17) через поле
`feature` узла (или id самой feature-записи); C-08 (таблицы) и C-16
(waivers) всегда глобальные (не привязаны к одной фиче); C-14/C-15 для
ADR-узлов тоже глобальные (ADR не несёт поле feature). Дубликаты ID
(C-02/C-15) детектируются по всем узлам (иначе не поймать дубль между
фичами), но в отчёт попадают только записи, относящиеся к `scope`.

Запуск:
  python3 tools/registry/build.py --write   # сначала обновить реестр
  python3 tools/consistency/check.py [--rule C-01] [--scope F-18] [--format md|json]

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
    "C-07": "требует OpenAPI BFF (contracts/openapi/fob/bff/v1/) — Этап 3 п.3, не начато",
    "C-09": "требует OpenAPI BFF + capability.ui сверку по всем экранам — Этап 3 (пилот 1 экрана есть, не 6)",
    "C-10": "требует OpenAPI BFF — Этап 3 п.3",
    "C-11": "требует CR-трейлеров — Этап 4 п.2, не начато",
    "C-12": "требует verified-дат на поясняющих артефактах — Этап 6/7",
    "C-13": "требует docs/changes/CR-*.md — Этап 4 п.2, не начато",
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


def rule_c01(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """у фичи непустой parentRequirements; ID существуют."""
    findings = []
    req_ids = {n["id"] for n in reg["nodes"]["requirement"]}
    for feat in reg["nodes"]["feature"]:
        fid = feat["id"]
        if scope and fid != scope:
            continue
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


def rule_c04(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """у UC active есть системная и сервисная SEQ (файлы существуют)."""
    findings = []
    for uc in reg["nodes"]["uc"]:
        if scope and uc.get("feature") != scope:
            continue
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


def rule_c05(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """CALC: валидная шапка, статус из перечня, якоря code/tests существуют.
    НЕ проверяет "проходящий пример" — Этап 4."""
    findings = []
    for calc in reg["nodes"]["calc"]:
        if scope and calc.get("feature") != scope:
            continue
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


def rule_c08(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """инвентарь таблиц init.sql = таблицы в docs/07-data/.

    Как у kafka-topic проверки в feature-yaml-checker: таблица считается
    документированной, если есть ЛИБО выделенный файл с её именем, ЛИБО
    она упомянута в обратных кавычках `table_name` в любом docs/07-data/*.md
    (bundled-doc, напр. data-flow.md/data-overview.md/oltp-schema.md
    описывают несколько таблиц в одном файле).

    `scope` игнорируется: таблицы в registry не привязаны к одной фиче
    (table-узлы не несут поле `feature`) — правило всегда глобальное."""
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


def rule_c14(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """статусы только из перечней.

    ADR-узлы не несут поле `feature` (ADR часто не 1:1 с фичей) — цикл по
    ADR всегда глобальный, `scope` на него не действует (документировано)."""
    findings = []
    for feat in reg["nodes"]["feature"]:
        if scope and feat["id"] != scope:
            continue
        lc = feat.get("lifecycle")
        if lc is not None and lc not in FEATURE_LIFECYCLE_ENUM:
            findings.append({
                "rule": "C-14", "severity": "error", "node": feat["id"],
                "message": f"{feat['id']}: lifecycle={lc!r} не из перечня {sorted(FEATURE_LIFECYCLE_ENUM)}",
            })
    for uc in reg["nodes"]["uc"]:
        if scope and uc.get("feature") != scope:
            continue
        st = uc.get("status")
        if st is not None and st not in UC_STATUS_ENUM:
            findings.append({
                "rule": "C-14", "severity": "error", "node": uc["id"],
                "message": f"{uc['id']}: status={st!r} не из перечня {sorted(UC_STATUS_ENUM)}",
            })
    for calc in reg["nodes"]["calc"]:
        if scope and calc.get("feature") != scope:
            continue
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


def rule_c15(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """ID уникальны (UC, CALC — AC/CR/UI пока не отдельные узлы реестра);
    ID в шапке = имени файла (CALC).

    Дублирование ID — всегда глобальная проверка (дубликат по природе
    затрагивает ≥2 узла, сужать до одной фичи некорректно); `scope`
    фильтрует только "id != filename" для CALC, где есть `feature`."""
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
        if scope and calc.get("feature") != scope:
            continue
        if calc["id"] != calc["filenameId"]:
            findings.append({
                "rule": "C-15", "severity": "error", "node": calc["id"],
                "message": f"{calc['path']}: id={calc['id']!r} != имени файла {calc['filenameId']!r}",
            })
    return findings


def rule_c16(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """нет просроченных исключений в specs/waivers.yaml.

    `scope` игнорируется — waiver-реестр не привязан к одной фиче."""
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


def rule_c02(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """у возможности есть AC с ID и хотя бы один UC; ID уникальны.

    Этап 4 п.1 (2026-10-27) — реализуемо только там, где у фичи заведён
    capabilities[] (сейчас только F-18, пилот 3 из 28 AC). У фич без
    capabilities[] правило не применяется (not-yet-applicable для них,
    не error) — пилотная группировка, не ретроактивная проверка всех фич.

    Дубликат cap-id детектируется по ВСЕМ фичам (иначе не поймать дубль
    между фичами), но в найденную запись попадает, только если ТЕКУЩАЯ
    (вторая встреченная) capability принадлежит `scope`."""
    findings = []
    uc_ids = {n["id"] for n in reg["nodes"]["uc"]}
    seen_cap_ids: dict[str, str] = {}
    for feat in reg["nodes"]["feature"]:
        caps = feat.get("capabilities") or []
        if not caps:
            continue
        in_scope = not scope or feat["id"] == scope
        valid_ac_ids = set(feat.get("acceptanceCriteriaIds") or [])
        for cap in caps:
            cid = cap.get("id", "?")
            if cid in seen_cap_ids and in_scope:
                findings.append({
                    "rule": "C-02", "severity": "error", "node": cid,
                    "message": f"{cid}: дублирующийся ID capability ({seen_cap_ids[cid]} и {feat['id']})",
                })
            seen_cap_ids[cid] = feat["id"]
            if not in_scope:
                continue
            acs = cap.get("acceptanceCriteria") or []
            if not acs:
                findings.append({
                    "rule": "C-02", "severity": "error", "node": cid,
                    "message": f"{cid}: acceptanceCriteria пуст",
                })
            for ac in acs:
                if ac not in valid_ac_ids:
                    findings.append({
                        "rule": "C-02", "severity": "error", "node": cid,
                        "message": f"{cid}: AC {ac} не найден в feature.yaml.acceptanceCriteria",
                    })
            ucs = cap.get("ucs") or []
            if not ucs:
                findings.append({
                    "rule": "C-02", "severity": "error", "node": cid,
                    "message": f"{cid}: нет ни одного UC",
                })
            for uc in ucs:
                uc_bare = uc.split("#")[0]
                if uc_bare not in uc_ids:
                    findings.append({
                        "rule": "C-02", "severity": "error", "node": cid,
                        "message": f"{cid}: UC {uc} не найден в registry",
                    })
    return findings


def rule_c03(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """ADR из capabilities[].adrs существуют и ссылаются обратно (WARN)."""
    findings = []
    adr_dir = ROOT / "docs" / "03-architecture" / "adr"
    adr_ids = {n["id"] for n in reg["nodes"]["adr"]}
    adr_text_cache: dict[str, str] = {}
    for feat in reg["nodes"]["feature"]:
        if scope and feat["id"] != scope:
            continue
        for cap in feat.get("capabilities") or []:
            cid = cap.get("id", "?")
            for adr_id in cap.get("adrs") or []:
                if adr_id not in adr_ids:
                    findings.append({
                        "rule": "C-03", "severity": "warn", "node": cid,
                        "message": f"{cid}: ADR {adr_id} из capabilities[].adrs не найден в registry",
                    })
                    continue
                adr_node = next(n for n in reg["nodes"]["adr"] if n["id"] == adr_id)
                adr_path = ROOT / adr_node["path"]
                if adr_id not in adr_text_cache:
                    try:
                        adr_text_cache[adr_id] = adr_path.read_text(encoding="utf-8", errors="replace")
                    except OSError:
                        adr_text_cache[adr_id] = ""
                if cid not in adr_text_cache[adr_id]:
                    findings.append({
                        "rule": "C-03", "severity": "warn", "node": cid,
                        "message": f"{cid}: {adr_id} не ссылается обратно на {cid} "
                                   f"(нет текста {cid!r} в {adr_node['path']})",
                    })
    return findings


def rule_c06(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """нет открытых TODO contract у возможностей с code: done."""
    findings = []
    for feat in reg["nodes"]["feature"]:
        if scope and feat["id"] != scope:
            continue
        for cap in feat.get("capabilities") or []:
            cid = cap.get("id", "?")
            status = cap.get("status") or {}
            if status.get("code") != "done":
                continue
            for contract in cap.get("contracts") or []:
                file_part = contract.split("#")[0]
                target = ROOT / file_part
                if not target.exists():
                    findings.append({
                        "rule": "C-06", "severity": "error", "node": cid,
                        "message": f"{cid}: контракт не существует на диске: {file_part}",
                    })
                    continue
                try:
                    text = target.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                if "TODO contract" in text:
                    findings.append({
                        "rule": "C-06", "severity": "error", "node": cid,
                        "message": f"{cid}: контракт {file_part} помечен TODO contract, "
                                   f"но capability.status.code=done",
                    })
    return findings


def rule_c17(reg: dict, waivers: list[dict], scope: str | None = None) -> list[dict]:
    """у AC возможности с code: done есть тест (warn -> error после этапа 8)."""
    findings = []
    for feat in reg["nodes"]["feature"]:
        if scope and feat["id"] != scope:
            continue
        for cap in feat.get("capabilities") or []:
            cid = cap.get("id", "?")
            status = cap.get("status") or {}
            if status.get("code") != "done":
                continue
            if not cap.get("acceptanceCriteria"):
                continue
            tests = cap.get("tests") or []
            if not tests:
                findings.append({
                    "rule": "C-17", "severity": "warn", "node": cid,
                    "message": f"{cid}: code=done, acceptanceCriteria={cap['acceptanceCriteria']}, "
                               f"но tests пуст",
                })
    return findings


RULES = {
    "C-01": rule_c01,
    "C-02": rule_c02,
    "C-03": rule_c03,
    "C-04": rule_c04,
    "C-05": rule_c05,
    "C-06": rule_c06,
    "C-08": rule_c08,
    "C-14": rule_c14,
    "C-15": rule_c15,
    "C-16": rule_c16,
    "C-17": rule_c17,
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
    ap.add_argument("--scope", help="фильтр по feature id (например F-18), Этап 8. "
                    "C-08/C-16 всегда глобальные (table/waiver не привязаны к фиче); "
                    "C-14/C-15 фильтруют feature/uc/calc, ADR-узлы в них — всегда глобальные "
                    "(ADR не несёт поле feature). См. докстринги правил.")
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

    if args.scope:
        feat_ids = {f["id"] for f in reg["nodes"]["feature"]}
        if args.scope not in feat_ids:
            print(f"ERROR: --scope {args.scope!r} не найден среди feature id в registry", file=sys.stderr)
            return 2

    findings: list[dict] = []
    for rid, fn in rules_to_run.items():
        findings.extend(fn(reg, waivers, args.scope))
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
