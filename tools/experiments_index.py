#!/usr/bin/env python3
"""Автообновление docs/experiments/README.md: таблица прогонов + открытые
вопросы для эксперта-трейдера, с переходом в конкретный EXP-файл.

    python3 tools/experiments_index.py            # печатает сгенерированные блоки в stdout
    python3 tools/experiments_index.py --write     # обновляет README.md на месте

Зачем это отдельный скрипт, а не ручная правка README.md: цель — чтобы
эксперт открывал один файл и сразу видел (а) список прогонов и о чём каждый,
(б) какие вопросы ждут его решения прямо сейчас, с одним переходом в нужный
EXP-файл за деталями. Вручную эта таблица быстро разойдётся с реальностью
(как разошлась структура open-questions.md до правила от 2026-10-27) —
поэтому она генерируется из тех же файлов, что и
`tools/expert_review_report.py`, а не пишется параллельно.

Источники:
  - `docs/experiments/EXP-*.md` — сам список прогонов, тема (первая фраза
    "О прогоне"), есть ли у прогона открытые вопросы.
  - `docs/02-system/features/*/open-questions.md` — записи `OQ-*`, у которых
    поле "Источник" ссылается на `docs/experiments/EXP-*.md` и статус не
    `resolved` — это и есть "основные вопросы к эксперту" на главной странице.

README.md редактируется только между маркерами
`<!-- AUTO:...:START -->` / `<!-- AUTO:...:END -->` — остальной текст
(правило языка, словарь, конвенции) не трогается.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXP_DIR = ROOT / "docs" / "experiments"
FEATURES_DIR = ROOT / "docs" / "02-system" / "features"
README = EXP_DIR / "README.md"

OQ_HEADING = re.compile(r"^###\s+(OQ-[A-Za-z0-9_-]+)\s*(?:—|-)?\s*(.*)$", re.MULTILINE)
STATUS_FIELD = re.compile(r"\*\*Статус:?\*\*\s*`?([^`\n(]+)")
SOURCE_FIELD = re.compile(r"\*\*Источник[аи]?:?\*\*\s*(.+)")
LINK = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def first_sentence(text: str, limit: int = 160) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    m = re.search(r"^(.*?[.!?])(?:\s|$)", text)
    s = m.group(1) if m else text
    if len(s) > limit:
        s = s[:limit].rstrip() + "…"
    return s


def scan_experiments():
    rows = []
    if not EXP_DIR.exists():
        return rows
    for f in sorted(EXP_DIR.glob("EXP-*.md")):
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        m = re.match(r"EXP-(\d{4}-\d{2}-\d{2})", f.stem)
        date = m.group(1) if m else "?"

        heading_m = re.search(r"^#\s+(.+)$", text, re.MULTILINE)
        heading = heading_m.group(1).strip() if heading_m else f.stem
        # Заголовок часто = просто "EXP-<id>" без темы — тогда берём первую
        # фразу из "О прогоне", она как раз пишется как краткое описание.
        if re.fullmatch(r"EXP-[\d-]+[\w-]*", heading):
            about_m = re.search(r"\*\*О прогоне\.\*\*\s*(.+?)\n\n", text, re.DOTALL)
            topic = first_sentence(about_m.group(1)) if about_m else "—"
        else:
            topic = heading.split("—", 1)[-1].strip() if "—" in heading else heading

        q_m = re.search(
            r"\*\*Вопросы для эксперта[^*]*\.\*\*\s*(.+?)(?=\n\n---|\n\*\*\[для инженера\]|\Z)",
            text, re.DOTALL,
        )
        n_questions = 0
        if q_m:
            block = q_m.group(1).strip()
            if not re.match(r"^(Нет|нет)\b", block):
                n_questions = len(re.findall(r"^\d+\.\s", block, re.MULTILINE)) or 1

        rows.append({
            "date": date, "id": f.stem, "topic": topic,
            "n_questions": n_questions, "path": f.name,
        })
    return rows


def scan_expert_questions():
    rows = []
    if not FEATURES_DIR.exists():
        return rows
    for oq_file in sorted(FEATURES_DIR.glob("*/open-questions.md")):
        try:
            text = oq_file.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        headings = list(OQ_HEADING.finditer(text))
        for i, hm in enumerate(headings):
            start, end = hm.end(), (headings[i + 1].start() if i + 1 < len(headings) else len(text))
            block = text[start:end]
            src_m = SOURCE_FIELD.search(block)
            if not src_m:
                continue
            links = [(t, p) for t, p in LINK.findall(src_m.group(1)) if "docs/experiments/EXP-" in p]
            if not links:
                continue
            status_m = STATUS_FIELD.search(block)
            status = status_m.group(1).strip().rstrip(".") if status_m else "open"
            if status.startswith("resolved"):
                continue
            rows.append({
                "id": hm.group(1), "title": hm.group(2).strip(),
                "status": status, "links": links,
            })
    return rows


def render(experiments, questions) -> tuple:
    exp_lines = ["## Таблица экспериментов", ""]
    exp_lines.append("| Дата | Прогон | О чём | Вопросов эксперту |")
    exp_lines.append("|---|---|---|---|")
    for r in experiments:
        q = f"**{r['n_questions']}**" if r["n_questions"] else "0"
        exp_lines.append(f"| {r['date']} | [{r['id']}]({r['path']}) | {r['topic']} | {q} |")
    exp_block = "\n".join(exp_lines)

    q_lines = ["## Открытые вопросы для эксперта (из экспериментов)", ""]
    if questions:
        q_lines.append("| Вопрос | Статус | Прогон |")
        q_lines.append("|---|---|---|")
        for q in questions:
            links = ", ".join(f"[{t}]({p.split('/')[-1]})" for t, p in q["links"])
            q_lines.append(f"| **{q['id']}** — {q['title']} | {q['status']} | {links} |")
    else:
        q_lines.append("Нет открытых вопросов, заведённых из экспериментов.")
    q_block = "\n".join(q_lines)

    return exp_block, q_block


def _wrap(name: str, block: str) -> str:
    return f"<!-- AUTO:{name}:START -->\n{block}\n<!-- AUTO:{name}:END -->"


def splice(readme_text: str, exp_block: str, q_block: str) -> str:
    def find_pattern(name):
        return re.compile(
            re.escape(f"<!-- AUTO:{name}:START -->") + r".*?" + re.escape(f"<!-- AUTO:{name}:END -->"),
            re.DOTALL,
        )

    exp_pat, q_pat = find_pattern("EXPERIMENTS-TABLE"), find_pattern("EXPERT-QUESTIONS")
    text = readme_text
    has_exp, has_q = bool(exp_pat.search(text)), bool(q_pat.search(text))

    if has_exp:
        text = exp_pat.sub(_wrap("EXPERIMENTS-TABLE", exp_block), text)
    if has_q:
        text = q_pat.sub(_wrap("EXPERT-QUESTIONS", q_block), text)
    if has_exp and has_q:
        return text

    # Один или оба маркера отсутствуют (обычно — первый запуск): вставить
    # недостающие СРАЗУ ОБА одним куском (таблица, затем вопросы), в точку
    # сразу после intro-абзаца (перед первым "## "), не сканируя заново
    # промежуточный текст — иначе второй insert может найти "## " внутри
    # только что вставленного первого блока.
    missing = []
    if not has_exp:
        missing.append(_wrap("EXPERIMENTS-TABLE", exp_block))
    if not has_q:
        missing.append(_wrap("EXPERT-QUESTIONS", q_block))
    combined = "\n\n".join(missing)
    insert_at = text.find("\n## ")
    if insert_at == -1:
        return text.rstrip() + f"\n\n{combined}\n"
    return text[:insert_at] + f"\n\n{combined}\n" + text[insert_at:]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()

    experiments = scan_experiments()
    questions = scan_expert_questions()
    exp_block, q_block = render(experiments, questions)

    if a.write:
        current = README.read_text(encoding="utf-8") if README.exists() else "# docs/experiments\n"
        README.write_text(splice(current, exp_block, q_block), encoding="utf-8")
        print(f"written: {README.relative_to(ROOT)} "
              f"({len(experiments)} прогонов, {len(questions)} открытых вопросов)")
    else:
        print(exp_block)
        print()
        print(q_block)
    return 0


if __name__ == "__main__":
    sys.exit(main())
