#!/usr/bin/env python3
"""tools/route/route.py — маршрутизатор реплики (IN-019 §6.1, Этап 5, пилот).

Решение владельца 2026-10-27: сужено до `--candidates`/`--ascent`/`--descent`
на том, что реально есть в specs/registry/registry.yaml + docs/frontend/ui-map.yaml.
НЕ строит: хук route_hint.py, навык change, change-routing.md — не трогает
.claude/rules/agent-routing.md (решение владельца: не удалять до
подтверждённого паритета на golden.yaml).

Без LLM — substring/word-overlap скоринг, < 200 мс (нет сетевых вызовов).

--candidates: читает текст реплики из stdin, возвращает JSON:
  {"type": {...}, "anchors": [...]}
  type — скоринг по tools/route/markers.yaml.
  anchors — скоринг word-overlap против заголовков/label'ов узлов реестра
  (feature/capability/uc/calc) + элементов ui-map.yaml, топ-5.

--ascent <ID>: цепочка вверх по оси Ф (UI → CAP/CALC → F), с Р-уровнем
каждого узла (IN-019 §3.2). Грубая реализация — реестр пока не моделирует
Р3/Р5/Р6/Р7/Р9 как узлы (Этапы 2/4 сужены), поэтому цепочка перескакивает
прямо к Р2/Р4/Р8/kite, не проходя промежуточные уровни. Не выдаётся за
полную реализацию §3.2 графа.

--descent <ID>: обратный поиск — что в реестре ссылается на этот ID
(feature → его capabilities/uc/calc). Тоже грубо по той же причине.

Эвристика "верхняя точка"/класс (ТОЛЬКО для eval.py golden-прогона, НЕ
часть алгоритма §3.5 — тот требует шагов 2-6 с живым контекстом/владельцем,
не построен в этом заходе): top = Р-уровень якоря с поправкой по типу
реплики (см. TOP_POINT_BY_TYPE), класс выводится из top по таблице §3.6.
"""
from __future__ import annotations

import argparse
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
UI_MAP_PATH = ROOT / "docs" / "frontend" / "ui-map.yaml"
MARKERS_PATH = ROOT / "tools" / "route" / "markers.yaml"

STOPWORDS = {
    "и", "в", "на", "не", "с", "по", "за", "от", "до", "что", "как", "это",
    "у", "к", "из", "для", "а", "но", "же", "его", "её", "их", "то", "при",
    "the", "a", "is", "of", "to", "in", "on",
}

# §3.2: уровень по типу узла (грубая оценка — реестр не моделирует все Р).
LEVEL_BY_NODE_TYPE = {
    "ui": "Р8", "calc": "Р4", "capability": "Р2", "uc": "Р2",
    "feature": "kite", "adr": "Р3", "requirement": "Р1", "table": "Р7",
}

# §3.6: класс из верхней точки. ask/describe — читающие типы: §3.4 прямо
# говорит "CR не нужна, если цепочка полная" — её top-point (где величина
# определена) не означает, что этот уровень МЕНЯЕТСЯ, только что там
# искать ответ. Класс по §3.6 ("L: изменён хотя бы один из Р3/Р4/Р6/Р7")
# предполагает изменение, поэтому для ask/describe класс не выводится из
# top тем же способом, что для fix/change/extend — дефолт S (ответ, без
# правки), если не найдено явного разрыва; явного разрыва в цепочке этот
# non-LLM скорер не умеет распознавать (нужна семантика §3.5 шагов 3-5,
# не построена в этом заходе) — поэтому всегда S, не угадывание.
def class_from_top(top: str | None, reply_type: str) -> str:
    if reply_type == "new":
        return "XL"
    if reply_type == "spike":
        return "spike"
    if reply_type in ("ask", "describe"):
        return "S"
    if top in ("Р3", "Р4", "Р6", "Р7"):
        return "L"
    if top in ("Р2", "Р8"):
        return "M"
    return "S"


def load_registry() -> dict:
    if not REGISTRY_PATH.exists():
        return {}
    return yaml.safe_load(REGISTRY_PATH.read_text(encoding="utf-8")) or {}


def load_ui_map() -> dict:
    if not UI_MAP_PATH.exists():
        return {}
    return yaml.safe_load(UI_MAP_PATH.read_text(encoding="utf-8")) or {}


def load_markers() -> dict:
    if not MARKERS_PATH.exists():
        return {}
    return (yaml.safe_load(MARKERS_PATH.read_text(encoding="utf-8")) or {}).get("types", {})


def tokenize(text: str) -> set:
    words = re.findall(r"[a-zа-яё0-9_]+", text.lower())
    return {w for w in words if w not in STOPWORDS and len(w) > 2}


def classify_type(text: str, markers: dict) -> dict:
    low = text.lower()
    scores = {}
    for t, phrases in markers.items():
        matched = [p for p in phrases if p in low]
        if matched:
            scores[t] = matched
    if not scores:
        return {"type": "unclear", "matched": {}}
    best = max(scores.items(), key=lambda kv: len(kv[1]))
    return {"type": best[0], "matched": scores}


def build_anchor_index(reg: dict, ui_map: dict) -> list:
    """[(id, node_type, text_for_overlap)]"""
    idx = []
    for feat in reg.get("nodes", {}).get("feature", []):
        idx.append((feat["id"], "feature", feat.get("title", "")))
        for cap in feat.get("capabilities") or []:
            idx.append((cap["id"], "capability", cap.get("title", "")))
    for uc in reg.get("nodes", {}).get("uc", []):
        idx.append((uc["id"], "uc", uc.get("title", "")))
    for calc in reg.get("nodes", {}).get("calc", []):
        idx.append((calc["id"], "calc", calc.get("title", "")))
    for screen in ui_map.get("screens", []):
        for el in screen.get("elements", []):
            text = f"{el.get('label', '')} {el.get('meaning', '')}"
            idx.append((el["id"], "ui", text))
    return idx


# §3.5 шаг 1: "для экрана — маршрут, вкладка, подпись..." — явная
# screen-лексика в реплике повышает приоритет UI-якорей при равном
# word-overlap (иначе длинные title у capability/calc всегда выигрывают
# у короткого label UI-элемента чисто за счёт большего числа слов).
SCREEN_WORDS = {"экран", "экране", "вкладка", "вкладке", "панель", "панели",
                "колонка", "колонку", "график", "блок", "блоке"}


def score_anchors(text: str, index: list, top_n: int = 5) -> list:
    input_tokens = tokenize(text)
    mentions_screen = bool(input_tokens & SCREEN_WORDS)
    scored = []
    for aid, atype, atext in index:
        a_tokens = tokenize(atext)
        overlap = input_tokens & a_tokens
        if not overlap:
            continue
        # нормировка на длину title — иначе длинные формулировки (title
        # capability/calc) почти всегда перебивают короткий label UI
        # только за счёт числа слов, не релевантности.
        score = len(overlap) / max(1, len(a_tokens)) ** 0.5
        if mentions_screen and atype == "ui":
            score += 0.5
        scored.append({
            "id": aid, "type": atype, "score": round(score, 3),
            "matchedWords": sorted(overlap),
        })
    scored.sort(key=lambda x: -x["score"])
    return scored[:top_n]


def cmd_candidates_for_text(text: str) -> dict:
    reg = load_registry()
    ui_map = load_ui_map()
    markers = load_markers()
    type_result = classify_type(text, markers)
    index = build_anchor_index(reg, ui_map)
    anchors = score_anchors(text, index)
    return {"type": type_result, "anchors": anchors}


def cmd_candidates() -> dict:
    return cmd_candidates_for_text(sys.stdin.read())


def find_node(reg: dict, ui_map: dict, node_id: str):
    for feat in reg.get("nodes", {}).get("feature", []):
        if feat["id"] == node_id:
            return feat, "feature"
        for cap in feat.get("capabilities") or []:
            if cap["id"] == node_id:
                return {**cap, "_parentFeature": feat["id"]}, "capability"
    for uc in reg.get("nodes", {}).get("uc", []):
        if uc["id"] == node_id:
            return uc, "uc"
    for calc in reg.get("nodes", {}).get("calc", []):
        if calc["id"] == node_id:
            return calc, "calc"
    for screen in ui_map.get("screens", []):
        for el in screen.get("elements", []):
            if el["id"] == node_id:
                return {**el, "_screen": screen["id"]}, "ui"
    return None, None


def find_owning_capability(reg: dict, node_id: str, field: str):
    """Есть ли capability, у которой node_id в поле `field` (calcs/ui/ucs)?
    Возвращает (capability_dict, feature_id) или (None, None)."""
    for feat in reg.get("nodes", {}).get("feature", []):
        for cap in feat.get("capabilities") or []:
            values = cap.get(field) or []
            if node_id in values or any(v.split("#")[0] == node_id for v in values if isinstance(v, str)):
                return cap, feat["id"]
    return None, None


def ascent_chain(node_id: str, reg: dict, ui_map: dict) -> list:
    chain = []
    node, ntype = find_node(reg, ui_map, node_id)
    if node is None:
        return {"error": f"{node_id} не найден в реестре/ui-map"}
    chain.append({"id": node_id, "level": LEVEL_BY_NODE_TYPE.get(ntype, "?"), "type": ntype})

    feat_id = None
    if ntype == "ui":
        cap, feat_id = find_owning_capability(reg, node_id, "ui")
        calc_ids = node.get("calc")
        calc_ids = [calc_ids] if isinstance(calc_ids, str) else (calc_ids or [])
        for cid in calc_ids:
            cnode, _ = find_node(reg, ui_map, cid)
            if cnode:
                chain.append({"id": cid, "level": "Р4", "type": "calc"})
                if not feat_id:
                    feat_id = cnode.get("feature")
        if cap:
            chain.append({"id": cap["id"], "level": "Р2", "type": "capability"})
    elif ntype == "calc":
        cap, feat_id = find_owning_capability(reg, node_id, "calcs")
        if cap:
            chain.append({"id": cap["id"], "level": "Р2", "type": "capability"})
        if not feat_id:
            feat_id = node.get("feature")
    elif ntype == "capability":
        feat_id = node["_parentFeature"]
    elif ntype == "uc":
        cap, cap_feat_id = find_owning_capability(reg, node_id, "ucs")
        if cap:
            chain.append({"id": cap["id"], "level": "Р2", "type": "capability"})
        feat_id = cap_feat_id or node.get("feature")

    if feat_id:
        chain.append({"id": feat_id, "level": "kite", "type": "feature"})
    return chain


def cmd_ascent(node_id: str) -> dict:
    reg = load_registry()
    ui_map = load_ui_map()
    chain = ascent_chain(node_id, reg, ui_map)
    if not chain:
        return {"error": f"{node_id} не найден в реестре/ui-map"}
    return {"chain": chain}


def compute_top_point(reply_type: str, anchor_id: str, reg: dict, ui_map: dict) -> str | None:
    """§3.4 "Типичная верхняя точка": ask/fix/change ищут уровень, где
    величина ОПРЕДЕЛЕНА/правило задано — это CALC (Р4), если он есть в
    цепочке подъёма от якоря, иначе CAP (Р2); refine/extend — уровень
    самого якоря (там, где наблюдение сделано, но не описано/не хватает)."""
    if reply_type == "new":
        return "Р1"
    if reply_type == "describe":
        return "Р2"
    if reply_type == "spike" or anchor_id is None:
        return None
    chain = ascent_chain(anchor_id, reg, ui_map)
    if not chain:
        return None
    anchor_level = chain[0]["level"]
    if reply_type in ("ask", "fix", "change"):
        for node in chain:
            if node["type"] == "calc":
                return "Р4"
        for node in chain:
            if node["type"] == "capability":
                return "Р2"
        return anchor_level
    if reply_type in ("refine", "extend"):
        return anchor_level
    return anchor_level


def cmd_descent(node_id: str) -> dict:
    reg = load_registry()
    ui_map = load_ui_map()
    affected = []
    node, ntype = find_node(reg, ui_map, node_id)
    if node is None:
        return {"error": f"{node_id} не найден в реестре/ui-map"}

    if ntype in ("feature", "capability"):
        feat_id = node_id if ntype == "feature" else node["_parentFeature"]
        feat, _ = find_node(reg, ui_map, feat_id)
        for cap in feat.get("capabilities") or []:
            if ntype == "feature" or cap["id"] == node_id:
                affected.append({"id": cap["id"], "level": "Р2", "type": "capability"})
                for ac in cap.get("acceptanceCriteria") or []:
                    affected.append({"id": ac, "level": "Р2", "type": "ac"})
                for uc in cap.get("ucs") or []:
                    affected.append({"id": uc, "level": "Р2", "type": "uc"})
                for calc in cap.get("calcs") or []:
                    affected.append({"id": calc, "level": "Р4", "type": "calc"})
                for ui_id in cap.get("ui") or []:
                    affected.append({"id": ui_id, "level": "Р8", "type": "ui"})
    return {"affected": affected}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--candidates", action="store_true")
    ap.add_argument("--ascent")
    ap.add_argument("--descent")
    ap.add_argument("--runtime", action="store_true")
    args = ap.parse_args()

    if args.candidates:
        print(json.dumps(cmd_candidates(), ensure_ascii=False, indent=2))
    elif args.ascent:
        print(json.dumps(cmd_ascent(args.ascent), ensure_ascii=False, indent=2))
    elif args.descent:
        print(json.dumps(cmd_descent(args.descent), ensure_ascii=False, indent=2))
    elif args.runtime:
        print(json.dumps({"error": "не реализовано в этом заходе (требует живой стенд)"},
                          ensure_ascii=False))
        return 2
    else:
        print("usage: route.py --candidates < text | --ascent ID | --descent ID", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
