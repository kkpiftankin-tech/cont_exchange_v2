# Отчёт: процесс разработки фичи F-XX в `cont_exchange_v2.0`

> Назначение: канонический поток — от вложения в чат до закоммиченного кода.
> По каждому этапу указаны руководящие файлы.
>
> Статус: canonical reference. Связано с
> [CLAUDE.md §0a](../../CLAUDE.md),
> [ingest-and-agents-integration.md](ingest-and-agents-integration.md),
> [ADR-018 (design traceability chain)](../03-architecture/adr/),
> [ADR-029 (LLM-assisted development governance)](../03-architecture/adr/).

---

## 0. Что задаёт правила игры

Корневые "constitution" документы, которые обязаны прочитываться всеми участниками и агентами:

- [CLAUDE.md](../../CLAUDE.md) — §0a docs-as-code workflow, §0b repository map, §0c feature/UC/sequence placement, §3 источники истины, §10 архитектурный стиль, §26a sequence diagram rules.
- [docs/00-methodology/ingest-and-agents-integration.md](ingest-and-agents-integration.md) — единая модель **ingest × агенты**, "сэндвич" на каждом уровне.
- [docs/00-methodology/repository-map.md](repository-map.md) — закреплённая 11-folder layout.
- [docs/00-methodology/sequence-diagram-rules.md](sequence-diagram-rules.md) — Mermaid `sequenceDiagram`, где система-уровень vs сервис-уровень vs внутренний.
- [docs/00-methodology/artifact-templates.md](artifact-templates.md) — шаблоны feature.yaml, use-case.md, ADR, implementation tasks.
- [.claude/settings.json](../../.claude/settings.json) — общие `permissions.deny` (никаких `--no-verify`, force-push на main, DROP TABLE, рекурсивный rm) + `PostToolUse` hook.
- ADR [ADR-018](../03-architecture/adr/) (design traceability chain) и [ADR-029](../03-architecture/adr/) (LLM-assisted development governance) — обязательность цепочки и роли LLM.

---

## 1. Этап 1 — приём вложения (intake)

### 1.1. Auto-archive UserPromptSubmit hook

Пользователь присылает документ в чат как `<document>` блок. **До** того как ассистент его увидит, отрабатывает хук:

- [tools/auto-archive-attachments.py](../../tools/auto-archive-attachments.py) — парсит `<document>` блоки, сохраняет каждый как `incoming-docs/YYYY-MM-DD-<slug>-<sha8>.md`, дедуплицирует по SHA1.
- Регистрация хука: [.claude/settings.json](../../.claude/settings.json) — **shared, committed** (с AUDIT-001 T-AUDIT-004 хук работает из коробки у каждого разработчика). Событие `UserPromptSubmit`. Override possible через `.claude/settings.local.json`.
- Smoke-test: `python3 tools/auto-archive-attachments.py --self-test` — CI запускает на каждый PR.
- Поведение зафиксировано в [CLAUDE.md §0a "Auto-archive of chat attachments"](../../CLAUDE.md).

Результат: новый immutable файл в [incoming-docs/](../../incoming-docs/) — например `2026-06-05-F-09-batch-combo-orders-v2.md`.

### 1.2. Жёсткое правило неприкосновенности

Оригиналы в `incoming-docs/` иммутабельны (CLAUDE.md §0a, ingest-docs skill §"Source Archive Rule"). При обновлении присылается новый файл с новой датой/хэшем, старый не правится.

---

## 2. Этап 2 — регистрация IN-NNN

### 2.1. Skill `ingest-docs`

Запускается явным запросом пользователя ("прими IN-XXX", "загрузи F-XX спецификацию"). Skill определён в:

- [.claude/skills/ingest-docs/SKILL.md](../../.claude/skills/ingest-docs/SKILL.md) — обязательный 9-шаговый pipeline: **Register → Segment → Classify → Map → Normalize → Insert/Merge → Link bidirectionally → Validate coverage → Generate implementation tasks**.

### 2.2. Артефакты регистрации

Skill создаёт для каждого входящего источника два файла под [incoming-docs/](../../incoming-docs/):

- `IN-NNN.meta.md` — метаинформация (Source File, Document Type, Processing Status, Target Areas, Conflict Notes). Пример: [incoming-docs/IN-011.meta.md](../../incoming-docs/IN-011.meta.md).
- `IN-NNN.fragment-map.md` — построчная нарезка источника на фрагменты с классификацией и целевыми артефактами. Пример: [incoming-docs/IN-011.fragment-map.md](../../incoming-docs/IN-011.fragment-map.md).
- Индекс [incoming-docs/index.md](../../incoming-docs/index.md) обновляется со ссылкой на новый IN-NNN.

### 2.3. Классификация фрагментов

Skill использует фиксированный enum из ~25 категорий (см. SKILL.md §Classify):
`METHODOLOGY`, `BUSINESS_REQUIREMENT`, `FUNCTIONAL_REQUIREMENT`, `NON_FUNCTIONAL_REQUIREMENT`, `ACTOR`, `EXTERNAL_SYSTEM`, `FEATURE`, `USE_CASE`, `SYSTEM_SEQUENCE_HINT`, `SERVICE_SEQUENCE_HINT`, `CONTAINER`, `COMPONENT`, `DOMAIN_ENTITY`, `DOMAIN_EVENT`, `BUSINESS_RULE`, `CONTRACT_HINT`, `REST_CONTRACT`, `GRPC_CONTRACT`, `KAFKA_TOPIC`, `DATA_MODEL`, `DATA_FLOW`, `TEST_REQUIREMENT`, `OPERATIONAL_REQUIREMENT`, `IMPLEMENTATION_HINT`.

### 2.4. Placement matrix

Каждой категории сопоставлена целевая папка через [.claude/skills/ingest-docs/SKILL.md §Placement Matrix](../../.claude/skills/ingest-docs/SKILL.md). Например `FEATURE → docs/02-system/features/F-XX/`, `KAFKA_TOPIC → docs/06-api/messaging/`, `DATA_MODEL → docs/07-data/`.

### 2.5. Конфликты

Если фрагмент противоречит существующему доку — `Conflict Notes` в целевом артефакте; если архитектурный — ADR в [docs/03-architecture/adr/](../03-architecture/adr/). Молчаливый выбор запрещён ([CLAUDE.md §0a "Conflict rule"](../../CLAUDE.md)). Пример: CN-IN011-01..04 в [IN-011.meta.md](../../incoming-docs/IN-011.meta.md).

---

## 3. Этап 3 — единый сэндвич `ingress → agent → ingress` на каждом уровне SDLC

Это **главная организационная модель**, описанная в [docs/00-methodology/ingest-and-agents-integration.md §3](ingest-and-agents-integration.md):

```text
ingress(open)  — docs-ingestion-engineer #13 открывает уровень
                 (placement, ожидаемые связи)
       ↓
agent(refine)  — профильный специалист авторствует содержание
                 (advisory, читает incoming-docs/<source>)
       ↓
ingress(close) — docs-ingestion-engineer #13 закрывает уровень
                 (writer-of-record для docs/, traceability, coverage)
```

Это **единственный** способ продвижения по уровням — никаких прямых правок `docs/` специалистами (они в plan-mode, read-only). См. [§7 интеграции](ingest-and-agents-integration.md) — "Правила недопущения конфликта".

### 3.1. Координатор

- [.claude/agents/00-sdlc-coordinator.md](../../.claude/agents/00-sdlc-coordinator.md) — навигатор. Не пишет код и доки. Определяет: текущий уровень, недостающие артефакты, следующего агента и точный prompt вызова.

### 3.2. Матрица владения уровень → специалист → выход

Канонический порядок из [.claude/agents/00-sdlc-coordinator.md "Канонический порядок жизненного цикла фичи F-XX"](../../.claude/agents/00-sdlc-coordinator.md) и [§4-5 интеграции](ingest-and-agents-integration.md):

| Уровень | Специалист (refine) | Куда пишет `ingress(close)` |
| --- | --- | --- |
| 1. Business | [01-business-analyst](../../.claude/agents/01-business-analyst.md) | [docs/01-business/](../01-business/) |
| 2. System (feature.yaml, UC, FR/NFR, sys-seq) | [02-system-analyst](../../.claude/agents/02-system-analyst.md) | [docs/02-system/](../02-system/) |
| 3. Architecture / ADR | [03-solution-architect](../../.claude/agents/03-solution-architect.md) | [docs/03-architecture/](../03-architecture/) |
| 4. Domain | [06-trading-domain-specialist](../../.claude/agents/06-trading-domain-specialist.md) | [docs/04-domain/](../04-domain/) |
| 5. Service sequences | [02-system-analyst](../../.claude/agents/02-system-analyst.md) / [08-cpp-service-architect](../../.claude/agents/08-cpp-service-architect.md) | [docs/05-components/sequences/](../05-components/sequences/) |
| 6. Contracts (gRPC/REST/Kafka) | [04-proto-contract-designer](../../.claude/agents/04-proto-contract-designer.md) | [docs/06-api/](../06-api/) + [contracts/proto/fob/](../../contracts/proto/fob/) (+ [14-devops-engineer](../../.claude/agents/14-devops-engineer.md) для [infra/kafka/create_topics.sh](../../infra/kafka/create_topics.sh)) |
| 7. Data | [05-data-schema-designer](../../.claude/agents/05-data-schema-designer.md) | [docs/07-data/](../07-data/) + [14-devops-engineer](../../.claude/agents/14-devops-engineer.md) для [infra/postgres/init.sql](../../infra/postgres/init.sql) |
| 8. Frontend | [07-frontend-architect](../../.claude/agents/07-frontend-architect.md) | feature.yaml `frontend` блок |
| 9. Backend plan | [08-cpp-service-architect](../../.claude/agents/08-cpp-service-architect.md) | [docs/09-implementation/](../09-implementation/) |
| 10. Tests | [09-test-architect](../../.claude/agents/09-test-architect.md) | [docs/10-testing/](../10-testing/) |
| 11. Implementation tasks | [10-implementation-planner](../../.claude/agents/10-implementation-planner.md) | [docs/implementation-plan/F-XX-*.tasks.md](../implementation-plan/) |
| 12. Implementation | [11-code-implementer](../../.claude/agents/11-code-implementer.md) + [14-devops-engineer](../../.claude/agents/14-devops-engineer.md) | `cpp/`, `frontend/`, `infra/` |
| 13. Review | [12-code-reviewer](../../.claude/agents/12-code-reviewer.md), [15-security-reviewer](../../.claude/agents/15-security-reviewer.md) | read-only |

[13-docs-ingestion-engineer](../../.claude/agents/13-docs-ingestion-engineer.md) выступает `ingress` оператором на каждом уровне.

### 3.3. Один уровень — один писатель

Жёсткое правило ([§7 интеграции](ingest-and-agents-integration.md)): один файл в `docs/` за проход правит **только** ingestion-engineer. Специалисты — plan-mode (Read/Grep/Glob), их выход — advisory draft, который `ingress(close)` инкорпорирует.

### 3.4. Skill'ы как сокращения для частых операций

Когда специалист (или главный цикл) выдаёт скелет — используются скиллы из [.claude/skills/](../../.claude/skills/):

- [create-feature](../../.claude/skills/create-feature/SKILL.md) — bootstrap F-XX (feature.yaml + README + UC + sys-seq + svc-seq + traceability stub).
- [register-kafka-topic](../../.claude/skills/register-kafka-topic/SKILL.md) — добавить топик в `create_topics.sh` + `docs/06-api/messaging/<topic>.md` + ссылку из feature.yaml.
- [register-pg-table](../../.claude/skills/register-pg-table/SKILL.md) — `CREATE TABLE` в `init.sql` + `docs/07-data/<table>.md`.

---

## 4. Этап 4 — гейт "no code before docs"

### 4.1. Required chain

Из [CLAUDE.md §0a "No-code-before-docs rule"](../../CLAUDE.md) — до создания кода обязаны существовать все артефакты цепочки:

> Business Requirement → Feature → Use Case → System Sequence → Service Sequence → Contracts → Data Objects → Components → Tests → Code

Этот же чеклист — pre-condition в начале каждого `F-XX-*.tasks.md`. См. [docs/implementation-plan/README.md "Pre-conditions per file"](../implementation-plan/README.md).

### 4.2. Traceability

Прослеживаемость поддерживается в:

- [docs/traceability/feature-traceability.md](../traceability/feature-traceability.md) — markdown проекция цепочки feature → req → UC → seq → contract → data → component → test → code.
- [docs/traceability/coverage-matrix.md](../traceability/coverage-matrix.md) — статус "covered" по уровням для каждой F-XX.
- [docs/traceability/source-to-artifact-map.md](../traceability/source-to-artifact-map.md) — IN-NNN → артефакт.

Машиночитаемые источники истины (компактнее markdown'ов):

- [specs/domain/feature-component-map.yaml](../../specs/domain/feature-component-map.yaml)
- [specs/domain/code-map.yaml](../../specs/domain/code-map.yaml)
- [specs/domain/traceability.yaml](../../specs/domain/traceability.yaml)
- [specs/contracts/proto-map.yaml](../../specs/contracts/proto-map.yaml)

### 4.3. Валидаторы

- `python3 tools/traceability-checker/check.py` — проверяет согласованность YAML/markdown.
- `python3 tools/proto-contract-auditor/check_proto_map.py` — что все proto messages зафиксированы.

### 4.4. Mermaid и contract rules

Из [CLAUDE.md §0a](../../CLAUDE.md) + [§26a](../../CLAUDE.md):

- **Mermaid rule** — interaction scenarios только `sequenceDiagram`, не `graph`/`flowchart`.
- **Contract rule** — каждая стрелка service-level диаграммы должна иметь backing-контракт (REST endpoint в `docs/06-api/rest/`, gRPC в `docs/06-api/grpc/`, Kafka в `docs/06-api/messaging/`, SQL в `docs/07-data/`). Иначе — `TODO contract` файл-заглушка, не unlinked имя.
- Placement рулы для seq diagrams — см. [docs/00-methodology/sequence-diagram-rules.md](sequence-diagram-rules.md).

---

## 5. Этап 5 — implementation plan

После прохождения уровней 1–10 [10-implementation-planner](../../.claude/agents/10-implementation-planner.md) генерирует `docs/implementation-plan/F-XX-short-name.tasks.md`.

### 5.1. Структура tasks-файла

Из [docs/implementation-plan/README.md](../implementation-plan/README.md) и шаблона в [docs/00-methodology/artifact-templates.md](artifact-templates.md):

- Pre-conditions checklist (feature ✓, UC ✓, sys-seq ✓, svc-seq ✓, contracts ✓, data ✓, AC ✓).
- Список задач формата `T-FXX-NNN` или `PR-FXX-NNN` (commit naming).
- Каждая задача — small-scoped: 1-3 файла, маппинг на acceptance criteria из feature.yaml, ссылки на контракты и data schemas.

Примеры: [F-04-batch-clearing.tasks.md](../implementation-plan/F-04-batch-clearing.tasks.md), [F-09-batch-combo-orders.tasks.md](../implementation-plan/F-09-batch-combo-orders.tasks.md), [F-12-execution-hedge.tasks.md](../implementation-plan/F-12-execution-hedge.tasks.md).

---

## 6. Этап 6 — implementation

### 6.1. Code-implementer

- [.claude/agents/11-code-implementer.md](../../.claude/agents/11-code-implementer.md) — реализует **один** конкретный `T-FXX-NNN` за раз, в worktree-изоляции (чтобы параллельные сессии не клобберили друг друга).
- Имеет write-доступ (Read/Grep/Glob/Edit/Write/Bash).
- Обязан читать: feature docs, proto, data schemas, rules из CLAUDE.md, родительский task-файл.
- Commit naming: `PR-FXX-NNN` ([CLAUDE.md §27](../../CLAUDE.md), пример из истории — PR-F02-001..016, PR-F12-3a..15, PR-COMMENTS-001..005).

### 6.2. Devops-engineer

- [.claude/agents/14-devops-engineer.md](../../.claude/agents/14-devops-engineer.md) — единственный автор `infra/` (docker-compose, init.sql, create_topics.sh, env vars, Dockerfile).
- Также пишет CI и runbook.

### 6.3. Жёсткие правила слоёв (для code-implementer)

Из [CLAUDE.md §10](../../CLAUDE.md):

```text
transport/  — HTTP/gRPC handlers, Kafka adapters
app/        — use cases, orchestration
domain/     — entities, value objects, invariants (pure)
infra/      — DB repos, external clients, Kafka wrappers
```

- `domain` не знает о gRPC/Kafka/DB.
- Money — **только** `cex::common::Decimal`, не `double`/`float` ([CLAUDE.md §9](../../CLAUDE.md)).
- Generated `.pb.cc`/`.pb.h` править руками запрещено.

### 6.4. PostToolUse hook

[.claude/settings.json](../../.claude/settings.json) регистрирует `scripts/hooks/post_edit_hint.sh` на `PostToolUse` — после каждого `Edit`/`Write` хук эмиттит контекстные напоминания (rebuild service, init.sql семантика, bundle hash верификация). Это закодированные уроки PR-F02-001..016.

### 6.5. Rebuild и тестирование на dev-хосте

После записи кода — skill [rebuild-service](../../.claude/skills/rebuild-service/SKILL.md):

- rsync исходников на `nik@ubuntu-dev` (Tailscale `100.65.232.81`).
- `docker compose up -d --build <service>`.
- Логи + HW counters проверка.

---

## 7. Этап 7 — quality gates

### 7.1. Code review

- [.claude/agents/12-code-reviewer.md](../../.claude/agents/12-code-reviewer.md) — read-only. Проверяет: корректность, layering, traceability к feature docs, тесты, security, money invariants (Decimal vs double), Kafka topic compatibility, proto backward compat.
- Опциональный slash: `/code-review` (skill `code-review`).

### 7.2. Security review

- [.claude/agents/15-security-reviewer.md](../../.claude/agents/15-security-reviewer.md) — auth/authz, secret handling, replay safety, audit trail, dangerous tool usage, KYC/AML implications.
- Slash: `/security-review` для текущей ветки.

### 7.3. Hard deny rules

[.claude/settings.json](../../.claude/settings.json) `permissions.deny` блокирует независимо от любых allow:

- `rm -rf /*`, `git reset --hard *`, force push на main, `--no-verify`, `--no-gpg-sign`.
- `DROP TABLE`, `DROP DATABASE`, `TRUNCATE flow_orders|hedgeflows|ledger_*`.
- `docker compose down -v`, `docker volume prune`.

### 7.4. Coverage gate

Перед мёрджем должно быть отмечено `covered` в [docs/traceability/coverage-matrix.md](../traceability/coverage-matrix.md) и проставлены ссылки на код в [docs/traceability/feature-traceability.md](../traceability/feature-traceability.md) (колонка `Code`).

#### 7.4.1. Определение "covered" (AUDIT-001 T-AUDIT-010)

Понятие `covered` разбивается на три независимых tier'а — каждый tier проверяется отдельно, фича считается полностью "covered" только когда все три зелёные.

**`covered-docs`** — документация цепочки полна:

- [ ] `docs/02-system/features/F-XX/feature.yaml` существует.
- [ ] `docs/02-system/features/F-XX/README.md` существует.
- [ ] `docs/02-system/features/F-XX/acceptance-criteria.md` существует и непустой.
- [ ] Хотя бы один `docs/02-system/use-cases/UC-FXX-*/use-case.md`.
- [ ] System-level sequence в `docs/02-system/use-cases/UC-FXX-*/sequences/SEQ-UC-FXX-*-system.md`.
- [ ] Service-level sequence в `docs/05-components/sequences/SEQ-FXX-UC-FXX-*-services.md`.
- [ ] Каждая стрелка service sequence имеет backing contract в `docs/06-api/` (нет `TODO contract` без файла).
- [ ] Данные, упомянутые в service sequence, имеют schema в `docs/07-data/`.

**`covered-code`** — реализация присутствует:

- [ ] `feature.yaml.codePaths` все существуют.
- [ ] `specs/domain/feature-component-map.yaml.F-XX.codePaths` существуют и согласованы (T-AUDIT-006 проверит).
- [ ] `docs/implementation-plan/F-XX-*.tasks.md` существует и все pre-conditions `[x]`.
- [ ] Unit-tests в `cpp/<service>/tests/` существуют (`*_test.cpp` файл с покрытием хотя бы main code path).
- [ ] Нет `TODO contract` файлов, связанных с этой фичой.
- [ ] `python3 tools/traceability-checker/check.py` не показывает ошибок про эту фичу.

**`covered-runtime`** — стек запускается и работает:

- [ ] `cd infra && docker compose -f docker-compose.dev.yml up --build <service>` — сервис стартует без ошибок.
- [ ] Все Kafka topics из `feature.yaml.kafkaTopics` созданы (`infra/kafka/create_topics.sh` создаёт их).
- [ ] PostgreSQL migrations применяются (если фича добавляла таблицы).
- [ ] Smoke-test проходит (E2E скрипт из `Testing/` или curl против gateway).
- [ ] Метрики из `docs/08-infrastructure/observability.md` для фичи видны в Prometheus/dashboards (если фича вводит новые).

#### 7.4.2. Маппинг на текущую coverage-matrix

Существующие статусы `complete` / `partial` / `needs-*` в [coverage-matrix.md](../traceability/coverage-matrix.md) — это **агрегированная** проекция трёх tier'ов:

| Tier State | docs | code | runtime | Aggregate Status |
| --- | --- | --- | --- | --- |
| Все ✅ | ✅ | ✅ | ✅ | `complete` |
| Только docs | ✅ | ❌ | ❌ | `partial` / `needs-code` |
| Docs + code, нет runtime | ✅ | ✅ | ⚠️ / ❌ | `needs-runtime` / `in-progress` |
| Docs неполны | ⚠️ | ❌ | ❌ | `needs-contracts` / `needs-data` / `needs-sequences` |
| Tests gap | ✅ | ⚠️ | — | `needs-tests` |

#### 7.4.3. Когда фича попадает в release

`release-ready` ≡ `covered-docs ∧ covered-code ∧ covered-runtime` И прошедшие code-review + security-review. До этого момента фича остаётся `in-progress-impl` или `planned`.

---

## 8. Жизненный пример: F-09 (batch / combo / multi-leg orders)

Текущий "in-flight" пример, на котором видна вся механика:

1. **Auto-archive** — `incoming-docs/2026-06-05-F-09-batch-combo-orders-v2.md` положил [tools/auto-archive-attachments.py](../../tools/auto-archive-attachments.py).
2. **Регистрация IN-011** — [incoming-docs/IN-011.meta.md](../../incoming-docs/IN-011.meta.md) + [IN-011.fragment-map.md](../../incoming-docs/IN-011.fragment-map.md) (skill `ingest-docs`).
3. **Сэндвичи по уровням 1–4** (статус из [§8 интеграции](ingest-and-agents-integration.md)):
   - ADR-031/032/033 в [docs/03-architecture/adr/](../03-architecture/adr/) (`03-solution-architect`).
   - [feature.yaml](../02-system/features/F-09-batch-combo-orders/feature.yaml), [README.md](../02-system/features/F-09-batch-combo-orders/README.md), [acceptance-criteria.md](../02-system/features/F-09-batch-combo-orders/acceptance-criteria.md) (`02-system-analyst`).
   - 3 use cases: [UC-F09-01](../02-system/use-cases/UC-F09-01-create-combo-order/), UC-F09-02, UC-F09-03.
4. **Оставшиеся уровни 5–11** перечислены в [§8 интеграции](ingest-and-agents-integration.md) с указанием специалиста и точки выхода:
   - Уровень 5 → `SEQ-F09-UC-F09-0{1,2,3}-services.md` (system-analyst).
   - Уровень 6 → grpc combo + messaging execution-groups + topics.md + create_topics.sh (proto-contract-designer + devops).
   - Уровень 7 → oltp/olap combo_*/grouped_* (data-schema-designer).
   - Уровень 10 → [F-09-test-plan.md](../10-testing/features/F-09-test-plan.md) (test-architect).
   - Уровень 11 → [F-09-batch-combo-orders.tasks.md](../implementation-plan/F-09-batch-combo-orders.tasks.md) (implementation-planner).
5. **Traceability**: статус F-09 в строке таблицы [feature-traceability.md](../traceability/feature-traceability.md) — UC, sequences, contracts, data objects уже зафиксированы; колонка Code = `planned`.
6. **Implementation** — пока не разрешён (нет полной цепочки 5–11), реализация будет начинаться только когда все pre-conditions в [F-09-batch-combo-orders.tasks.md](../implementation-plan/F-09-batch-combo-orders.tasks.md) станут `✓`.

---

## 9. Жёсткие инварианты процесса (свод)

| Запрет | Источник правила |
| --- | --- |
| Менять файл в `incoming-docs/` после архивации | [CLAUDE.md §0a](../../CLAUDE.md), [ingest-docs SKILL §Source Archive Rule](../../.claude/skills/ingest-docs/SKILL.md) |
| Писать код до полной цепочки docs | [CLAUDE.md §0a "No-code-before-docs"](../../CLAUDE.md), [implementation-plan/README §Pre-conditions](../implementation-plan/README.md) |
| Прямые правки `docs/` специалистами | [intg §3, §7](ingest-and-agents-integration.md) — только `ingress(close)` |
| Молчаливый выбор при конфликте | [CLAUDE.md §0a "Conflict rule"](../../CLAUDE.md) — `Conflict Notes` или ADR |
| Граф/flowchart вместо `sequenceDiagram` | [CLAUDE.md §0a "Mermaid rule"](../../CLAUDE.md), [sequence-diagram-rules.md](sequence-diagram-rules.md) |
| Unlinked имя контракта в seq diagram | [CLAUDE.md §0a "Contract rule"](../../CLAUDE.md) — `TODO contract` файл |
| `double` для money в ledger/risk/matching settlement | [CLAUDE.md §9](../../CLAUDE.md) |
| Правка generated `.pb.cc/.pb.h` | [CLAUDE.md §12.4](../../CLAUDE.md) |
| Force-push на main, `--no-verify`, `DROP TABLE` | [.claude/settings.json `permissions.deny`](../../.claude/settings.json) |
| Новый Kafka топик без `create_topics.sh` + `docs/06-api/messaging/` | [CLAUDE.md §7.3](../../CLAUDE.md), [intg §7](ingest-and-agents-integration.md) |
| ADR без context/decision/alternatives/consequences/reversibility | [CLAUDE.md §3.3](../../CLAUDE.md) |

---

## 10. Точки входа для команды

| Хочу… | Команда / Skill / Агент |
| --- | --- |
| Принять новый документ | положить в чат как вложение → `Skill ingest-docs` |
| Узнать текущий этап F-XX | `Agent 00-sdlc-coordinator` |
| Создать скелет новой фичи | `Skill create-feature` |
| Добавить Kafka топик | `Skill register-kafka-topic` |
| Добавить PG таблицу | `Skill register-pg-table` |
| Пересобрать сервис | `Skill rebuild-service` |
| Запустить implementation task | `Agent 11-code-implementer` (один `T-FXX-NNN` за раз) |
| Code review | `Agent 12-code-reviewer` или `/code-review` |
| Security review | `Agent 15-security-reviewer` или `/security-review` |
| Проверить traceability | `python3 tools/traceability-checker/check.py` |

---

**Резюме одной фразой**: документ из чата автоматически попадает в `incoming-docs/`, регистрируется skill'ом `ingest-docs` как `IN-NNN`, дальше каждый уровень SDLC проходит сэндвич `ingress → специалист → ingress` под управлением `sdlc-coordinator`, по окончании всех 10 уровней `implementation-planner` собирает `F-XX-*.tasks.md`, по которому `code-implementer` пишет код, а `code-reviewer` + `security-reviewer` его проверяют — и только тогда фича считается реализованной согласно правилам [CLAUDE.md](../../CLAUDE.md) и [docs/00-methodology/ingest-and-agents-integration.md](ingest-and-agents-integration.md).
