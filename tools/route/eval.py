#!/usr/bin/env python3
"""tools/route/eval.py — прогоняет tools/route/golden.yaml через route.py
(IN-019 §6.5, Этап 5, пилот). Приёмка: якорь >= 18/20, верхняя точка
>= 16/20, класс >= 16/20 (из IN-019 §8 Этап 5) — репортит факт, не
подгоняет эвристику под порог.

Запуск: python3 tools/route/eval.py [--verbose]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parent.parent.parent
GOLDEN_PATH = ROOT / "tools" / "route" / "golden.yaml"

sys.path.insert(0, str(ROOT / "tools" / "route"))
import route  # noqa: E402


def eval_case(case: dict, reg: dict, ui_map: dict) -> dict:
    result = route.cmd_candidates_for_text(case["text"])
    top_anchor = result["anchors"][0]["id"] if result["anchors"] else None
    anchor_ok = top_anchor == case.get("expected_anchor")

    reply_type = result["type"]["type"]
    computed_top = route.compute_top_point(reply_type, top_anchor, reg, ui_map)
    top_ok = computed_top == case.get("expected_top")

    computed_class = route.class_from_top(computed_top, reply_type)
    class_ok = computed_class == case.get("expected_class")

    return {
        "id": case["id"], "text": case["text"],
        "type": {"got": reply_type, "expected": case.get("expected_type"),
                  "ok": reply_type == case.get("expected_type")},
        "anchor": {"got": top_anchor, "expected": case.get("expected_anchor"), "ok": anchor_ok},
        "top": {"got": computed_top, "expected": case.get("expected_top"), "ok": top_ok},
        "class": {"got": computed_class, "expected": case.get("expected_class"), "ok": class_ok},
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not GOLDEN_PATH.exists():
        print(f"ERROR: {GOLDEN_PATH} не найден", file=sys.stderr)
        return 2
    golden = yaml.safe_load(GOLDEN_PATH.read_text(encoding="utf-8")) or {}
    cases = golden.get("cases", [])

    reg = route.load_registry()
    ui_map = route.load_ui_map()
    results = [eval_case(c, reg, ui_map) for c in cases]
    n = len(results)
    type_score = sum(r["type"]["ok"] for r in results)
    anchor_score = sum(r["anchor"]["ok"] for r in results)
    top_score = sum(r["top"]["ok"] for r in results)
    class_score = sum(r["class"]["ok"] for r in results)

    for r in results:
        if args.verbose or not (r["anchor"]["ok"] and r["top"]["ok"] and r["class"]["ok"]):
            print(f"{r['id']}: type {r['type']['got']!r}(exp {r['type']['expected']!r}) "
                  f"anchor {r['anchor']['got']!r}(exp {r['anchor']['expected']!r}) "
                  f"top {r['top']['got']!r}(exp {r['top']['expected']!r}) "
                  f"class {r['class']['got']!r}(exp {r['class']['expected']!r})")

    print(f"\nType: {type_score}/{n}")
    print(f"Anchor: {anchor_score}/{n}  (приёмка >= 18/20)")
    print(f"Top point: {top_score}/{n}  (приёмка >= 16/20)")
    print(f"Class: {class_score}/{n}  (приёмка >= 16/20)")

    target = 20
    ok = (anchor_score >= 18 * n / target and top_score >= 16 * n / target
          and class_score >= 16 * n / target)
    print(f"\nПриёмка Этапа 5 §8: {'ПРОЙДЕНА' if ok else 'НЕ пройдена'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
