# Интеграция проектного ingest и SDLC-агентов

> Назначение: связать **проектный ingest-процесс** (`incoming-docs/` + skill
> `ingest-docs` + агент `docs-ingestion-engineer` #13) и **SDLC-агентов**
> (`sdlc-coordinator` #00 + специалисты #01–#15) в **единый поток**, где они
> дополняют, а не дублируют друг друга.
>
> Статус: canonical. Связано с [CLAUDE.md §0a](../../CLAUDE.md),
> [.claude/skills/ingest-docs/SKILL.md](../../.claude/skills/ingest-docs/SKILL.md),
> [ADR-018 (design traceability chain)](../03-architecture/adr/ADR-018-design-traceability-chain.md),
> [ADR-029 (LLM-assisted development governance)](../03-architecture/adr/ADR-029-llm-assisted-development-governance.md).

## 1. Корень противоречия

| Процесс | Что заявляет | Риск |
| --- | --- | --- |
| `ingest-docs` skill / `docs-ingestion-engineer` #13 | DoD: один агент делает **всё** — register → … → feature.yaml, use cases, sequences, контракты, схемы, ADR, tasks, traceability. | Авторствует артефакты, которые по SDLC принадлежат специалистам. |
| `sdlc-coordinator` #00 | 14-этапный цикл; **каждый** артефакт пишет свой специалист (#01–#10). | Параллельный запуск с ingest — оба пишут одни файлы. |

## 2. Разделение ответственности

- **Проектный ingress = связность и исключение противоречий *между уровнями*.**
  Это сквозной интегратор: приём (`incoming-docs/`), регистрация IN-NNN,
  сегментация, классификация, placement, **traceability**, bidirectional links,
  `Conflict Notes`/ADR, coverage gate. Он держит «общую картину» и следит, чтобы
  уровни (business → system → domain → contracts → data → tests → tasks) не
  противоречили друг другу.
- **Агенты = специфика конкретной задачи *внутри уровня*.** Глубокое содержание
  своего класса (домен-математика, proto-совместимость, схемы БД, перф-тесты),
  с чтением **первичного источника** `incoming-docs/<source>`.

## 3. Базовая единица: сэндвич на каждом уровне

Для **каждого уровня** жизненного цикла выполняется цикл из трёх шагов:

```text
┌─ ingress(open)  ─ проектный ingest открывает уровень
│     • что и где на этом уровне (placement по matrix §5)
│     • входная связность: какие артефакты предыдущих уровней он обязан
│       уважать; какие IN-NNN-фрагменты сюда относятся
│     • создаёт скелет/контекст и фиксирует ожидаемые связи
│
├─ agent(refine) ─ профильный специалист дорабатывает специфику
│     • авторствует содержание уровня по матрице владения (§5)
│     • читает incoming-docs/<source>, не выдумывает
│     • возвращает нормализованный черновик (advisory)
│
└─ ingress(close) ─ проектный ingest закрывает уровень
      • размещает содержание (writer-of-record для docs/)
      • bidirectional links + traceability обновлены
      • повторная проверка связности и исключение противоречий
        (Conflict Notes / ADR при расхождении уровней)
      • уровень помечается covered в coverage-matrix
```

Только после `ingress(close)` уровня переходим к `ingress(open)` следующего.
Так связность поддерживается **инкрементально**, а не «в конце».

## 4. Применение сэндвича к уровням жизненного цикла

Порядок уровней — из [sdlc-coordinator](../../.claude/agents/00-sdlc-coordinator.md)
и 11-folder layout. На каждом — сэндвич §3.

| # | Уровень | agent(refine) | ingress пишет в |
| --- | --- | --- | --- |
| 1 | Business (vision/goals/glossary) | business-analyst #01 | `docs/01-business/` |
| 2 | System (feature.yaml, UC, FR/NFR, system seq) | system-analyst #02 | `docs/02-system/` |
| 3 | Architecture / ADR | solution-architect #03 | `docs/03-architecture/` |
| 4 | Domain (entities, events, rules, math) | trading-domain-specialist #06 | `docs/04-domain/` |
| 5 | Service sequences / components | system-analyst #02 / cpp-service-architect #08 | `docs/05-components/` |
| 6 | Contracts (gRPC/REST/Kafka) | proto-contract-designer #04 | `docs/06-api/` (+ devops #14 для `infra/kafka/`) |
| 7 | Data (OLTP/OLAP/flow) | data-schema-designer #05 | `docs/07-data/` (+ devops #14 для `infra/postgres/`) |
| 8 | Frontend Scope | frontend-architect #07 | feature.yaml `frontend` |
| 9 | Backend layering plan | cpp-service-architect #08 | `docs/09-implementation/` |
| 10 | Tests | test-architect #09 | `docs/10-testing/` |
| 11 | Implementation tasks | implementation-planner #10 | `docs/implementation-plan/` |
| 12 | Implementation | code-implementer #11 / devops #14 | `cpp/`, `frontend/`, `infra/` |
| 13 | Review / quality gate | code-reviewer #12 / security-reviewer #15 | read-only |

`sdlc-coordinator` #00 — навигатор: на каждом уровне отвечает «текущий уровень /
чего не хватает / какого специалиста звать / с каким prompt / какие @-файлы».

## 5. Матрица владения (classification → owner agent)

«Owner Agent» авторствует содержание; **запись в `docs/`** делает
`docs-ingestion-engineer` (writer-of-record), в `infra/` — `devops-engineer`.
Специалисты #01–#10 — advisory/read-only.

| Classification | Owner agent (content) | Уровень §4 |
| --- | --- | --- |
| `GLOSSARY`, `BUSINESS_REQUIREMENT` | business-analyst #01 | 1 |
| `FEATURE` | #01 (candidate) → system-analyst #02 (formalize) | 1→2 |
| `FUNCTIONAL_/NON_FUNCTIONAL_REQUIREMENT`, `ACTOR`, `EXTERNAL_SYSTEM` | system-analyst #02 | 2 |
| `USE_CASE`, `SYSTEM_SEQUENCE_HINT` | system-analyst #02 | 2 |
| `CONTAINER`, `COMPONENT`, architectural conflict → ADR | solution-architect #03 (+ cpp-service-architect #08) | 3/5 |
| `DOMAIN_ENTITY`, `DOMAIN_EVENT`, `BUSINESS_RULE`, domain-math | trading-domain-specialist #06 | 4 |
| `SERVICE_SEQUENCE_HINT`, `FID_CHAIN` | system-analyst #02 / cpp-service-architect #08 | 5 |
| `CONTRACT_HINT`, `REST_/GRPC_CONTRACT`, `KAFKA_TOPIC` | proto-contract-designer #04 | 6 |
| `DATA_MODEL`, `DATA_FLOW` | data-schema-designer #05 | 7 |
| (user-facing FEATURE → Frontend Scope) | frontend-architect #07 | 8 |
| `TEST_REQUIREMENT` | test-architect #09 | 10 |
| `OPERATIONAL_REQUIREMENT` | devops-engineer #14 (**сам пишет** `infra/`) | 6/7/12 |
| `IMPLEMENTATION_HINT` | implementation-planner #10 | 11 |
| `METHODOLOGY`, `REPO_STRUCTURE` | human / coordinator | 0 |

`fragment-map` дополняется колонкой **Owner Agent**, чтобы связь
classification → специалист была явной и машиночитаемой (вход для `ingress(open)`
каждого уровня).

## 6. Оркестрация в текущем харнессе

- Субагент не спавнит субагентов. Оркестратор — **главный цикл** (ассистент) или
  явный `Workflow` (один уровень вложенности).
- На каждом уровне главный цикл: вызывает `docs-ingestion-engineer` для
  `ingress(open)` → профильного специалиста для `agent(refine)` →
  `docs-ingestion-engineer` для `ingress(close)`.
- Лёгкий режим: для простого фрагмента ingestion-engineer может пройти весь
  сэндвич сам, встраивая профильное знание; специалист обязателен при наличии
  формул/контрактов/инвариантов/перф-порогов (критерий — `severity` фрагмента).

## 7. Правила недопущения конфликта

- Один файл `docs/` — один писатель за проход (ingestion-engineer).
- Специалисты не редактируют `docs/`/`contracts/`/`cpp/` (plan-mode).
- Расхождение источника и docs → `Conflict Notes`; архитектурное → ADR
  (CLAUDE.md §0a). Не молчаливый выбор.
- Код — только после `docs/implementation-plan/F-XX-*.tasks.md` (ADR-007).
- Новый Kafka-топик — только с `infra/kafka/create_topics.sh` +
  `docs/06-api/messaging/` (proto #04 + devops #14).

## 8. Текущее состояние IN-011 / F-09

Уровни 1–4 пройдены (intake + ADR + feature/AC + use cases + system sequences;
частично в «лёгком режиме» главного цикла). Остаток — по сэндвичу §3:

| Уровень | agent(refine) | ingress(close) пишет |
| --- | --- | --- |
| 5 service sequences | system-analyst #02 | `SEQ-F09-UC-F09-0{1,2,3}-services` |
| 6 contracts | proto-contract-designer #04 | grpc combo, messaging execution-groups (+ topics.md, create_topics.sh → #14) |
| 7 data | data-schema-designer #05 | oltp/olap combo_*/grouped_* |
| 4 domain (доп.) | trading-domain-specialist #06 | entities + business-rules §F-09 |
| 10 tests | test-architect #09 | F-09 test plan |
| 11 tasks | implementation-planner #10 | F-09-batch-combo-orders.tasks.md |
| — traceability | ingress | source-to-artifact, coverage-matrix, feature-traceability |
