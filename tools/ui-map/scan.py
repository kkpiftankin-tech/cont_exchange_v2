#!/usr/bin/env python3
"""tools/ui-map/scan.py — заполняет generated: в docs/frontend/ui-map.yaml.

IN-019 §4.1, Этап 3 (ADR-086), пилот одного экрана (/vector-clearing-live,
решение владельца 2026-10-27). Ищет `data-ui-id="<ID>"` по всему
`frontend/web/src/`, не только в заявленном `component:` экрана — React-страницы
в этом репозитории обычно разбиты на несколько файлов одной папки
(`VectorClearingLive.js` импортирует `SystemPnlBlock.js` и т.п.), и привязка
"один экран = один файл" из примера IN-019 §4.1 для них не работает.

`generated.api` — эвристика: ближайший `fetch(...)` / литерал пути `/api/...`
в пределах 40 строк ВВЕРХ от найденного data-ui-id в том же файле; не
гарантированно точный, помечается как `heuristic: true`, когда найден.

ВНИМАНИЕ про --write: пишет через yaml.safe_dump — сохраняет только ведущий
блок комментариев в начале файла, все ПОЛЕВЫЕ/строчные комментарии (`# TODO
Этап 3 п.3`, placeholder-пометки у capability и т.п.) будут потеряны. Пилотный
docs/frontend/ui-map.yaml (2026-10-27) заполнен вручную по выводу без --write
именно поэтому — проверь `git diff` после --write на не-пилотных файлах,
прежде чем принимать.

Запуск:
  python3 tools/ui-map/scan.py --write   # перезаписывает generated: в ui-map.yaml
  python3 tools/ui-map/scan.py           # печатает найденное, не трогает файл
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
UI_MAP_PATH = ROOT / "docs" / "frontend" / "ui-map.yaml"
FRONTEND_SRC = ROOT / "frontend" / "web" / "src"

DATA_UI_ID_RE = re.compile(r'data-ui-id=["\']([A-Za-z0-9_-]+)["\']')
API_LITERAL_RE = re.compile(r'''["'`](/api/[A-Za-z0-9_\-./{}]*)["'`]''')


def find_data_ui_ids() -> dict[str, dict]:
    """Сканирует весь frontend/web/src — возвращает {id: {file, line, api}}."""
    found: dict[str, dict] = {}
    if not FRONTEND_SRC.exists():
        return found
    for f in FRONTEND_SRC.rglob("*.js"):
        try:
            lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            for m in DATA_UI_ID_RE.finditer(line):
                uid = m.group(1)
                api = None
                window_start = max(0, i - 40)
                for j in range(i, window_start - 1, -1):
                    am = API_LITERAL_RE.search(lines[j])
                    if am:
                        api = am.group(1)
                        break
                found[uid] = {
                    "file": str(f.relative_to(ROOT)),
                    "line": i + 1,
                    "api": api,
                }
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    if not UI_MAP_PATH.exists():
        print(f"ERROR: {UI_MAP_PATH.relative_to(ROOT)} не найден", file=sys.stderr)
        return 2

    ui_map = yaml.safe_load(UI_MAP_PATH.read_text(encoding="utf-8")) or {}
    found = find_data_ui_ids()

    changed = False
    report = []
    for screen in ui_map.get("screens", []):
        declared_component = screen.get("component")
        for el in screen.get("elements", []):
            eid = el["id"]
            hit = found.get(eid)
            if not hit:
                report.append((eid, "NOT FOUND in frontend/web/src — data-ui-id не проставлен в JSX"))
                continue
            mismatch = ""
            if declared_component and hit["file"] != declared_component:
                mismatch = f" (файл отличается от screen.component={declared_component})"
            report.append((eid, f"{hit['file']}:{hit['line']}{mismatch}"
                                 + (f" api={hit['api']}" if hit["api"] else " api=не найден")))
            new_generated = {"component": hit["file"], "line": hit["line"]}
            if hit["api"]:
                new_generated["api"] = f"GET {hit['api']}"
                new_generated["heuristic"] = True
            if el.get("generated") != new_generated:
                el["generated"] = new_generated
                changed = True

    for eid, msg in report:
        print(f"{eid}: {msg}")

    if args.write and changed:
        # yaml.safe_dump не сохраняет комментарии — переносим ведущий блок
        # комментариев исходного файла (шапка с объяснением пилота) вручную.
        orig_lines = UI_MAP_PATH.read_text(encoding="utf-8").splitlines(keepends=True)
        header = []
        for line in orig_lines:
            if line.startswith("#") or line.strip() == "":
                header.append(line)
            else:
                break
        body = yaml.safe_dump(ui_map, allow_unicode=True, sort_keys=False, default_flow_style=False)
        UI_MAP_PATH.write_text("".join(header) + body, encoding="utf-8")
        print(f"\nwritten: {UI_MAP_PATH.relative_to(ROOT)}")
    elif args.write:
        print("\nбез изменений (generated: уже актуален)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
