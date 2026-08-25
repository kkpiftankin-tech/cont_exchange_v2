# Специализированные агенты проекта (SDLC subagents)

Проект использует набор **специализированных субагентов** Claude Code, каждый из которых
покрывает свой этап docs-as-code конвейера
(Business Requirement → Feature → Use Case → Sequences → Contracts → Data → Components →
Tests → Code → Review). Агенты вызываются через инструмент **Agent** (`subagent_type`);
координацию between ними ведёт `sdlc-coordinator`.

> **Где заданы.** Агенты предоставляются на уровне сессии/SDK (frontmatter
> `.claude/agents/*.md` или `agents` в SDK), а не хранятся как файлы в этом репозитории.
> Этот документ — **канонический реестр-описание** для команды. Доступность конкретного
> агента может отличаться от сессии к сессии; актуальный список — в системном сообщении
> «Available agent types».

## Карта: этап SDLC → агент

| Этап (docs-as-code) | Агент | Артефакты на выходе |
|---|---|---|
| 1. Бизнес-требования | `business-analyst` | business requirements, goals, users, feature candidates |
| 2. Системный анализ | `system-analyst` | feature.yaml, use cases, L0/L1 sequence diagrams, FR/NFR, acceptance criteria |
| 3. Архитектура | `solution-architect` | C4 (C1/C2), границы сервисов, ADR, integration/deployment views |
| 3a. Домен | `trading-domain-specialist` | FOB/CSLO matching, risk/ledger/hedge/venue правила, инварианты, формулы, тест-сценарии |
| 4. Контракты | `proto-contract-designer` | protobuf (gRPC + Kafka envelopes), `docs/06-api/`, регистрация топиков, backward-compat |
| 5. Данные | `data-schema-designer` | PostgreSQL OLTP + ClickHouse OLAP схемы, миграции, data-flow |
| 6. Внутренняя архитектура сервиса | `cpp-service-architect` | слои C++ (transport/app/domain/infra), gRPC-хендлеры, порты, Kafka-обвязка |
| 7. Frontend | `frontend-architect` | страницы/компоненты, customer vs ops chrome, API-usage, состояния, UI acceptance |
| 8. Инфраструктура | `devops-engineer` | Docker Compose, Dockerfile, env, Kafka topic init, health checks, CI, deploy |
| 9. План реализации | `implementation-planner` | задачи `PR-FXX-NNN` / `T-FXX-NNN`, привязанные к файлам/тестам/AC |
| 10. Тесты | `test-architect` | unit/integration/E2E/contract/replay/SLA тест-планы |
| 11. Реализация | `code-implementer` | реализация **одной** задачи `T-FXX-NNN` с тестами (в worktree-изоляции) |
| 12. Ревью (качество) | `code-reviewer` | ревью корректности/слоёв/traceability/money-инвариантов/тестов (read-only) |
| 12a. Ревью (безопасность) | `security-reviewer` | auth/authz/secrets/replay/audit/KYC/operator-safety (read-only) |
| Ингест документации | `docs-ingestion-engineer` | регистрация IN-NNN → segment → classify → map → normalize (навык `ingest-docs`) |
| Координация | `sdlc-coordinator` | решает, какой специалист что производит для фичи F-XX |

## Описание агентов

### Проектные (SDLC)

- **business-analyst** — старт фичи: сырые заметки/`IN-NNN` → структурированные бизнес-требования, цели, целевые пользователи, кандидаты-фичи. Не пишет код/тех-спеки. Tools: Read, Grep, Glob.
- **system-analyst** — после бизнес-требований: детальные `feature.yaml`, use cases, system-level (L0) и service-level (L1) sequence-диаграммы (Mermaid), FR/NFR, acceptance criteria. Tools: Read, Grep, Glob.
- **solution-architect** — C4-архитектура, границы сервисов, ADR, integration map, deployment views из утверждённых фич/use-case. Не пишет код/proto. Tools: Read, Grep, Glob.
- **trading-domain-specialist** — доменное ядро: FOB/CSLO matching, risk-политики, ledger-инварианты, hedge-триггеры, venue-правила (F-04/F-06/F-07/F-08/F-09/F-11/F-12). Производит спеки, инварианты, математику, тест-сценарии — не код. Tools: Read, Grep, Glob.
- **proto-contract-designer** — protobuf-сообщения (gRPC-сервисы + Kafka-envelope) в `contracts/proto/fob/`, распространение в `docs/06-api/`, регистрация топиков в `create_topics.sh`, backward-compat до реализации. Tools: Read, Grep, Glob.
- **data-schema-designer** — PostgreSQL OLTP (`infra/postgres/init.sql`) + ClickHouse OLAP схемы, миграции, data-flow (`flow_orders`, `fills`, `batchresults`, `execution_reports`, …). Tools: Read, Grep, Glob.
- **cpp-service-architect** — внутренняя архитектура C++ сервисов (transport/app/domain/infra), структура gRPC-хендлеров, интерфейсы-репозитории, Kafka producer/consumer wiring для `gateway`/`order_flow`/`matching`/`risk`/`ledger`/`market_data`/`venues`/`observability`. Не пишет код. Tools: Read, Grep, Glob.
- **frontend-architect** — страницы/компоненты, customer vs ops chrome, API-usage, loading/error/empty состояния, UI-acceptance. Не пишет JS/JSX/CSS. Tools: Read, Grep, Glob.
- **devops-engineer** — Docker Compose (`infra/docker-compose.dev.yml`, `Testing/*-override.yml`), Dockerfile, env (`infra/env/.env-example`), Kafka topic init, health checks, build/CI, deploy. **Может** править infra-файлы. Tools: Read, Grep, Glob, Edit, Write, Bash.
- **implementation-planner** — после того как есть feature.yaml + use cases + sequences + proto + data + frontend + backend + test plans: создаёт мелкие задачи `PR-FXX-NNN`, привязанные к файлам/тестам/AC. Не пишет код. Tools: Read, Grep, Glob.
- **code-implementer** — реализует **одну** узко-скоупленную задачу `T-FXX-NNN` из `docs/implementation-plan/` с тестами, следуя docs/proto/схемам. Работает в **worktree-изоляции**. Tools: Read, Grep, Glob, Edit, Write, Bash.
- **code-reviewer** — после изменений кода (diff/`PR-FXX-NNN`): корректность, слои, traceability к docs, тесты, безопасность, money-инварианты (Decimal vs double), Kafka/proto backward-compat, правила проекта. Предпочтительно read-only (не Edit/Write). Tools: Read, Grep, Glob, Bash.
- **security-reviewer** — auth/authz, secret handling, replay-safety, полнота audit trail, money-инварианты, dangerous tools, KYC/AML, operator-control safety. Read-only. Tools: Read, Grep, Glob, Bash.
- **docs-ingestion-engineer** — обрабатывает `incoming-docs/YYYY-MM-DD-*.md`: регистрация `IN-NNN` → segment → classify → map → normalize → insert → link → traceability (навык [`ingest-docs`](../../.claude/skills/ingest-docs/SKILL.md)). Tools: Read, Grep, Glob, Edit, Write.
- **sdlc-coordinator** — координирует полный SDLC фичи F-XX: решает, какой специалист производит business-анализ, system-анализ, архитектуру, proto, данные, frontend, backend, тесты, план, код, ревью. Сам не пишет код/docs. Tools: Read, Grep, Glob.

### Служебные (встроенные)

- **general-purpose** — универсальный агент для сложных многошаговых задач и поиска, когда не подходит специализированный. Tools: все.
- **Explore** — read-only fan-out поиск по многим файлам/директориям (возвращает вывод, не аудит). Tools: все, кроме Agent/Edit/Write.
- **Plan** — архитектор планов реализации (пошаговые планы, критические файлы, trade-offs). Tools: все, кроме Agent/Edit/Write.
- **claude** / **claude-code-guide** / **statusline-setup** — общий catch-all, справка по Claude Code/SDK/API, настройка статус-строки.

## Как вызывать

```text
Agent(subagent_type="system-analyst", description="...", prompt="...")
```

- Для последовательности этапов одной фичи — начинать с `sdlc-coordinator` (он распределит), либо вызывать специалистов по порядку карты выше.
- Правила: **docs перед кодом** (§0a CLAUDE.md) — сначала `business-analyst`/`system-analyst`/`solution-architect`/`proto-contract-designer`/`data-schema-designer`/`test-architect`/`implementation-planner`, и только затем `code-implementer`.
- `code-implementer` запускать **только** когда существует задача `T-FXX-NNN`.
- Ревью (`code-reviewer`, `security-reviewer`) — после изменений кода, до merge.

## Связанные документы

- Docs-as-code workflow и traceability chain — [CLAUDE.md §0a](../../CLAUDE.md).
- Ингест входящих документов — [document-ingestion.md](document-ingestion.md), навык [`ingest-docs`](../../.claude/skills/ingest-docs/SKILL.md).
- Шаблоны артефактов — [artifact-templates.md](artifact-templates.md).
- Карта репозитория — [repository-map.md](repository-map.md).
