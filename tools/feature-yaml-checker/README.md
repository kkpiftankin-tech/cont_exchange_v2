# feature-yaml-checker

Кросс-валидатор согласованности между:

- `docs/02-system/features/F-XX/feature.yaml` (human-readable feature definition)
- `specs/domain/feature-component-map.yaml` (machine-readable mapping)

Введён в [AUDIT-001 T-AUDIT-006](../../docs/00-methodology/audits/AUDIT-001-feature-development-process.md)
после [PM-002 (F-15 status drift)](../../docs/00-methodology/postmortems/PM-002-f15-status-drift.md).

## Что проверяет

1. **Status sync** — `feature.yaml.feature.status == feature-component-map.yaml.F-XX.status`.
2. **codePaths exist** — все пути из обоих источников существуют на диске.
3. **codePaths coverage** (WARN) — `feature.yaml.codePaths` покрыты canonical roots в `feature-component-map.yaml.codePaths`.
4. **protoContracts exist** — все `.proto` файлы существуют.
5. **kafkaTopics** — все topic'и из `feature.yaml.kafkaTopics.{produces,consumes}` упомянуты в `infra/kafka/create_topics.sh` И имеют doc в `docs/06-api/messaging/<topic>.md`.

## Запуск

```bash
python3 tools/feature-yaml-checker/check.py
# или
make feature-check
```

Exit code:

- `0` — нет ошибок (WARN'ы возможны).
- `1` — есть ERROR (сообщения на stdout).
- `2` — невозможно запуститься (pyyaml отсутствует, и т.п.).

## Запуск тестов

```bash
python3 tools/feature-yaml-checker/tests/test_checker.py
# или
python3 -m unittest tools/feature-yaml-checker/tests/test_checker.py
```

## Известные ограничения (на момент landing T-AUDIT-006)

- Не проверяет contractbinding для service-level sequence diagrams (это сделает sequence-diagram-linter, не входит в AUDIT-001).
- Не проверяет код "X в feature.yaml = X в YAML2" — только наличие путей.
- `kafkaTopics` check видит только `feature.yaml.kafkaTopics` (если фича документирована в machine-readable map под `kafkaTopics` блоком — это поле тоже стоит сверять; future enhancement).

## Roadmap (вне AUDIT-001)

- Добавить проверку `restEndpoints` и `openapiContracts`.
- Добавить проверку acceptance criteria непустые.
- Интеграция с `coverage-matrix.md` для автогенерации Status столбца.
