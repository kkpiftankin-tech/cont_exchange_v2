# kafka-contract-auditor

Кросс-валидатор согласованности Kafka топиков по 4 источникам:

1. **`infra/kafka/create_topics.sh`** — реально создаваемые топики (dev/CI).
2. **`docs/06-api/messaging/topics.md`** — каталог топиков (human-facing таблица).
3. **`docs/06-api/messaging/<topic>.md`** — индивидуальные contract docs (либо bundled `<group>-topics.md`).
4. **`docs/02-system/features/F-XX/feature.yaml.kafkaTopics`** — feature ↔ topic mapping.

Дополнительно (best-effort): grep `cpp/**/*.cpp` для `producer.produce("X")` и `consumer.subscribe({"X"})` literal'ов.

Введён в [AUDIT-001 T-AUDIT-007](../../docs/00-methodology/audits/AUDIT-001-feature-development-process.md) после:

- [PM-001 (F-04 `fills` topic Kafka contract drift)](../../docs/00-methodology/postmortems/PM-001-f04-kafka-contract-drift.md);
- [PM-003 (`batch.outputs` producer/consumer schema bifurcation)](../../docs/00-methodology/postmortems/PM-003-batch-outputs-schema-bifurcation.md).

## Что проверяет

| Check | Severity |
|---|---|
| A. Topic в `create_topics.sh` имеет doc (`<topic>.md` или mention в bundle `*-topics.md`) | ERROR |
| B. Topic в `create_topics.sh` упомянут в `topics.md` каталоге | WARN |
| C. Topic в `feature.yaml.kafkaTopics` существует в `create_topics.sh` | ERROR |
| D. Topic, появляющийся в `cpp/.../produce("X")` или `subscribe({"X"})`, есть в `create_topics.sh` | ERROR |

Топики `logs`, `metrics` — exemption (infrastructure, без contract doc by design).

## Bundled docs

Принимаются:

- `docs/06-api/messaging/<topic-with-dash>.md` — индивидуальный (predominant convention);
- `docs/06-api/messaging/<group>-topics.md` (например `venue-topics.md`, `replay-topics.md`) — bundled. Бандл считается покрывающим topic, если внутри него есть упоминание ``` `topic.name` ``` (backtick form).

## Запуск

```bash
python3 tools/kafka-contract-auditor/check.py
# или
make kafka-check
```

Exit code:

- `0` — нет ошибок (WARN'ы возможны).
- `1` — есть ERROR.
- `2` — невозможно запуститься (pyyaml отсутствует).

## Запуск тестов

```bash
python3 tools/kafka-contract-auditor/tests/test_auditor.py
# или
python3 -m unittest tools/kafka-contract-auditor/tests/test_auditor.py
```

## Известные ограничения

- regex для `produce`/`subscribe` — упрощённый, не покрывает динамические topic names (e.g. `produce(topic_var, ...)` либо string concat). Best-effort: ловит очевидные literal-drift'ы.
- Не проверяет protobuf message type producer ≡ message type consumer. Этот checker для **топиков**; type-level checker — отдельный (planned, см. AUDIT-001 §3.5 follow-ups).
- `topics.md` mention check использует наивный backtick-grep, может false-positive'ить на inline-code, не относящемся к топикам.
