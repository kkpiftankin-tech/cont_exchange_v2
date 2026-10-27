# Специализированные агенты проекта (SDLC subagents)

Проект использует набор **специализированных субагентов** Claude Code.
Агенты вызываются через инструмент **Agent** (`subagent_type`); какого агента
вызывать и когда — решает `.claude/rules/agent-routing.md` (классы S/M/L/XL +
триггеры эскалации), не фиксированный порядок этапов.

> **Где заданы.** Агенты — файлы `.claude/agents/*.md` в этом репозитории
> (frontmatter `name`/`description`/`tools`/`model`/`maxTurns` + тело —
> системный промпт). Это не внешняя SDK-конфигурация — правка файла меняет
> поведение агента в следующем вызове. Актуальный список для текущей сессии
> — системное сообщение «Available agent types».

## Карта: уровень спецификации (ось Р) → агент

Из [IN-019](../../incoming-docs/IN-019.meta.md) §3.2 (принято как целевая
модель, [ADR-086](../03-architecture/adr/ADR-086-two-axis-reply-routing.md)).
**Это целевая раскладка, не действующий порядок вызова** — ревизия
`description`/тела всех 16 агентов под эту таблицу (явный `Use when`
уровень Р, `Returns` до 30 строк) запланирована на Этап 6 плана IN-019 и
ещё не произведена; сегодня агенты вызываются по триггерам
`.claude/rules/agent-routing.md`, не по номеру Р. Номер агента — его имя,
не позиция на оси: `06` работает на Р4, `07` — на Р8.

| Р | Уровень (левая ветвь — спецификация) | Агент-специалист | Проверка на правой ветви |
|---|---|---|---|
| Р1 | Бизнес-требование | `business-analyst` | приёмка владельцем на экране (Р15) |
| Р2 | Возможность, AC, сценарий | `system-analyst` | приёмочные тесты по AC |
| Р3 | Архитектура | `solution-architect` | архитектурные проверки (границы, зависимости) |
| Р4 | Домен | `trading-domain-specialist` | исполняемые примеры CALC, инвариант-тесты |
| Р5 | Взаимодействие сервисов | `system-analyst`, `cpp-service-architect` | интеграционные и e2e-тесты |
| Р6 | Контракты и DTO | `proto-contract-designer` | `buf breaking`, валидация ответов BFF, контрактные тесты |
| Р7 | Данные | `data-schema-designer` | миграция на копии dev-тома, сверка инвентаря таблиц |
| Р8 | Экран (спецификация) | `frontend-architect` | UI-тесты, соответствие `data-ui-id` карте |
| Р9 | Устройство сервиса | `cpp-service-architect` | проверки слоёв (`domain` не зависит от `infra`) |
| Р10 | Тест-план | `test-architect` | — (проект правой ветви) |
| Р11 | Задачи | `implementation-planner` | — |
| Р12 | Код и unit-тесты | главная сессия, `code-implementer`, `devops-engineer` | unit-тесты |
| Р13 | Ревью | `code-reviewer`, `security-reviewer` | вердикт ревью |
| Р14 | Сборка и стенд | `devops-engineer` (навык `rebuild-service`) | зелёный CI, сервис жив на dev |
| Р15 | Экран (наблюдение) | главная сессия + владелец | значение на экране = пример CALC/AC, подтверждение владельца |

`docs-ingestion-engineer` — не уровень Р, а отдельная роль: только приём и
регистрация входящих документов (`incoming-docs/*.md` → `IN-NNN`), навык
[`ingest-docs`](../../.claude/skills/ingest-docs/SKILL.md).
`sdlc-coordinator` — навигатор анализа, см. ниже.

## Описание агентов

### Проектные

- **business-analyst** — старт фичи: сырые заметки/`IN-NNN` → структурированные бизнес-требования, цели, целевые пользователи, кандидаты-фичи. Не пишет код/тех-спеки. Tools: Read, Grep, Glob.
- **system-analyst** — после бизнес-требований: детальные `feature.yaml`, use cases, system-level (L0) и service-level (L1) sequence-диаграммы (Mermaid), FR/NFR, acceptance criteria. Tools: Read, Grep, Glob.
- **solution-architect** — C4-архитектура, границы сервисов, ADR, integration map, deployment views из утверждённых фич/use-case. Не пишет код/proto. Tools: Read, Grep, Glob.
- **trading-domain-specialist** — доменное ядро: FOB/CSLO matching, risk-политики, ledger-инварианты, hedge-триггеры, venue-правила (F-04/F-06/F-07/F-08/F-09/F-11/F-12). Производит спеки, инварианты, математику, тест-сценарии — не код. Если находка требует решения эксперта/владельца, а не инженерного — формулирует кандидата в `OQ-*` для записи главной сессией (`.claude/rules/agent-routing.md` §3). Tools: Read, Grep, Glob.
- **proto-contract-designer** — protobuf-сообщения (gRPC-сервисы + Kafka-envelope) в `contracts/proto/fob/`, распространение в `docs/06-api/`, регистрация топиков в `create_topics.sh`, backward-compat до реализации. Tools: Read, Grep, Glob.
- **data-schema-designer** — PostgreSQL OLTP (`infra/postgres/init.sql`) + ClickHouse OLAP схемы, миграции, data-flow (`flow_orders`, `fills`, `batchresults`, `execution_reports`, …). Tools: Read, Grep, Glob.
- **cpp-service-architect** — внутренняя архитектура C++ сервисов (transport/app/domain/infra), структура gRPC-хендлеров, интерфейсы-репозитории, Kafka producer/consumer wiring для `gateway`/`order_flow`/`matching`/`risk`/`ledger`/`market_data`/`venues`/`observability`. Не пишет код. Tools: Read, Grep, Glob.
- **frontend-architect** — страницы/компоненты, customer vs ops chrome, API-usage, loading/error/empty состояния, UI-acceptance. Не пишет JS/JSX/CSS. Tools: Read, Grep, Glob.
- **devops-engineer** — Docker Compose (`infra/docker-compose.dev.yml`, `Testing/*-override.yml`), Dockerfile, env (`infra/env/.env-example`), Kafka topic init, health checks, build/CI, deploy. **Может** править infra-файлы. Tools: Read, Grep, Glob, Edit, Write, Bash.
- **implementation-planner** — после того как есть feature.yaml + use cases + sequences + proto + data + frontend + backend + test plans: создаёт мелкие задачи `PR-FXX-NNN`, привязанные к файлам/тестам/AC. Не пишет код. Tools: Read, Grep, Glob.
- **code-implementer** — реализует **одну** узко-скоупленную задачу `T-FXX-NNN` из `docs/implementation-plan/` с тестами, следуя docs/proto/схемам. Работает в **worktree-изоляции**. Tools: Read, Grep, Glob, Edit, Write, Bash.
- **code-reviewer** — после изменений кода (diff/`PR-FXX-NNN`), обязательно для денежного контура и класса L: корректность, слои, traceability к docs, тесты, безопасность, money-инварианты (Decimal vs double), Kafka/proto backward-compat, правила проекта. Находка, не требующая немедленного блока, но заслуживающая трекинга — в `known-gaps.md` или `OQ-*`, не только в ответе. Предпочтительно read-only (не Edit/Write). Tools: Read, Grep, Glob, Bash.
- **security-reviewer** — auth/authz, secret handling, replay-safety, полнота audit trail, money-инварианты, dangerous tools, KYC/AML, operator-control safety. Read-only. Tools: Read, Grep, Glob, Bash.
- **docs-ingestion-engineer** — обрабатывает `incoming-docs/YYYY-MM-DD-*.md`: регистрация `IN-NNN` → segment → classify → map → normalize → insert → link → traceability (навык [`ingest-docs`](../../.claude/skills/ingest-docs/SKILL.md)). Только входящие документы — не writer-of-record для обычных правок `docs/`. Tools: Read, Grep, Glob, Edit, Write.
- **sdlc-coordinator** — навигатор анализа для класса **XL** (новая фича) или по прямой просьбе владельца: читает цепочку от якоря, возвращает таблицу «чего не хватает / какой специалист дальше». Не вызывается автоматически для реплик меньшего класса. Сам не пишет код/docs и не вызывает других агентов (нет инструмента `Agent`). Tools: Read, Grep, Glob.

### Служебные (встроенные)

- **general-purpose** — универсальный агент для сложных многошаговых задач и поиска, когда не подходит специализированный. Tools: все.
- **Explore** — read-only fan-out поиск по многим файлам/директориям (возвращает вывод, не аудит). Tools: все, кроме Agent/Edit/Write.
- **Plan** — архитектор планов реализации (пошаговые планы, критические файлы, trade-offs). Tools: все, кроме Agent/Edit/Write.
- **claude** / **claude-code-guide** / **statusline-setup** — общий catch-all, справка по Claude Code/SDK/API, настройка статус-строки.

## Как вызывать

```text
Agent(subagent_type="system-analyst", description="...", prompt="...")
```

- Класс и триггеры — по [`.claude/rules/agent-routing.md`](../../.claude/rules/agent-routing.md), не по фиксированному порядку этапов: класс S — без агентов; M — 1–2 профильных по триггерам; L — профильные по затронутым уровням + `code-reviewer`; XL — `sdlc-coordinator` + полный проход.
- `code-implementer` запускать **только** когда существует задача `T-FXX-NNN`.
- Ревью (`code-reviewer`, `security-reviewer`) — обязательно при денежном триггере (§2 `agent-routing.md`), иначе — после изменений кода, до merge.

## Связанные документы

- Маршрутизация и триггеры — [`.claude/rules/agent-routing.md`](../../.claude/rules/agent-routing.md).
- Двухосевая модель (ось Ф, ось Р, V-петля) — [IN-019](../../incoming-docs/IN-019.meta.md), [ADR-086](../03-architecture/adr/ADR-086-two-axis-reply-routing.md).
- Docs-as-code workflow и traceability chain — [`.claude/rules/docs-as-code.md`](../../.claude/rules/docs-as-code.md).
- Ингест входящих документов — [ingest-and-agents-integration.md](ingest-and-agents-integration.md) (historical) + навык [`ingest-docs`](../../.claude/skills/ingest-docs/SKILL.md).
- Шаблоны артефактов — [artifact-templates.md](artifact-templates.md).
- Карта репозитория — [repository-map.md](repository-map.md).
