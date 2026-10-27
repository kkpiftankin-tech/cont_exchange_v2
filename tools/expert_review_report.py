#!/usr/bin/env python3
"""Сводный отчёт: критичная функциональность и вопросы для эксперта.

    python3 tools/expert_review_report.py              # печатает в stdout
    python3 tools/expert_review_report.py --write       # пишет docs/traceability/expert-review-report.md

Собирает в один отчёт то, что сейчас разбросано по репозиторию и не видно
единым взглядом:

  1. Структурированные OQ-* вопросы (status/addressee) из
     docs/02-system/features/*/open-questions.md — формат из
     docs/00-methodology/artifact-templates.md "Open Questions Template".
  2. Старые нумерованные open-questions.md без OQ-ID/статуса (F-04/F-11/F-12/
     F-15 на момент написания) — считаются и перечисляются отдельно, как
     "нужна структуризация", а не теряются молча.
  3. Карточки вычислений docs/04-domain/calculations/CALC-*.md со
     status != implemented (drift/partial/proposed/planned/calibrated...).
  4. ADR в статусе status: proposed (ещё не принят — то есть ждёт решения).
  5. "Конкретные долги" из docs/00-methodology/known-gaps.md.

Это best-effort текстовый парсинг, не полноценный YAML/Markdown AST — при
неожиданном формате файл просто попадёт в соответствующую "unparsed"
категорию, а не уронит скрипт. Источник истины всегда — сами файлы; это
отчёт для ориентации, не для автоматических решений.

Почему это должно быть "автоматическим", а не ручным: до этого скрипта
критичные открытые вопросы оставались в чате ревью/ответах агентов и не
попадали в docs вовсе (см. incoming-docs по эпизоду 2026-10-27, money-path
guard + эта же дыра для открытых вопросов). .claude/rules/agent-routing.md
§3 теперь требует, чтобы такие находки от read-only агентов
(code-reviewer/trading-domain-specialist/security-reviewer/
solution-architect) записывались в open-questions.md/known-gaps.md главной
сессией, НЕ оставались только в ответе чата — этот отчёт после этого их
собирает в одно место для владельца/эксперта.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FEATURES_DIR = ROOT / "docs" / "02-system" / "features"
CALC_DIR = ROOT / "docs" / "04-domain" / "calculations"
ADR_DIR = ROOT / "docs" / "03-architecture" / "adr"
KNOWN_GAPS = ROOT / "docs" / "00-methodology" / "known-gaps.md"

OQ_HEADING = re.compile(r"^###\s+(OQ-[A-Za-z0-9_-]+)\s*(?:—|-)?\s*(.*)$", re.MULTILINE)
STATUS_FIELD = re.compile(r"\*\*Статус:?\*\*\s*`?([A-Za-zА-Яа-яёЁ]+)`?", re.IGNORECASE)
ADDRESSEE_FIELD = re.compile(r"\*\*Адресат:?\*\*\s*([^\n]+)")


def frontmatter(text: str) -> dict:
    """Плоский парсер YAML-подобного frontmatter между первыми '---'."""
    m = re.match(r"^---\n(.*?)\n---\n", text, re.DOTALL)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        km = re.match(r"^([A-Za-z_]+):\s*(.*)$", line)
        if km:
            key, val = km.group(1), km.group(2).strip()
            val = val.split(" #", 1)[0].strip()  # отрезать inline YAML-комментарий
            if val:
                out[key] = val
    return out


def feature_id_from_dir(d: pathlib.Path) -> str:
    m = re.match(r"(F-\d+)", d.name)
    return m.group(1) if m else d.name


def scan_open_questions():
    structured, unstructured = [], []
    if not FEATURES_DIR.exists():
        return structured, unstructured
    for d in sorted(FEATURES_DIR.iterdir()):
        f = d / "open-questions.md"
        if not f.is_file():
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        fid = feature_id_from_dir(d)
        rel = f.relative_to(ROOT)
        headings = list(OQ_HEADING.finditer(text))
        if not headings:
            body_lines = [l for l in text.splitlines() if l.strip() and not l.startswith("#")]
            if body_lines:
                unstructured.append((fid, rel, len(body_lines)))
            continue
        for i, hm in enumerate(headings):
            start = hm.end()
            end = headings[i + 1].start() if i + 1 < len(headings) else len(text)
            block = text[start:end]
            sm = STATUS_FIELD.search(block)
            am = ADDRESSEE_FIELD.search(block)
            structured.append({
                "feature": fid,
                "id": hm.group(1),
                "title": hm.group(2).strip(),
                "status": (sm.group(1).lower() if sm else "unknown"),
                "addressee": (am.group(1).strip().rstrip(".") if am else "—"),
                "path": rel,
            })
    return structured, unstructured


def scan_calc_cards():
    out = []
    if not CALC_DIR.exists():
        return out
    for f in sorted(CALC_DIR.glob("CALC-*.md")):
        if f.name == "CALC-TEMPLATE.md":
            continue
        try:
            fm = frontmatter(f.read_text(encoding="utf-8", errors="replace"))
        except Exception:
            continue
        status = fm.get("status", "unknown")
        if status != "implemented":
            out.append({
                "id": fm.get("id", f.stem),
                "title": fm.get("title", f.stem),
                "feature": fm.get("feature", "—"),
                "status": status,
                "path": f.relative_to(ROOT),
            })
    return out


def scan_proposed_adrs():
    out = []
    if not ADR_DIR.exists():
        return out
    for f in sorted(ADR_DIR.glob("ADR-*.md")):
        try:
            fm = frontmatter(f.read_text(encoding="utf-8", errors="replace"))
        except Exception:
            continue
        if fm.get("status") == "proposed":
            out.append({
                "id": fm.get("id", f.stem),
                "title": fm.get("title", f.stem),
                "feature": fm.get("feature", "—"),
                "path": f.relative_to(ROOT),
            })
    return out


def scan_known_gaps_debts():
    if not KNOWN_GAPS.exists():
        return []
    text = KNOWN_GAPS.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"## Конкретные долги.*?\n(.*?)(?=\n## |\Z)", text, re.DOTALL)
    if not m:
        return []
    # Bold title may itself wrap across lines in the source; DOTALL lets
    # the non-greedy .+? cross newlines up to the nearest closing **.
    titles = re.findall(r"^- \*\*(.+?)\*\*", m.group(1), re.MULTILINE | re.DOTALL)
    return [re.sub(r"\s+", " ", t).strip() for t in titles]


def render(structured, unstructured, calc, adrs, debts) -> str:
    blocking = [o for o in structured if o["status"] == "blocking"]
    open_ = [o for o in structured if o["status"] == "open"]
    lines = []
    lines.append("# Отчёт: критичная функциональность и вопросы для эксперта")
    lines.append("")
    lines.append("Сгенерировано `tools/expert_review_report.py`. Источник истины — файлы,")
    lines.append("на которые ведут ссылки ниже; этот отчёт — только навигация по текущему")
    lines.append("состоянию, не редактируется руками. Перегенерировать после любой правки")
    lines.append("`open-questions.md`, статуса CALC-карточки, ADR или `known-gaps.md`.")
    lines.append("")
    lines.append("## Сводка")
    lines.append("")
    lines.append(f"- **Blocking** вопросов (нельзя продолжать фичу): **{len(blocking)}**")
    lines.append(f"- Открытых структурированных вопросов (`OQ-*`): **{len(open_)}**")
    lines.append(f"- Фич со старым списком вопросов без структуры (мигрировать по касанию): "
                  f"**{len(unstructured)}** ({sum(n for _, _, n in unstructured)} пунктов суммарно)")
    lines.append(f"- Карточек вычислений не в статусе `implemented`: **{len(calc)}**")
    lines.append(f"- ADR в статусе `proposed` (ждут решения владельца): **{len(adrs)}**")
    lines.append(f"- Инженерных долгов в known-gaps.md: **{len(debts)}**")
    lines.append("")

    if blocking:
        lines.append("## Blocking — нельзя продолжать фичу, пока не снято")
        lines.append("")
        for o in blocking:
            lines.append(f"- **{o['id']}** ({o['feature']}) — {o['title']} · адресат: "
                          f"{o['addressee']} · [{o['path']}]({o['path']})")
        lines.append("")

    if open_:
        lines.append("## Открытые вопросы для эксперта (по адресату)")
        lines.append("")
        by_addr = {}
        for o in open_:
            by_addr.setdefault(o["addressee"], []).append(o)
        for addr in sorted(by_addr):
            lines.append(f"### {addr}")
            lines.append("")
            for o in by_addr[addr]:
                lines.append(f"- **{o['id']}** ({o['feature']}) — {o['title']} · "
                              f"[{o['path']}]({o['path']})")
            lines.append("")

    if unstructured:
        lines.append("## Старые списки вопросов без OQ-ID/статуса (нужна структуризация)")
        lines.append("")
        lines.append("Формат — `docs/00-methodology/artifact-templates.md` "
                      "\"Open Questions Template\"; мигрировать по касанию фичи, не разом.")
        lines.append("")
        for fid, path, n in unstructured:
            lines.append(f"- **{fid}** — {n} вопрос(ов) без структуры · [{path}]({path})")
        lines.append("")

    if calc:
        lines.append("## Карточки вычислений не в статусе `implemented`")
        lines.append("")
        for c in calc:
            lines.append(f"- **{c['id']}** ({c['feature']}, `{c['status']}`) — {c['title']} · "
                          f"[{c['path']}]({c['path']})")
        lines.append("")

    if adrs:
        lines.append("## ADR в статусе `proposed` (ждут решения владельца)")
        lines.append("")
        for a in adrs:
            lines.append(f"- **{a['id']}** ({a['feature']}) — {a['title']} · [{a['path']}]({a['path']})")
        lines.append("")

    if debts:
        lines.append("## Инженерные долги (docs/00-methodology/known-gaps.md)")
        lines.append("")
        for d in debts:
            lines.append(f"- {d}")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                     help="записать в docs/traceability/expert-review-report.md вместо stdout")
    a = ap.parse_args()

    structured, unstructured = scan_open_questions()
    calc = scan_calc_cards()
    adrs = scan_proposed_adrs()
    debts = scan_known_gaps_debts()
    report = render(structured, unstructured, calc, adrs, debts)

    if a.write:
        out = ROOT / "docs" / "traceability" / "expert-review-report.md"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(report, encoding="utf-8")
        print(f"written: {out.relative_to(ROOT)}")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
