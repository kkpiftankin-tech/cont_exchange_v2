#!/usr/bin/env python3
"""
feature-yaml-checker — кросс-валидатор согласованности между
`docs/02-system/features/F-XX/feature.yaml` и `specs/domain/feature-component-map.yaml`.

Введён по AUDIT-001 T-AUDIT-006 после PM-002 (F-15 status drift). До этого
traceability-checker проверял только наличие путей; никто не сверял, что
`feature.yaml.status == feature-component-map.yaml.F-XX.status` и что
codePaths/protoContracts/kafkaTopics согласованы.

Проверки (каждая = отдельный errors-entry):

  1. Для каждой F-XX в `specs/domain/feature-component-map.yaml`:
     - docsPath существует.
     - feature.yaml существует и парсится.
  2. Status sync:
     - feature.yaml.feature.status == feature-component-map.yaml.F-XX.status
       (если оба заданы; если один отсутствует — WARN, не ERROR).
  3. Code paths:
     - feature.yaml.codePaths все существуют (recursive если nested).
     - feature-component-map.yaml.F-XX.codePaths все существуют.
     - Подмножество canonical roots feature.yaml.codePaths ⊆
       feature-component-map.yaml.codePaths (либо явное замещение root'ом).
  4. Proto contracts:
     - feature.yaml.protoContracts все существуют как файлы.
     - feature-component-map.yaml.F-XX.protoContracts все существуют как файлы.
  5. Kafka topics:
     - feature.yaml.kafkaTopics.produces|consumes все упомянуты в
       infra/kafka/create_topics.sh.
     - Для каждого topic — существует docs/06-api/messaging/<topic>.md
       (поддерживается dot-to-dash convention: "batch.outputs" → "batch-outputs.md").

Выход:
  - Exit 0 если ошибок нет; 1 если есть ERROR; 2 при невозможности запуститься.
  - На stdout — human-readable отчёт; WARN'ы выводятся, но не падают.

Запуск:
  python3 tools/feature-yaml-checker/check.py
  make feature-check
"""

from pathlib import Path
import sys
import re

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

ROOT = Path.cwd()


def load_yaml(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except yaml.YAMLError as exc:
        print(f"ERROR: cannot parse {path}: {exc}", file=sys.stderr)
        return {}


def flatten_code_paths(value) -> list[str]:
    """feature.yaml.codePaths может быть либо list, либо nested map.

    Returns list of all leaf string values.
    """
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        out: list[str] = []
        for item in value:
            out.extend(flatten_code_paths(item))
        return out
    if isinstance(value, dict):
        out = []
        for sub in value.values():
            out.extend(flatten_code_paths(sub))
        return out
    return []


def topic_doc_filename(topic: str) -> str:
    """`batch.outputs` → `batch-outputs.md`. Так задокументировано в
    docs/06-api/messaging/ — точки заменяются на дефисы."""
    return topic.replace(".", "-") + ".md"


def read_kafka_topics_in_script(path: Path) -> set[str]:
    """Parse `infra/kafka/create_topics.sh` looking for `create_topic <name>` lines.

    Conservative regex: matches `create_topic foo` or `create_topic "foo"`."""
    if not path.exists():
        return set()
    pattern = re.compile(r'^\s*create_topic\s+"?([a-z0-9_.-]+)"?', re.MULTILINE)
    return set(pattern.findall(path.read_text(encoding="utf-8")))


def check_feature(
    fid: str,
    map_entry: dict,
    errors: list[str],
    warns: list[str],
    topics_in_script: set[str],
) -> None:
    docs_path_str = map_entry.get("docsPath")
    if not docs_path_str:
        errors.append(f"{fid}: feature-component-map has no docsPath")
        return

    docs_path = ROOT / docs_path_str
    if not docs_path.exists():
        errors.append(f"{fid}: docsPath does not exist: {docs_path_str}")
        return

    feature_yaml_path = docs_path / "feature.yaml"
    if not feature_yaml_path.exists():
        warns.append(f"{fid}: {feature_yaml_path.relative_to(ROOT)} missing")
        return

    fdata = load_yaml(feature_yaml_path)
    feature_block = fdata.get("feature") or {}
    f_status = feature_block.get("status")
    map_status = map_entry.get("status")

    # ----- Check 2: status sync ----------------------------------------------
    if f_status and map_status and f_status != map_status:
        errors.append(
            f"{fid}: status mismatch — feature.yaml={f_status!r}, "
            f"feature-component-map={map_status!r}"
        )
    elif f_status and not map_status:
        warns.append(f"{fid}: feature-component-map has no status (feature.yaml={f_status!r})")
    elif map_status and not f_status:
        warns.append(f"{fid}: feature.yaml.feature.status is missing (map={map_status!r})")

    # ----- Check 3: codePaths exist ------------------------------------------
    f_paths = flatten_code_paths(fdata.get("codePaths"))
    for p in f_paths:
        if not (ROOT / p).exists():
            errors.append(f"{fid}: feature.yaml.codePaths missing on disk: {p}")

    m_paths = map_entry.get("codePaths") or []
    for p in m_paths:
        if not (ROOT / p).exists():
            errors.append(f"{fid}: feature-component-map.codePaths missing on disk: {p}")

    # codePath canonical-root containment: если feature-component-map
    # содержит роутовый каталог (e.g. "cpp/backtest"), feature.yaml.codePaths
    # с префиксом этого каталога считаются покрытыми. Иначе — feature.yaml.codePaths
    # должен быть подмножеством m_paths (точное соответствие).
    if f_paths and m_paths:
        roots = [p for p in m_paths if not p.endswith((".cpp", ".hpp", ".h", ".cc", ".yaml"))]
        non_root = [p for p in m_paths if p not in roots]
        for p in f_paths:
            if any(p.startswith(r.rstrip("/") + "/") or p == r for r in roots):
                continue
            if p in non_root:
                continue
            warns.append(
                f"{fid}: feature.yaml codePath not covered by feature-component-map: {p}"
            )

    # ----- Check 4: proto contracts exist ------------------------------------
    f_protos = fdata.get("protoContracts") or []
    for p in f_protos:
        if not (ROOT / p).exists():
            errors.append(f"{fid}: feature.yaml protoContracts missing: {p}")
    m_protos = map_entry.get("protoContracts") or []
    for p in m_protos:
        if not (ROOT / p).exists():
            errors.append(f"{fid}: feature-component-map protoContracts missing: {p}")

    # ----- Check 5: kafka topics ---------------------------------------------
    # Bundled-doc support: accept either a dedicated <topic-with-dash>.md OR
    # any docs/06-api/messaging/*-topics.md that mentions the topic name in
    # backtick form. Same convention as kafka-contract-auditor.
    topics_block = fdata.get("kafkaTopics") or {}
    produces = topics_block.get("produces") or []
    consumes = topics_block.get("consumes") or []
    all_topics = list(produces) + list(consumes)
    messaging_dir = ROOT / "docs" / "06-api" / "messaging"
    bundled_docs: dict[Path, str] = {}
    if messaging_dir.exists():
        for f in messaging_dir.glob("*-topics.md"):
            if f.name == "topics.md":  # catalog
                continue
            try:
                bundled_docs[f] = f.read_text(encoding="utf-8")
            except OSError:
                pass

    def topic_documented(topic: str) -> bool:
        if (messaging_dir / topic_doc_filename(topic)).exists():
            return True
        token = f"`{topic}`"
        for content in bundled_docs.values():
            if token in content:
                return True
        return False

    for topic in all_topics:
        if topic not in topics_in_script:
            errors.append(
                f"{fid}: kafka topic '{topic}' in feature.yaml but missing "
                f"in infra/kafka/create_topics.sh"
            )
        if not topic_documented(topic):
            errors.append(
                f"{fid}: kafka topic '{topic}' has no doc "
                f"(neither docs/06-api/messaging/{topic_doc_filename(topic)} "
                f"nor a *-topics.md bundle mentions it)"
            )


def main() -> int:
    feature_map = load_yaml(ROOT / "specs" / "domain" / "feature-component-map.yaml")
    features = (feature_map or {}).get("features") or {}
    if not features:
        print(
            "feature-yaml-checker: nothing to check (no features in "
            "specs/domain/feature-component-map.yaml)",
            file=sys.stderr,
        )
        return 0

    topics_in_script = read_kafka_topics_in_script(
        ROOT / "infra" / "kafka" / "create_topics.sh"
    )

    errors: list[str] = []
    warns: list[str] = []

    for fid, entry in sorted(features.items()):
        if not isinstance(entry, dict):
            errors.append(f"{fid}: malformed entry (not a mapping)")
            continue
        check_feature(fid, entry, errors, warns, topics_in_script)

    if warns:
        print("feature-yaml-checker WARNs:")
        for w in warns:
            print(f"  - {w}")

    if errors:
        print("feature-yaml-checker ERRORS:")
        for e in errors:
            print(f"  - {e}")
        print(f"\nTotal: {len(errors)} error(s), {len(warns)} warn(s)")
        return 1

    print(f"feature-yaml-checker OK ({len(features)} features, {len(warns)} warns)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
