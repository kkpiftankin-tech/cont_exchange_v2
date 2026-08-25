# AUDIT-001 — План исправления замечаний по `feature-development-process.md`

> **Источник**: внешний review-документ "Проблемы репозитория v1.md"
> (анализатор смотрел ветку `feature/F-15-ephemeral-persist`).
>
> **Проверка**: 2026-06-08, ветка `feature/F-09-batch-combo-orders`.
>
> **Назначение**: зафиксировать валидность каждого замечания,
> сформировать приоритизированный план исправления и acceptance criteria.

---

## 1. Валидация замечаний

| # | Замечание | На F-09 ветке | Вердикт |
|---|---|---|---|
| 3.1 | "canonical reference" + отсутствуют `feature-development-process.md` и `ingest-and-agents-integration.md` | Оба файла существуют ([feature-development-process.md](../feature-development-process.md), [ingest-and-agents-integration.md](../ingest-and-agents-integration.md)) | ❌ Невалидно для F-09 (валидно для F-15-ephemeral-persist — там их нет). Косвенно указывает на риск broken-links — частично адресован `docs-validation.yml` |
| 3.4 | Нет CI/CD enforcement | Существует 7 workflows: [cpp-build](../../../.github/workflows/cpp-build.yml), [docs-validation](../../../.github/workflows/docs-validation.yml), [proto-contract-audit](../../../.github/workflows/proto-contract-audit.yml), [docs-code-drift](../../../.github/workflows/docs-code-drift.yml), [spec-validation](../../../.github/workflows/spec-validation.yml), [docker-publish](../../../.github/workflows/docker-publish.yml), [deploy-staging](../../../.github/workflows/deploy-staging.yml) | ⚠️ Частично — CI есть, но `traceability-checker` не интегрирован, [Makefile](../../../Makefile) беден (`build`/`test-ci`/`clean`/`check-deps`) |
| 4.1 | F-15 status mismatch | `feature.yaml`: `status: in-progress-impl` + ~30 cpp/backtest paths; `feature-component-map.yaml`: `status: not-implemented`, `codePaths: []`, `primaryComponents` без `backtest-service` | ✅ ВАЛИДНО |
| 4.2 | F-04 Kafka topic `fills` drift | `create_topics.sh`: создаёт ✓; `feature.yaml`: упомянут ✓; `topics.md`: `fills.md` помечен `(planned)`; [docs/06-api/messaging/fills.md](../../06-api/messaging/) — **НЕ существует**; `BatchOutputsProducer` пишет в `fills` ✓ | ✅ ВАЛИДНО (документация отстала от кода и инфры) |
| 4.3 | `batch.outputs` producer/consumer schema mismatch | Producer пишет `fob::matching::v1::BatchOutputs`-wrapper (с `result` + `fills`); consumer (`ledger`) делает try-`BatchOutputs` → fallback try-`BatchResult` | ⚠️ Частично — runtime работает через fallback, но контракт двусмысленный, fragile. Аналогичная проблема для `market_data`/`observability` consumers (не проверено) |
| 4.4 | `UserPromptSubmit` auto-archive hook | Shared `.claude/settings.json`: только `PostToolUse`; local `.claude/settings.local.json`: **тоже без `UserPromptSubmit`**. Только сам скрипт [tools/auto-archive-attachments.py](../../../tools/auto-archive-attachments.py) | ✅ ВАЛИДНО — auto-archive нигде не зарегистрирован, документация в [CLAUDE.md §0a](../../../CLAUDE.md) и [feature-development-process.md §1.1](../feature-development-process.md) обещает то, чего нет на любой машине из коробки |
| 4.5 | Validators проверяют только наличие файлов | `traceability-checker/check.py`: 99 строк, проверяет наличие docsPath/codePaths/protoContracts; `proto-contract-auditor`: проверяет mapping; **отсутствуют**: `feature-yaml-checker`, `kafka-contract-auditor`, `sequence-diagram-linter`, `docs-coverage-gate` | ✅ ВАЛИДНО |

**Итого валидных**: 4 (4.1, 4.2, 4.4, 4.5), 2 частично (3.4, 4.3), 1 невалидно (3.1).

---

## 2. Приоритизация

| Приоритет | Критерий | Задачи |
|---|---|---|
| **P0** — критично | Создаёт runtime/process риск; обещание в доке не подкреплено реальностью | T-AUDIT-001, T-AUDIT-002, T-AUDIT-003, T-AUDIT-004, T-AUDIT-005 |
| **P1** — важно | Усиление gate'ов и автоматизации; ловит будущие drift'ы | T-AUDIT-006, T-AUDIT-007, T-AUDIT-008, T-AUDIT-009 |
| **P2** — улучшение | Качество life-cycle, обучение команды | T-AUDIT-010, T-AUDIT-011 |

---

## 3. Подробный план задач

### T-AUDIT-001 — Sync F-15 в `feature-component-map.yaml` [P0]

**Замечание**: 4.1.

**Проблема**: `specs/domain/feature-component-map.yaml` F-15 говорит `status: not-implemented`, `codePaths: []`, `primaryComponents: [matching, risk, ledger, observability]` — но `docs/02-system/features/F-15-backtest-replay/feature.yaml` уже `in-progress-impl` с большим cpp/backtest codebase и `backtest-service` компонентом.

**Действие**:
1. Открыть [specs/domain/feature-component-map.yaml](../../../specs/domain/feature-component-map.yaml).
2. Обновить раздел `F-15`:
   - `status: in-progress-impl`
   - `primaryComponents`: добавить `backtest-service`, оставить остальные если они в реальности участвуют
   - `protoContracts`: добавить `contracts/proto/fob/replay/v1/replay.proto` (если есть)
   - `codePaths`: скопировать из `feature.yaml.codePaths` или указать корневой `cpp/backtest`
3. Аналогично проверить и при необходимости синхронизировать **остальные** features: пройтись по всем `docs/02-system/features/F-XX/feature.yaml` и сравнить status/primaryComponents/codePaths с `feature-component-map.yaml`.

**Файлы**:
- `specs/domain/feature-component-map.yaml`
- `specs/domain/code-map.yaml` (если status сменился — может потребовать update)
- `docs/traceability/feature-traceability.md` (markdown проекция — должна обновиться)
- `docs/traceability/coverage-matrix.md`

**Acceptance criteria**:
- [ ] Для каждой F-XX: `feature.yaml.status == feature-component-map.yaml.status`.
- [ ] Для каждой F-XX: подмножество `feature.yaml.codePaths` ⊆ `feature-component-map.yaml.codePaths` (или явно отмечено что full path не нужен).
- [ ] `python3 tools/traceability-checker/check.py` проходит.
- [ ] Расширенный `feature-yaml-checker` (T-AUDIT-006) подтверждает sync.

**Estimated effort**: 2-4 часа (пройтись по 12+ features).

---

### T-AUDIT-002 — Создать `docs/06-api/messaging/fills.md` [P0]

**Замечание**: 4.2.

**Проблема**: Kafka topic `fills` существует физически (в `create_topics.sh`, в producer, в Acceptance Criteria F-04), но его документация-контракт отсутствует — `topics.md` явно помечает `(planned)`. Это нарушает [CLAUDE.md §0a "Contract rule"](../../../CLAUDE.md) — "Missing contracts become TODO contract files, not unlinked names".

**Действие**:
1. Создать [docs/06-api/messaging/fills.md](../../06-api/messaging/) по шаблону из [docs/06-api/messaging/batch-outputs.md](../../06-api/messaging/batch-outputs.md):
   - Topic name: `fills`
   - Schema: `fob::matching::v1::FillEvent` (проверить точное имя в `contracts/proto/fob/matching/v1/batch.proto` или отдельном файле)
   - Producers: `matching` (через `BatchOutputsProducer::produce_fills`)
   - Consumers: список (см. F-04, F-09)
   - Partition key: `order_id` (?)
   - Delivery semantics: at-least-once + idempotent
   - Retention: исходя из retention_for_topic в create_topics.sh
2. Обновить `docs/06-api/messaging/topics.md`:
   - Убрать `(planned, см. Conflict Note C-1 ниже)` для fills.
   - Resolve Conflict Note C-1 (если она там есть).
3. Обновить `docs/02-system/features/F-04-batch-clearing/feature.yaml`:
   - В `kafkaTopics.produces` явно указать `fills` + schema reference.

**Файлы**:
- `docs/06-api/messaging/fills.md` (создать)
- `docs/06-api/messaging/topics.md` (обновить таблицу + Conflict Notes)
- `docs/02-system/features/F-04-batch-clearing/feature.yaml`
- `docs/02-system/features/F-09-batch-combo-orders/feature.yaml` (тоже использует fills с расширением — см. existing block в topics.md)
- `specs/domain/feature-component-map.yaml` — добавить fills в kafkaTopics блок если отсутствует

**Acceptance criteria**:
- [ ] `docs/06-api/messaging/fills.md` существует.
- [ ] `topics.md` больше не содержит "planned" пометки для fills.
- [ ] `feature.yaml` F-04 содержит `fills` в `kafkaTopics.produces` с reference на fills.md.
- [ ] kafka-contract-auditor (T-AUDIT-007) проходит.

**Estimated effort**: 2-3 часа.

---

### T-AUDIT-003 — Унифицировать контракт `batch.outputs` [P0]

**Замечание**: 4.3.

**Проблема**: Producer пишет `BatchOutputs` wrapper, consumer ledger делает dual-parse fallback. Это работает, но:
1. Контракт неоднозначный — что считать canonical?
2. Если новый consumer напишет наивный `from_bytes(payload, BatchResult)`, он будет фейлиться на текущем wire-формате.
3. Документация `docs/06-api/messaging/batch-outputs.md` говорит одно, код делает другое.

**Действие** (рекомендация — Option A из review):
1. Прочитать `docs/06-api/messaging/batch-outputs.md` и [contracts/proto/fob/matching/v1/](../../../contracts/proto/fob/matching/v1/) для понимания текущей схемы.
2. Решить: ADR для `batch.outputs = BatchResult` (fills в отдельном топике) ИЛИ `batch.outputs = BatchOutputs` (wrapper).
3. Если выбран `BatchResult` (рекомендуемо — fills уже идут в `fills` topic отдельно):
   - Изменить `BatchOutputsProducer::produce()` — публиковать сырой `BatchResult` напрямую.
   - Удалить `fob::matching::v1::BatchOutputs` proto-message (или пометить deprecated с migration plan).
   - Упростить `KafkaConsumers::loop_batch_outputs()` — убрать fallback path, оставить только `BatchResult`-parse.
4. Если выбран `BatchOutputs`:
   - Удалить fallback в consumer.
   - Все будущие consumers ОБЯЗАНЫ ожидать wrapper.
5. ADR в `docs/03-architecture/adr/ADR-034-batch-outputs-canonical-schema.md`.

**Файлы**:
- `cpp/matching/src/infra/kafka/batch_outputs_producer.cpp`
- `cpp/ledger/src/infra/kafka_consumers.cpp`
- `cpp/market_data/...` (тоже подписывается на `batch.outputs`?)
- `cpp/observability/...` (подписывается?)
- `cpp/backtest/...` (для F-15 backtest_uc)
- `contracts/proto/fob/matching/v1/batch.proto` (если меняется message)
- `docs/06-api/messaging/batch-outputs.md`
- `docs/03-architecture/adr/ADR-034-batch-outputs-canonical-schema.md` (создать)

**Acceptance criteria**:
- [ ] Один canonical message type для `batch.outputs` (документирован в ADR + topic doc).
- [ ] Producer и все consumers используют ТОЛЬКО canonical type — никаких fallback'ов.
- [ ] kafka-contract-auditor (T-AUDIT-007) подтверждает однозначность.
- [ ] Existing tests проходят, e2e на ubuntu-dev (skill `rebuild-service`) показывает корректное применение fills в ledger.

**Estimated effort**: 4-8 часов (зависит от того сколько consumers нужно тронуть + integration testing).

**Риски**: breaking change для consumers если выбран BatchResult. Нужна координация деплоя.

---

### T-AUDIT-004 — Зарегистрировать `UserPromptSubmit` hook в shared settings [P0]

**Замечание**: 4.4.

**Проблема**: Документация ([CLAUDE.md §0a](../../../CLAUDE.md), [feature-development-process.md §1.1](../feature-development-process.md)) обещает что вложения в чате автоматически архивируются в `incoming-docs/`. Реальность: `tools/auto-archive-attachments.py` существует, но hook `UserPromptSubmit` нигде не зарегистрирован — ни в shared `.claude/settings.json`, ни в local `.claude/settings.local.json`. На новой машине / новой инсталляции — auto-archive не работает.

**Действие**:
1. Решить: shared (всем) или per-user (рекомендуется в example)?
   - **Аргументы за shared**: каждый член команды получает одинаковое поведение.
   - **Аргументы за example**: некоторые могут не хотеть, чтобы каждый промт парсился.
2. **Рекомендуется**: shared. Auto-archive нужен для governance — без него ломается весь pipeline `ingress(open)`.
3. Изменить `.claude/settings.json`:
   ```json
   "hooks": {
     "UserPromptSubmit": [
       {
         "hooks": [
           {
             "type": "command",
             "command": "python3 \"$CLAUDE_PROJECT_DIR/tools/auto-archive-attachments.py\""
           }
         ]
       }
     ],
     "PostToolUse": [ ... existing ... ]
   }
   ```
4. Создать smoke-test `tools/auto-archive-attachments.py --self-test` который вызывается из CI (один из workflows) — убеждается что parser работает на канонических `<document>` блоках.
5. Обновить [CLAUDE.md §0a "Auto-archive of chat attachments"](../../../CLAUDE.md):
   - Подтвердить что hook в shared settings.json.
   - Убрать упоминание `.claude/settings.local.json` как primary location (только как override).
6. Обновить [feature-development-process.md §1.1](../feature-development-process.md).

**Файлы**:
- `.claude/settings.json`
- `tools/auto-archive-attachments.py` (добавить `--self-test`)
- `.github/workflows/spec-validation.yml` или новый workflow — call self-test
- `CLAUDE.md` §0a
- `docs/00-methodology/feature-development-process.md` §1.1

**Acceptance criteria**:
- [ ] `.claude/settings.json` содержит `UserPromptSubmit` hook.
- [ ] `python3 tools/auto-archive-attachments.py --self-test` возвращает 0.
- [ ] CI прогоняет self-test.
- [ ] На свежем clone репозитория без `.claude/settings.local.json` пользовательский тест "пришли документ в чате → файл появился в `incoming-docs/`" проходит.

**Estimated effort**: 1-2 часа.

---

### T-AUDIT-005 — Создать `.claude/settings.example.json` [P0]

**Замечание**: 4.4 (вспомогательная).

**Проблема**: Новый разработчик не знает, что положить в personal `settings.local.json` для override.

**Действие**:
1. Создать [.claude/settings.example.json](../../../.claude/) — referenced пример с:
   - Permissions.allow с типичными rsync/docker commands (как сейчас у текущего пользователя).
   - Опциональным `UserPromptSubmit` override (на случай если кто-то хочет отключить).
   - Документированными комментариями `_comment` для каждого блока.
2. Добавить раздел в [Development.md](../../../Development.md) или README:
   - "Если хотите автоматизировать частые операции — `cp .claude/settings.example.json .claude/settings.local.json` и отредактируйте под себя."

**Файлы**:
- `.claude/settings.example.json` (создать)
- `Development.md` или `README.md`

**Acceptance criteria**:
- [ ] Файл существует и валидный JSON.
- [ ] Описано в Development.md как использовать.

**Estimated effort**: 30 минут.

---

### T-AUDIT-006 — `feature-yaml-checker` [P1]

**Замечание**: 4.5.

**Проблема**: Нет валидатора, который проверяет согласованность между `feature.yaml` и `feature-component-map.yaml` (то самое, что было пропущено и привело к F-15 status drift).

**Действие**:
1. Создать [tools/feature-yaml-checker/check.py](../../../tools/) с правилами:
   - Для каждой F-XX в `docs/02-system/features/F-XX-*/feature.yaml`:
     - **Status sync**: `feature.yaml.status == feature-component-map.yaml.features.F-XX.status`.
     - **Code paths sync**: `feature.yaml.codePaths` non-empty ⇒ `feature-component-map.yaml.codePaths` non-empty И ⊆.
     - **Proto contracts sync**: `feature.yaml.protoContracts` ⊆ `feature-component-map.yaml.protoContracts`.
     - **Kafka topics**: каждый topic из `feature.yaml.kafkaTopics.{produces,consumes}` существует в `infra/kafka/create_topics.sh` И имеет doc в `docs/06-api/messaging/<topic>.md`.
     - **REST endpoints**: каждый `restEndpoints` имеет doc в `docs/06-api/rest/`.
     - **Acceptance criteria**: feature имеет хотя бы один AC с непустым ID.
2. Exit code 1 при любом нарушении, structured output (JSON или human-readable).
3. Добавить unit-tests для самого чекера (test fixtures с известными ошибками).

**Файлы**:
- `tools/feature-yaml-checker/check.py`
- `tools/feature-yaml-checker/tests/` (test fixtures)
- `tools/feature-yaml-checker/README.md`

**Acceptance criteria**:
- [ ] Запуск на текущем state репозитория ловит хотя бы 1 mismatch (F-15).
- [ ] После T-AUDIT-001 (sync F-15) — exit 0.
- [ ] Подключён в `Makefile` target `governance-check`.
- [ ] Подключён в CI (T-AUDIT-008).

**Estimated effort**: 4-6 часов (включая unit-tests).

---

### T-AUDIT-007 — `kafka-contract-auditor` [P1]

**Замечание**: 4.5, 4.2, 4.3.

**Проблема**: Нет cross-validator для Kafka topics. Контракт растёт в 5 местах (feature.yaml, topics.md, <topic>.md, create_topics.sh, producer code, consumer code) — drift неизбежен.

**Действие**:
1. Создать [tools/kafka-contract-auditor/check.py](../../../tools/):
   - Сначала собрать canonical список topics:
     - Из `infra/kafka/create_topics.sh` (через `grep "^create_topic"`).
     - Из `docs/06-api/messaging/topics.md` (таблица).
     - Из всех `docs/02-system/features/*/feature.yaml` → `kafkaTopics.produces|consumes`.
   - Проверить **пересечение/полноту**:
     - Каждый topic в `create_topics.sh` имеет `docs/06-api/messaging/<topic>.md`.
     - Каждый topic упомянут в `topics.md`.
     - Каждый topic, упомянутый в любом `feature.yaml`, есть в `create_topics.sh`.
   - Опционально (Stage 2): grep по `cpp/*/src/**.cpp` для `produce(` / `subscribe(` literal'ов — сравнить с canonical.
2. Exit 1 при mismatch с подробным отчётом ("topic X is in feature.yaml F-04 but not in create_topics.sh").
3. Unit-tests с фейковыми scenarios.

**Файлы**:
- `tools/kafka-contract-auditor/check.py`
- `tools/kafka-contract-auditor/README.md`
- `tools/kafka-contract-auditor/tests/`

**Acceptance criteria**:
- [ ] На текущем state ловит `fills` doc gap (T-AUDIT-002 ещё не закрыт).
- [ ] После T-AUDIT-002 (создание fills.md) — exit 0.
- [ ] Подключён в Makefile + CI.

**Estimated effort**: 6-8 часов.

---

### T-AUDIT-008 — Расширить CI с governance-checks [P1]

**Замечание**: 3.4, 4.5.

**Проблема**: `traceability-checker` и будущие `feature-yaml-checker` / `kafka-contract-auditor` не интегрированы в CI.

**Действие**:
1. Расширить `.github/workflows/spec-validation.yml` ИЛИ создать `.github/workflows/governance.yml`:
   ```yaml
   name: governance
   on: [pull_request, push]
   jobs:
     governance:
       runs-on: ubuntu-latest
       steps:
         - uses: actions/checkout@v4
         - uses: actions/setup-python@v5
           with: {python-version: "3.12"}
         - run: pip install pyyaml
         - run: python3 tools/traceability-checker/check.py
         - run: python3 tools/proto-contract-auditor/check_proto_map.py
         - run: python3 tools/feature-yaml-checker/check.py
         - run: python3 tools/kafka-contract-auditor/check.py
         - run: python3 tools/auto-archive-attachments.py --self-test
   ```
2. Сделать обязательным для merge в `main` через GitHub branch protection (config — README или ADR).

**Файлы**:
- `.github/workflows/governance.yml` (создать или расширить существующий spec-validation.yml)
- `Development.md` (описать gate)

**Acceptance criteria**:
- [ ] Все checks запускаются на PR.
- [ ] Failed governance check блокирует merge в `main`.

**Estimated effort**: 1-2 часа.

---

### T-AUDIT-009 — Расширить Makefile [P1]

**Замечание**: 3.4.

**Проблема**: Makefile содержит только `build`/`test-ci`/`clean`/`check-deps`. Разработчик не имеет local команды для governance-проверки.

**Действие**:
1. Добавить в [Makefile](../../../Makefile):
   ```makefile
   .PHONY: docs-check proto-check feature-check kafka-check governance-check

   docs-check:
   	python3 tools/traceability-checker/check.py

   proto-check:
   	python3 tools/proto-contract-auditor/check_proto_map.py

   feature-check:
   	python3 tools/feature-yaml-checker/check.py

   kafka-check:
   	python3 tools/kafka-contract-auditor/check.py

   governance-check: docs-check proto-check feature-check kafka-check
   ```
2. Добавить `make help` (если ещё нет) — список доступных targets.

**Файлы**:
- `Makefile`

**Acceptance criteria**:
- [ ] `make governance-check` запускает все validators и exit 0 на чистой ветке.
- [ ] `make help` показывает новые targets.

**Estimated effort**: 30 минут (зависит от наличия T-AUDIT-006, T-AUDIT-007).

---

### T-AUDIT-010 — Определение "covered" [P2]

**Замечание**: 4.5 (вспомогательное).

**Проблема**: [docs/traceability/coverage-matrix.md](../../traceability/coverage-matrix.md) использует статус `covered`, но определение не зафиксировано → каждый ставит на свой вкус.

**Действие**:
1. Добавить раздел в [docs/00-methodology/feature-development-process.md](../feature-development-process.md) "Определение covered":
   - `covered-docs`: feature.yaml + README + AC + UC + sys-seq + svc-seq + contracts + data docs существуют.
   - `covered-code`: codePaths существуют, implementation tasks done, tests pass, нет TODO contracts.
   - `covered-runtime`: docker compose стартует, smoke test проходит, Kafka topics существуют, migrations применяются.
2. Обновить `coverage-matrix.md` — добавить колонки `docs`/`code`/`runtime` вместо общего `covered`.

**Файлы**:
- `docs/00-methodology/feature-development-process.md`
- `docs/traceability/coverage-matrix.md`

**Acceptance criteria**:
- [ ] Каждая F-XX имеет 3 явных статуса (docs/code/runtime).
- [ ] Definition зафиксировано и упомянуто в [implementation-plan/README.md](../../implementation-plan/README.md).

**Estimated effort**: 2-3 часа.

---

### T-AUDIT-011 — Post-mortems PM-001 / PM-002 [P2]

**Замечание**: review §5.7 (хорошее предложение).

**Проблема**: Найденные drift'ы (F-04 fills, F-15 status, batch.outputs schema, UserPromptSubmit hook) — не разовые баги, а сбои процесса. Без post-mortem'а команда повторит эти же ошибки в F-XX.

**Действие**:
1. Создать [docs/00-methodology/postmortems/](../) (новый подкаталог) с шаблоном.
2. **PM-001 — F-04 Kafka contract drift**:
   - Symptoms, Root cause (отсутствие kafka-contract-auditor), Fix (T-AUDIT-002 + T-AUDIT-007), Lessons learned.
3. **PM-002 — F-15 status drift**:
   - Symptoms, Root cause (нет feature-yaml-checker), Fix (T-AUDIT-001 + T-AUDIT-006).
4. **PM-003 — batch.outputs schema bifurcation**:
   - Symptoms (dual-parse fallback), Root cause (нет ADR на момент изменения), Fix (T-AUDIT-003).
5. **PM-004 — UserPromptSubmit hook in personal-only settings**:
   - Symptoms (docs обещают, реальность нет), Root cause (нет CI smoke-test для hooks), Fix (T-AUDIT-004).

**Файлы**:
- `docs/00-methodology/postmortems/_template.md`
- `docs/00-methodology/postmortems/PM-001-f04-kafka-contract-drift.md`
- `docs/00-methodology/postmortems/PM-002-f15-status-drift.md`
- `docs/00-methodology/postmortems/PM-003-batch-outputs-schema-bifurcation.md`
- `docs/00-methodology/postmortems/PM-004-userpromptsubmit-hook-personal-only.md`

**Acceptance criteria**:
- [ ] 4 PM-документа существуют со всеми разделами шаблона.
- [ ] [CLAUDE.md §0b "Stable Repository Map"](../../../CLAUDE.md) упоминает `docs/00-methodology/postmortems/`.

**Estimated effort**: 3-4 часа.

---

## 4. Сводная таблица

| Task | P | Effort | Зависимости |
|---|---|---|---|
| T-AUDIT-001 — Sync F-15 в feature-component-map | P0 | 2-4ч | — |
| T-AUDIT-002 — fills.md | P0 | 2-3ч | — |
| T-AUDIT-003 — Унификация batch.outputs | P0 | 4-8ч | ADR-034 |
| T-AUDIT-004 — UserPromptSubmit hook в shared | P0 | 1-2ч | — |
| T-AUDIT-005 — settings.example.json | P0 | 0.5ч | T-AUDIT-004 |
| T-AUDIT-006 — feature-yaml-checker | P1 | 4-6ч | — |
| T-AUDIT-007 — kafka-contract-auditor | P1 | 6-8ч | — |
| T-AUDIT-008 — CI governance.yml | P1 | 1-2ч | T-AUDIT-006, 007 |
| T-AUDIT-009 — Makefile targets | P1 | 0.5ч | T-AUDIT-006, 007 |
| T-AUDIT-010 — Definition of covered | P2 | 2-3ч | — |
| T-AUDIT-011 — Post-mortems | P2 | 3-4ч | — |

**Итого**: ~30-50 часов работы (~1 неделя в полную загрузку).

---

## 5. Что НЕ включено и почему

- **3.1 "canonical reference"** — на F-09 ветке оба файла есть. Link checker (`docs-validation.yml`) уже работает. Замечание было артефактом анализа неактуальной ветки.
- **3.4 "Нет CI"** в целом виде — CI существует, перечислены пробелы конкретно (T-AUDIT-008).
- **Замечание из 5.7 review про "AUDIT-001 как post-mortem на сам отчёт"** — заменено на T-AUDIT-011 с конкретными PM по найденным drift'ам.

---

## 6. Порядок исполнения с учётом активного F-09

### Phase 1 — F-09-safe (выполнять параллельно F-09 build, ~15-20ч)

Не трогает ни одного файла, который F-09 build видит:

```text
T-AUDIT-004 — UserPromptSubmit hook         (1-2ч)  .claude/settings.json
T-AUDIT-005 — settings.example.json         (0.5ч)  новый файл
T-AUDIT-001 — F-15 sync                     (2-4ч)  только F-15 entry в specs/
T-AUDIT-006 — feature-yaml-checker          (4-6ч)  новый tools/
T-AUDIT-007 — kafka-contract-auditor        (6-8ч)  новый tools/
T-AUDIT-009 — Makefile additions            (0.5ч)  additive targets
T-AUDIT-010 — definition of covered         (2-3ч)  docs only
T-AUDIT-011 — post-mortems PM-001..004      (3-4ч)  docs only
```

**Гарантия**: ни один из этих task не трогает `cpp/`, `contracts/proto/`, `infra/postgres/init.sql`,
`infra/kafka/create_topics.sh`, `infra/clickhouse/init.sql` — то есть **build, тесты, docker compose
для F-09 не подвергаются риску**. Изменения попадают в:
- `specs/domain/*.yaml` (только F-15 entry, не F-09)
- `.claude/settings*.json`
- `tools/feature-yaml-checker/`, `tools/kafka-contract-auditor/` (новые директории)
- `Makefile` (additive)
- `docs/` (методология и audits)

Tools T-AUDIT-006 и T-AUDIT-007 будут run-able локально (`make governance-check`), но **НЕ
подключены в CI** — F-09 PR не блокируется.

### Phase 2 — после mergе F-09 в `main` (~10-15ч)

Эти задачи имеют материальные пересечения с F-09 и должны быть скоординированы:

```text
T-AUDIT-002 — fills.md                       (2-3ч)
  Reason: F-09 расширяет fills schema (parentOrderId/executionGroupId/legId).
          Документация должна включать F-09 extensions с самого начала.

T-AUDIT-003 — batch.outputs унификация       (4-8ч)
  Reason: меняет cpp/matching producer + cpp/ledger consumer.
          Риск merge conflict с F-09 matching changes.
          Требует ADR-034.

T-AUDIT-008 — CI governance.yml hookup       (1-2ч)
  Reason: новый gate в CI. Если F-09 PR не пройдёт новые validators,
          merge заблокируется. Подключать ПОСЛЕ того как F-09 merged.
```

Каждая задача — отдельный PR с naming `PR-AUDIT-001..011`.

### Phase-3 backstop

После Phase 1 запустить `make governance-check` локально на F-09 ветке: если выявит проблемы в
F-09 (например, F-09 feature.yaml ссылается на отсутствующий контракт) — открыть отдельные issues,
команда F-09 фиксит до merge. Так Phase 2 (T-AUDIT-008) не заблокирует F-09 неожиданно.

---

## 7. Связь с processes

- Этот AUDIT-001 сам по себе — пример уровня 3 "Architecture / ADR" из [ingest-and-agents-integration.md](../ingest-and-agents-integration.md). T-AUDIT-003 включает создание ADR-034, T-AUDIT-008 расширяет CI gate.
- После завершения AUDIT-001 файлы [feature-development-process.md](../feature-development-process.md), [CLAUDE.md](../../../CLAUDE.md) могут потребовать корректировок — отметить в финальном PR.
- Этот документ ссылочно линкуется из:
  - [docs/00-methodology/README.md](../) (если есть)
  - [docs/traceability/feature-traceability.md](../../traceability/feature-traceability.md) — секция "Process audits"

---

## 8. Статус

**Текущий**: `proposed` (план составлен, исполнение не начато).

После approval'а пользователем — переход в `in-progress`. После T-AUDIT-001..011 закрытия — `closed`.
