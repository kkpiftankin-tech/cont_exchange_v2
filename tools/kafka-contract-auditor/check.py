#!/usr/bin/env python3
"""
kafka-contract-auditor — cross-validator Kafka топиков по 4 источникам:

  1. `infra/kafka/create_topics.sh` — реально создаваемые топики (dev/CI).
  2. `docs/06-api/messaging/topics.md` — таблица каталог топиков (human-facing).
  3. `docs/06-api/messaging/<topic>.md` — индивидуальные contract docs.
  4. `docs/02-system/features/F-XX/feature.yaml`.kafkaTopics — topic ↔ feature.

Введён по AUDIT-001 T-AUDIT-007 после PM-001 (F-04 `fills` topic drift) и
PM-003 (`batch.outputs` schema bifurcation). Без него drift между этими
4 источниками не ловится и тонет в PR-review.

Проверки:

  A. Каждый topic в `create_topics.sh` имеет `docs/06-api/messaging/<topic>.md`.
  B. Каждый topic в `create_topics.sh` упомянут в `topics.md` (по grep regex
     для строки `` `topic.name` `` либо `| `topic` |`).
  C. Каждый topic, упомянутый в любом `feature.yaml.kafkaTopics.{produces,consumes}`,
     существует в `create_topics.sh`.
  D. (Stage 2 — best-effort) Каждый `producer_.produce("X", ...)` literal в
     `cpp/**/src/**/*.cpp` соответствует topic из create_topics.sh. Аналогично
     для `consumer.subscribe({"X", ...})`.

Конвенция filename: точки заменяются дефисами — "batch.outputs" → "batch-outputs.md".

Выход:
  - Exit 0 если ошибок нет; 1 если есть ERROR; 2 при init failure.

Запуск:
  python3 tools/kafka-contract-auditor/check.py
  make kafka-check
"""

from pathlib import Path
import re
import sys
import os

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml is required. pip install pyyaml", file=sys.stderr)
    sys.exit(2)

ROOT = Path.cwd()

CREATE_TOPIC_RE = re.compile(r'^\s*create_topic\s+"?([a-z0-9_.-]+)"?', re.MULTILINE)

# Топики которые сейчас используются для logs/metrics infrastructure — у них
# нет индивидуального contract doc (по design), но они есть в create_topics.sh.
# Если в будущем понадобятся per-topic docs — убрать из exemption.
INFRA_TOPICS_NO_DOC = {"logs", "metrics"}

# Шаблоны для grep'а producer/consumer literal'ов в C++. Упрощённый regex.
PRODUCER_RE = re.compile(r'\.produce\(\s*"([a-z0-9_.-]+)"')
SUBSCRIBE_RE = re.compile(r'\.subscribe\(\s*\{\s*"([a-z0-9_.-]+)"')


def topic_doc_filename(topic: str) -> str:
    return topic.replace(".", "-") + ".md"


def load_yaml(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except yaml.YAMLError as exc:
        print(f"ERROR: cannot parse {path}: {exc}", file=sys.stderr)
        return {}


def topics_from_create_script(script: Path) -> set[str]:
    if not script.exists():
        return set()
    return set(CREATE_TOPIC_RE.findall(script.read_text(encoding="utf-8")))


def topics_mentioned_in_catalog(catalog: Path) -> set[str]:
    """topics.md: ищем topic-имена в backtick-quoted form (``foo``) или в
    table-cells ``| `foo` |``. Берём union — sloppy, но достаточно для
    membership-check'а."""
    if not catalog.exists():
        return set()
    text = catalog.read_text(encoding="utf-8")
    backticked = set(re.findall(r"`([a-z0-9_.-]+)`", text))
    # Фильтр: только потенциальные topic-имена (минимум одна точка ИЛИ известный
    # plain name типа "fills"). Иначе попадает мусор из inline-code (`yaml`,
    # `cpp` etc).
    return {t for t in backticked if "." in t or t in {"fills", "logs", "metrics"}}


def topics_from_features() -> dict[str, list[str]]:
    """Returns {topic: [F-XX, ...]} — какие features ссылаются на topic."""
    out: dict[str, list[str]] = {}
    features_dir = ROOT / "docs" / "02-system" / "features"
    if not features_dir.exists():
        return out
    for feature_dir in sorted(features_dir.iterdir()):
        if not feature_dir.is_dir():
            continue
        fyaml = feature_dir / "feature.yaml"
        if not fyaml.exists():
            continue
        data = load_yaml(fyaml)
        fid = (data.get("feature") or {}).get("id") or feature_dir.name.split("-")[0:2]
        if isinstance(fid, list):
            fid = "-".join(fid)
        topics_block = data.get("kafkaTopics") or {}
        for direction in ("produces", "consumes"):
            for t in topics_block.get(direction) or []:
                out.setdefault(t, []).append(str(fid))
    return out


def topics_from_cpp_producers_consumers() -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """Grep cpp/ для produce/subscribe literal'ов. Возвращает
    ({topic: [file:line, ...]}, {topic: [file:line, ...]}).

    Best-effort: regex может пропустить динамические topic names или string
    concat. Этого достаточно чтобы ловить очевидные drift'ы."""
    producers: dict[str, list[str]] = {}
    consumers: dict[str, list[str]] = {}
    cpp_root = ROOT / "cpp"
    if not cpp_root.exists():
        return producers, consumers

    for cpp_file in cpp_root.rglob("*.cpp"):
        try:
            content = cpp_file.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = cpp_file.relative_to(ROOT)
        for m in PRODUCER_RE.finditer(content):
            topic = m.group(1)
            producers.setdefault(topic, []).append(str(rel))
        for m in SUBSCRIBE_RE.finditer(content):
            topic = m.group(1)
            consumers.setdefault(topic, []).append(str(rel))
    return producers, consumers


def main() -> int:
    script = ROOT / "infra" / "kafka" / "create_topics.sh"
    catalog = ROOT / "docs" / "06-api" / "messaging" / "topics.md"
    messaging_dir = ROOT / "docs" / "06-api" / "messaging"

    script_topics = topics_from_create_script(script)
    catalog_topics = topics_mentioned_in_catalog(catalog)
    feature_topics = topics_from_features()
    cpp_producers, cpp_consumers = topics_from_cpp_producers_consumers()

    if not script_topics:
        print(
            f"kafka-contract-auditor: {script} has no create_topic lines — nothing to audit",
            file=sys.stderr,
        )
        return 0

    errors: list[str] = []
    warns: list[str] = []

    # ----- Check A: every create_topic.sh topic has a doc (individual or bundled) -
    # Bundled doc convention: e.g. venue-topics.md, replay-topics.md describe
    # multiple topics in one file. We accept either:
    #   - dedicated docs/06-api/messaging/<topic-with-dash>.md, OR
    #   - any docs/06-api/messaging/*-topics.md that mentions the topic name
    #     in backtick form (`topic.name`).
    bundled_docs: dict[Path, str] = {}
    if messaging_dir.exists():
        for f in messaging_dir.glob("*-topics.md"):
            if f.name == "topics.md":  # the catalog itself; handled separately
                continue
            try:
                bundled_docs[f] = f.read_text(encoding="utf-8")
            except OSError:
                pass

    def topic_documented(topic: str) -> bool:
        # Individual doc?
        if (messaging_dir / topic_doc_filename(topic)).exists():
            return True
        # Bundled?
        token = f"`{topic}`"
        for content in bundled_docs.values():
            if token in content:
                return True
        return False

    for topic in sorted(script_topics):
        if topic in INFRA_TOPICS_NO_DOC:
            continue
        if not topic_documented(topic):
            errors.append(
                f"topic '{topic}' is in create_topics.sh but has no doc "
                f"(neither docs/06-api/messaging/{topic_doc_filename(topic)} "
                f"nor a *-topics.md bundle mentions it)"
            )

    # ----- Check B: catalog (topics.md) mentions every script topic ---------
    for topic in sorted(script_topics):
        if topic in INFRA_TOPICS_NO_DOC:
            continue
        if topic not in catalog_topics:
            warns.append(
                f"topic '{topic}' in create_topics.sh but not mentioned in "
                f"docs/06-api/messaging/topics.md"
            )

    # ----- Check C: every feature-mentioned topic exists in script ----------
    for topic, refs in sorted(feature_topics.items()):
        if topic not in script_topics:
            errors.append(
                f"topic '{topic}' referenced by feature(s) {sorted(set(refs))} "
                f"but missing in infra/kafka/create_topics.sh"
            )

    # ----- Check D: cpp producer/consumer literals match script -------------
    for topic, files in sorted(cpp_producers.items()):
        if topic not in script_topics:
            errors.append(
                f"topic '{topic}' is produced by cpp code "
                f"({list(set(files))[:2]}) but missing in create_topics.sh"
            )
    for topic, files in sorted(cpp_consumers.items()):
        if topic not in script_topics:
            errors.append(
                f"topic '{topic}' is consumed by cpp code "
                f"({list(set(files))[:2]}) but missing in create_topics.sh"
            )

    # ----- Output -----------------------------------------------------------
    if warns:
        print("kafka-contract-auditor WARNs:")
        for w in warns:
            print(f"  - {w}")

    if errors:
        print("kafka-contract-auditor ERRORS:")
        for e in errors:
            print(f"  - {e}")
        print(f"\nTotal: {len(errors)} error(s), {len(warns)} warn(s) "
              f"across {len(script_topics)} topics")
        return 1

    print(
        f"kafka-contract-auditor OK "
        f"({len(script_topics)} topics, {len(feature_topics)} feature refs, "
        f"{len(cpp_producers)} cpp producers, {len(cpp_consumers)} cpp consumers, "
        f"{len(warns)} warns)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
