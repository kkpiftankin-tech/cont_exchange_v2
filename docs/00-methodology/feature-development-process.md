# Процесс разработки в `cont_exchange_v2.0`

> Статус: canonical reference. Переписан 2026-10-27 под двухосевую модель
> [ADR-086](../03-architecture/adr/ADR-086-two-axis-reply-routing.md) /
> [IN-019](../../incoming-docs/IN-019.meta.md), которая заменила единый
> 14-уровневый сэндвич `ingress → agent → ingress`
> ([ingest-and-agents-integration.md](ingest-and-agents-integration.md),
> теперь superseded). Связано с
> [CLAUDE.md](../../CLAUDE.md),
> [.claude/rules/agent-routing.md](../../.claude/rules/agent-routing.md),
> [ADR-018 (design traceability chain)](../03-architecture/adr/),
> [ADR-029 (LLM-assisted development governance)](../03-architecture/adr/).
>
> **Как читать этот документ.** Двухосевая модель из IN-019 внедряется
> поэтапно (10 этапов, IN-019 §8), каждый — отдельный PR с подтверждением
> владельца перед началом. На 2026-10-27 принят и исполнен только **Этап 0**.
> Поэтому ниже каждый раздел модели помечен статусом:
> **действует сегодня** (работает прямо сейчас, можно полагаться) или
> **план** (часть целевой модели, механизма ещё нет — указан этап, на
> котором появится). Не путай одно с другим: описывать план как уже
> работающий процесс — ровно та ошибка, которую должен был исправить ADR-086.

---

## 0. Что задаёт правила игры

- [CLAUDE.md](../../CLAUDE.md) — непереговорные правила, приоритет источников, слои сервиса.
- [.claude/rules/docs-as-code.md](../../.claude/rules/docs-as-code.md) — traceability chain, no-code-before-docs, conflict rule, mermaid rule, placement по уровням L0/L1/L2/L3.
- [.claude/rules/agent-routing.md](../../.claude/rules/agent-routing.md) — **действует сегодня**: классы S/M/L/XL, триггеры эскалации, правило «находка агента не остаётся только в ответе».
- [docs/00-methodology/agents.md](agents.md) — реестр агентов; таблица уровней Р (план, см. §2 ниже).
- [docs/00-methodology/artifact-templates.md](artifact-templates.md) — шаблоны feature.yaml, use-case.md, ADR, Open Questions.
- [.claude/settings.json](../../.claude/settings.json) — `permissions.deny`, хуки (`auto-archive-attachments.py`, `post_edit_hint.py`, `money_path_guard.py`, `log_subagents.py`).
- ADR [ADR-018](../03-architecture/adr/) (design traceability chain), [ADR-029](../03-architecture/adr/) (LLM-assisted development governance), [ADR-086](../03-architecture/adr/ADR-086-two-axis-reply-routing.md) (эта модель).

---

## 1. Приём входящих документов — **действует сегодня**

Не изменилось ADR-086 (это была узкая, а не отменённая часть прежней модели):

1. **Auto-archive.** Документ в чат как `<document>`-блок → хук `UserPromptSubmit`
   ([tools/auto-archive-attachments.py](../../tools/auto-archive-attachments.py))
   сохраняет его в [incoming-docs/](../../incoming-docs/) **до** того, как
   ассистент его увидит, дедупликация по SHA1. Бинарные вложения (PDF) и
   вложения вне `<document>`-блока хук не ловит — сохраняются вручную
   (прецеденты IN-013/IN-017/IN-018/IN-019).
2. **Регистрация `IN-NNN`** — навык [ingest-docs](../../.claude/skills/ingest-docs/SKILL.md),
   только по явному запросу: Register → Segment → Classify → Map → Normalize
   → Insert/Merge → Link bidirectionally → Validate coverage → Generate tasks.
   Создаёт `IN-NNN.meta.md` + `IN-NNN.fragment-map.md`, обновляет
   [incoming-docs/index.md](../../incoming-docs/index.md).
3. **Оригиналы иммутабельны** (CLAUDE.md, ingest-docs skill §Source Archive
   Rule). Обновление — новый файл с новой датой/хэшем.
4. **Конфликт с существующими docs** → `Conflict Notes` в целевом документе;
   архитектурный конфликт → ADR. Молчаливый выбор запрещён.

Агент [docs-ingestion-engineer](../../.claude/agents/13-docs-ingestion-engineer.md)
исполняет этот навык. Он **не** writer-of-record для обычных правок `docs/`
вне intake — это и была часть модели, отменённая ADR-086 (§3 ниже).

---

## 2. Маршрутизация реплики — **действует сегодня** (упрощённая версия)

Сегодняшний рабочий механизм — [.claude/rules/agent-routing.md](../../.claude/rules/agent-routing.md):

1. Классифицируй задачу: класс **S** (diff в одну фразу, нормативные
   документы не трогаются) / **M** (меняется поведение внутри фичи) /
   **L** (новый/изменённый контракт, формула, инвариант, ADR) / **XL**
   (новая фича).
2. Триггеры эскалации добавляют агента при любом классе независимо от него
   (деньги → `code-reviewer`, auth → `security-reviewer`, контракты →
   `proto-contract-designer`, схемы БД → `data-schema-designer`, формулы →
   `trading-domain-specialist`, инфра → `devops-engineer`, межсервисные
   границы → `solution-architect`).
3. Находка read-only агента, которая указывает на вопрос эксперту/владельцу
   или на инженерный долг, **обязана** попасть в
   `docs/02-system/features/F-XX/open-questions.md` (шаблон — Open Questions
   в `artifact-templates.md`) или в `docs/00-methodology/known-gaps.md` —
   не оставаться только в ответе агента.
4. Замечания с фронта/CLI — навык `/triage` (`.claude/skills/triage/`),
   читает `docs/feedback/inbox.md`.
5. Docs-first полностью — для L/XL (см. §4 ниже); для S/M — только
   затронутые документы (карточка `CALC-*`, `feature.yaml`,
   `docs/feedback/`).

### План (Этап 5 IN-019): двухосевая модель поверх этого

IN-019 вводит ось Ф (`F-XX → CAP-FXX-NN → UC-FXX-NN → UI-…`) и ось Р
(15 уровней спецификации Р1…Р15, левая ветвь — спецификация и код, правая —
проверка; граф зависимостей, не линия) с V-петлёй: спуск по затронутым
уровням спецификации в топологическом порядке, затем проверка каждого
затронутого уровня. Класс изменения (S/M/L/XL/spike) в целевой модели
**выводится** из множества затронутых уровней Р, а не классифицируется
заранее экспертно, как сейчас. Полная модель, типы реплик (`ask`/`fix`/
`refine`/`change`/`extend`/`new`/`describe`/`spike`), алгоритм в 10 шагов —
IN-019 §3.4–3.6. Это заменит текущий упрощённый порядок **после** Этапа 5
(новый маршрутизатор `tools/route/route.py`, навык `change`, правило
`change-routing.md`). До тех пор триггеры и классы из §2 выше — рабочий
механизм, `agent-routing.md` не удаляется.

---

## 3. Что отменено: единый сэндвич `ingress → agent → ingress`

До 2026-10-27 любое изменение `docs/` формально требовало прохода по
14 уровням с писателем-посредником (`docs-ingestion-engineer`) на каждом.
[ADR-086](../03-architecture/adr/ADR-086-two-axis-reply-routing.md) отменяет
это как обязательный путь — модель описана (исторически) в
[ingest-and-agents-integration.md](ingest-and-agents-integration.md)
(`status: superseded`). Специалисты по-прежнему read-only/plan-mode (кроме
`code-implementer`, `docs-ingestion-engineer`, `devops-engineer`), но пишет
правки в `docs/` главная сессия напрямую, классифицируя задачу по §2, а не
обязательно через writer-of-record на каждом уровне.

---

## 4. Гейт "no code before docs" — **действует сегодня**

### 4.1. Required chain (для L/XL)

Из [.claude/rules/docs-as-code.md](../../.claude/rules/docs-as-code.md):

> Business Requirement → Feature → Use Case → System Sequence → Service Sequence → Contracts → Data Objects → Components → Tests → Code

Этот же чеклист — pre-condition в начале каждого `F-XX-*.tasks.md`.

### 4.2. Traceability

- [docs/traceability/feature-to-uc.md](../traceability/feature-to-uc.md), [uc-to-sequences.md](../traceability/uc-to-sequences.md), [sequence-to-code.md](../traceability/sequence-to-code.md).
- [docs/traceability/expert-review-report.md](../traceability/expert-review-report.md) (генерируется `tools/expert_review_report.py --write`) — сводка открытых `OQ-*`, CALC не `implemented`, ADR в `proposed`, инженерные долги.
- Машиночитаемые: [specs/domain/feature-component-map.yaml](../../specs/domain/feature-component-map.yaml), [specs/domain/code-map.yaml](../../specs/domain/code-map.yaml), [specs/domain/traceability.yaml](../../specs/domain/traceability.yaml), [specs/contracts/proto-map.yaml](../../specs/contracts/proto-map.yaml).

### 4.3. Валидаторы

- `python3 tools/traceability-checker/check.py`
- `python3 tools/proto-contract-auditor/check_proto_map.py`
- `python3 tools/expert_review_report.py --write`
- `python3 tools/agents_report.py --days 7` — калибровка маршрутизации по журналу вызовов.

### 4.4. Mermaid и contract rules

Из [.claude/rules/docs-as-code.md](../../.claude/rules/docs-as-code.md) и
[.claude/rules/sequences.md](../../.claude/rules/sequences.md): interaction
scenarios — только `sequenceDiagram`; каждая стрелка service-level диаграммы
— backing-контракт (REST/gRPC/Kafka/SQL) или `TODO contract`-файл, не
unlinked имя.

### План (Этапы 2–4 IN-019)

- **Реестр** `specs/registry/registry.yaml` (генерируемый) заменит часть
  ручных traceability-markdown — Этап 2.
- **Три класса артефактов** (нормативный/поясняющий/генерируемый) и
  `tools/consistency/check.py` (правила C-01…C-17) — Этап 2. Пересекается с
  текущими валидаторами §4.3 — решение «заменить/обернуть/оставить рядом»
  принимается явно на этом этапе, не молча.
- **Карточка изменения CR** (`docs/changes/CR-FXX-NNN-*.md`) для M/L/XL —
  Этап 4. Пересекается с текущим `docs/feedback/FB-*.md` — решение явно на
  этом этапе.
- **Исполняемые примеры CALC** (не текстовые) и матрица AC → тесты — Этап 4.

---

## 5. Implementation — **действует сегодня**

### 5.1. Code-implementer

[.claude/agents/11-code-implementer.md](../../.claude/agents/11-code-implementer.md)
— реализует **один** `T-FXX-NNN` за раз, worktree-изоляция, write-доступ.
Обязан читать feature docs, proto, data schemas, CLAUDE.md, родительский
task-файл. Commit naming — `PR-FXX-NNN`.

### 5.2. Devops-engineer

[.claude/agents/14-devops-engineer.md](../../.claude/agents/14-devops-engineer.md)
— единственный автор `infra/`; также CI и runbook.

### 5.3. Слои (жёсткое правило)

Из [.claude/rules/cpp.md](../../.claude/rules/cpp.md):

```text
transport/  — HTTP/gRPC handlers, Kafka adapters
app/        — use cases, orchestration
domain/     — entities, value objects, invariants (pure)
infra/      — DB repos, external clients, Kafka wrappers
```

`domain` не знает о gRPC/Kafka/DB. Деньги — только `cex::common::Decimal`,
не `double`/`float`. Generated `.pb.cc`/`.pb.h` править руками запрещено.

### 5.4. Хуки PostToolUse

[.claude/settings.json](../../.claude/settings.json): `scripts/hooks/post_edit_hint.py`
(контекстные напоминания после Edit/Write — rebuild, CALC-карточки, язык
`docs/experiments/`), `.claude/hooks/money_path_guard.py` (Stop-хук,
блокирует завершение хода, если денежный путь изменён, а `code-reviewer` не
запускался).

### 5.5. Rebuild и тестирование

Навык [rebuild-service](../../.claude/skills/rebuild-service/SKILL.md): rsync
на `nik@ubuntu-dev`, `docker compose up -d --build <service>`, проверка
логов и HW counters.

### План (Этап 3 IN-019)

`docs/frontend/ui-map.yaml` (карта экрана, генерируется частично из
`data-ui-id`) и `contracts/openapi/fob/bff/v1/` (нормативный контракт для
путей BFF) — сейчас не существуют; REST BFF документируется вручную в
`docs/06-api/rest/`.

---

## 6. Quality gates — **действует сегодня**

### 6.1. Code review

[.claude/agents/12-code-reviewer.md](../../.claude/agents/12-code-reviewer.md)
— read-only, обязателен для денежного контура (хук `money_path_guard.py`) и
класса L. Проверяет корректность, слои, traceability, тесты, security,
money-инварианты, Kafka/proto backward-compat.

### 6.2. Security review

[.claude/agents/15-security-reviewer.md](../../.claude/agents/15-security-reviewer.md)
— auth/authz, secrets, replay safety, audit trail, KYC/AML, operator safety.

### 6.3. Hard deny rules

[.claude/settings.json](../../.claude/settings.json) `permissions.deny`:
`rm -rf /*`, `git reset --hard *`, force push на main, `--no-verify`,
`--no-gpg-sign`, `DROP TABLE/DATABASE`, `TRUNCATE flow_orders|hedgeflows|ledger_*`,
`docker compose down -v`, `docker volume prune`, `Read` на `infra/.env`/`secrets/**`.

### План (Этап 7 IN-019)

Stop-хук `level_gate.py`: изменения в `codePaths`/`ui-map.component` без CR
или трейлера `Change: S|spike` блокируют ход. CI-гейты: `buf breaking`,
раннер примеров CALC, `consistency/check.py` — блокирующие. Сейчас этих
гейтов нет; `money_path_guard.py` остаётся при любом исходе Этапа 7.

---

## 7. Жёсткие инварианты (свод) — **действуют сегодня**

| Запрет | Источник правила |
| --- | --- |
| Менять файл в `incoming-docs/` после архивации | [.claude/rules/docs-as-code.md](../../.claude/rules/docs-as-code.md), ingest-docs SKILL §Source Archive Rule |
| Писать код до полной цепочки docs (L/XL) | [.claude/rules/docs-as-code.md](../../.claude/rules/docs-as-code.md) "No-code-before-docs" |
| Молчаливый выбор при конфликте | [.claude/rules/docs-as-code.md](../../.claude/rules/docs-as-code.md) "Conflict rule" — `Conflict Notes` или ADR |
| Граф/flowchart вместо `sequenceDiagram` | [.claude/rules/sequences.md](../../.claude/rules/sequences.md) |
| Unlinked имя контракта в seq diagram | "Contract rule" — `TODO contract` файл |
| `double` для money в domain/ledger/risk/matching settlement | [CLAUDE.md §1](../../CLAUDE.md) |
| Правка generated `.pb.cc/.pb.h` | [.claude/rules/contracts.md](../../.claude/rules/contracts.md) |
| Force-push на main, `--no-verify`, `DROP TABLE` | [.claude/settings.json `permissions.deny`](../../.claude/settings.json) |
| Новый Kafka топик без `create_topics.sh` + `docs/06-api/messaging/` | [.claude/rules/contracts.md](../../.claude/rules/contracts.md) |
| ADR без context/decision/alternatives/consequences/reversibility | [CLAUDE.md §1](../../CLAUDE.md) |
| Находка read-only агента остаётся только в ответе чата | [.claude/rules/agent-routing.md §3](../../.claude/rules/agent-routing.md) |
| Денежный путь изменён без ревью | [.claude/hooks/money_path_guard.py](../../.claude/hooks/money_path_guard.py) (Stop-хук) |

---

## 8. Точки входа для команды

| Хочу… | Команда / Skill / Агент |
| --- | --- |
| Принять новый документ | положить в чат как вложение → `Skill ingest-docs` (по явному запросу) |
| Разобрать замечание с фронта | `/triage` (`.claude/skills/triage/`) |
| Прогнать и сформировать выжимку для эксперта-трейдера | `/experiment` (`.claude/skills/experiment/`) |
| Узнать, чего не хватает для новой фичи (класс XL) | `Agent sdlc-coordinator` |
| Создать скелет новой фичи | `Skill create-feature` |
| Добавить Kafka топик / PG таблицу | `Skill register-kafka-topic` / `register-pg-table` |
| Пересобрать сервис | `Skill rebuild-service` |
| Запустить implementation task | `Agent code-implementer` (один `T-FXX-NNN` за раз) |
| Code review / Security review | `Agent code-reviewer` / `security-reviewer`, или `/code-review`, `/security-review` |
| Проверить traceability | `python3 tools/traceability-checker/check.py` |
| Сводка критичной функциональности и вопросов эксперту | `python3 tools/expert_review_report.py --write` |

---

## 9. Историческая иллюстрация (сэндвич-модель, до 2026-10-27)

Старый пример F-09 (как фича проходила 14-уровневый сэндвич) удалён из
этого документа вместе с самой моделью — он описывал процесс, который
больше не действует. Полный текст — в истории git
(`git log -p -- docs/00-methodology/feature-development-process.md`,
версия до коммита, вводящего ADR-086). Фактический статус F-09 на
сегодня — [feature.yaml](../02-system/features/F-09-batch-combo-orders/feature.yaml)
и [F-09-batch-combo-orders.tasks.md](../implementation-plan/F-09-batch-combo-orders.tasks.md),
не этот документ.

---

**Резюме одной фразой**: входящий документ архивируется автоматически и
регистрируется по запросу как `IN-NNN`; обычная реплика (вопрос, замечание,
правка) классифицируется по `.claude/rules/agent-routing.md` (класс S/M/L/XL и
триггеры) и исполняется главной сессией с минимально достаточным набором
агентов — не обязательным проходом по всем уровням; L/XL проходят полную
цепочку docs-first; новая фича (XL) — через `sdlc-coordinator` и
`/create-feature`. Двухосевая модель (ось Ф/Р, V-петля, CR, реестр,
`route.py`) — целевая, внедряется поэтапно по IN-019/ADR-086, с
подтверждением владельца перед каждым этапом.
