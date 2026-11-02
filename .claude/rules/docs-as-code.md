---
paths: ["docs/**", "incoming-docs/**", "specs/**"]
---

# Docs-as-Code Workflow (перенесено из CLAUDE.md §0a, §0b, §0c, §5, §11.2)

Репозиторий — docs-as-code и contract-first. Документация — источник истины;
код может быть сгенерирован только после того, как существует полная цепочка
документации.

## Required traceability chain

Business Requirement → Feature → Use Case → System Sequence → Service
Sequence → Contracts → Data Objects → Components → Tests → Code.

## No-code-before-docs rule

Не реализуй код фичи, пока не существуют все из:

- feature document в `docs/02-system/features/`;
- use case document в `docs/02-system/use-cases/{UC-ID}/use-case.md`;
- system-level sequence diagram в `docs/02-system/use-cases/{UC-ID}/sequences/`;
- service-level sequence diagram в `docs/05-components/sequences/`;
- contract binding table (в service-level sequence);
- data binding table (в service-level sequence);
- контракты в `docs/06-api/`;
- data objects в `docs/07-data/`;
- acceptance criteria в `feature.yaml` или `docs/10-testing/`.

Если чего-то не хватает, сначала создай/обнови документацию. Генерируй
implementation tasks в `docs/implementation-plan/F-XX-name.tasks.md`, не код.
Это правило — для класса L/XL по `.claude/rules/agent-routing.md`; для S/M
обнови только затронутые документы (см. там же §1).

## Incoming documentation workflow

Когда пользователь присылает новую документацию, никогда не вставляй
исходник напрямую в целевой файл. Используй навык
[`.claude/skills/ingest-docs/SKILL.md`](../skills/ingest-docs/SKILL.md).

Pipeline: register → segment → classify → map → normalize → insert/merge →
link bidirectionally → validate coverage → generate implementation tasks.
Оригиналы в `incoming-docs/` — immutable.

### Auto-archive of chat attachments

`UserPromptSubmit` хук ([tools/auto-archive-attachments.py](../../tools/auto-archive-attachments.py),
зарегистрирован в [.claude/settings.json](../settings.json) — shared,
committed, AUDIT-001 T-AUDIT-004) парсит `<document>` блоки в каждом
пользовательском промте и сохраняет их в
`incoming-docs/YYYY-MM-DD-<slug>-<sha8>.md` **до** того, как ассистент
увидит сообщение. Хук эмиттит `additionalContext` с отчётом, какие файлы
были сохранены. Smoke-тест: `python3 tools/auto-archive-attachments.py --self-test`
(CI запускает на каждый PR).

Поведение ассистента:

- При появлении сообщения с auto-archive-отчётом убедиться, что новые файлы
  в `incoming-docs/` соответствуют тому, что прислал пользователь.
- Сами файлы — immutable архив. Регистрация `IN-NNN` (meta + fragment-map)
  выполняется **только по явному запросу** пользователя через скилл
  `ingest-docs`.
- Dedupe — по sha1 содержимого: повторная отправка того же файла не создаёт
  дубликата.
- Хук никогда не блокирует промт (exit 0 при любой ошибке).

Ограничения: хук работает только с `<document>` блоками (текстовые
аттачи) — изображения и бинарные файлы не обрабатываются; в чат-промте
аттачи приходят как inline-текст, файла на диске нет до срабатывания хука.

## Conflict rule

Если входящий текст противоречит существующим docs:

1. Добавь `Conflict Notes`/`> **Conflict Note**` в затронутый целевой
   документ (см. пример в `docs/04-domain/business-rules.md`, раздел
   HedgePnL, и `docs/04-domain/entities.md`, раздел FlowOrder).
2. Если конфликт архитектурный, создай ADR в `docs/03-architecture/adr/`.
3. Не выбирай молча одну версию.

## Contract rule

Каждая стрелка service-level sequence должна иметь transport (REST / gRPC /
Kafka / SQL / WebSocket / internal), имя контракта/сообщения и целевой
contract-документ в `docs/06-api/` или таблицу в `docs/07-data/`. Отсутствующие
контракты становятся `TODO contract`-файлами, а не безымянными связями.

## Mermaid rule

Для interaction-сценариев используй Mermaid `sequenceDiagram`. Не используй
`graph` / `flowchart` / неформальные стрелки как основной артефакт.
Детальные правила размещения по уровням — `.claude/rules/sequences.md`.

## Repository map (docs/)

- `incoming-docs/` — immutable архив входящих документов. Индекс:
  [`incoming-docs/index.md`](../../incoming-docs/index.md).
- `docs/00-methodology/` — методология, правила репозитория, ingestion
  workflow, шаблоны, аудиты (`audits/`), post-mortems (`postmortems/`).
- `docs/01-business/` — vision, goals, stakeholders, glossary, constraints.
- `docs/02-system/` — системное поведение, actors, requirements, features,
  use cases.
- `docs/03-architecture/` — C4/C1/C2, architecture overview, ADR.
- `docs/04-domain/` — доменные сущности, инварианты, domain events,
  ubiquitous language.
- `docs/05-components/` — сервисы/компоненты, sequence diagrams
  (service-level + internal).
- `docs/06-api/` — REST, gRPC, Kafka/message contracts.
- `docs/07-data/` — PostgreSQL, ClickHouse, data flow, retention.
- `docs/08-infrastructure/` — deployment, configuration, observability, CI/CD.
- `docs/09-implementation/` — implementation notes, shared libs, migration map.
- `docs/10-testing/` — acceptance, unit, integration, E2E, performance tests.
- `docs/11-operations/` — runbooks, incident response, onboarding.
- `docs/traceability/` — source-to-artifact maps и coverage matrices.
- `docs/implementation-plan/` — implementation tasks из завершённой
  документации.

**Conflict Note (нумерация папок).** Bootstrap-инструкции ссылаются на
10-папочный layout (`09-testing`, `10-operations`). Этот репозиторий
использует **11-папочный layout**, утверждённый владельцем проекта
(добавлена `09-implementation`). Не переименовывать папки без ADR.

## Placement по уровням декомпозиции (Cockburn: L0 Kite / L1 Sea / L2 Fish)

Документация имеет **две оси** (IN-013): ось этапов (папки
`01-business`..`11-operations`, когда создаётся) и ось уровней декомпозиции
(насколько глубоко смотрим). Полное описание — двухосевая модель в
[`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../docs/00-methodology/functional-hierarchy-and-decomposition.md).

| Уровень | Артефакт | Путь |
| --- | --- | --- |
| **L0 ☁️** | Feature | `docs/02-system/features/F-XX-*/` |
| **L0 ☁️** | System sequence (actor ↔ [System]) | `docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md` |
| **L1 🌊** | Use case | `docs/02-system/use-cases/{UC-ID}/use-case.md` |
| **L1 🌊** | Service-level sequence (cross-component) | `docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md` |
| **L2 🐟** | Component-internal sequence | `docs/05-components/{component-name}/sequences/SEQ-{COMPONENT}-NNN-{topic}.md` |
| **L3 (ниже Fish)** | Вычисление (формула конкретной величины) | `docs/04-domain/calculations/CALC-{F-XX}-{NAME}.md` |

Features живут только в `docs/02-system/features/`. Не создавай
`docs/05-features/`.

Каждый новый артефакт обязан иметь поле `level:` в frontmatter / feature.yaml
(`kite` / `sea` / `fish`). Существующие артефакты дополняются `level:` при
следующем касании (не backfill всё сразу).

Traceability матрицы по уровням:

- [`docs/traceability/feature-to-uc.md`](../../docs/traceability/feature-to-uc.md) — F-XX → UCs.
- [`docs/traceability/uc-to-sequences.md`](../../docs/traceability/uc-to-sequences.md) — UC → L0/L1/L2 sequences.
- [`docs/traceability/sequence-to-code.md`](../../docs/traceability/sequence-to-code.md) — L2 → `cpp/...`.

## Документарный порядок изменений для фичи

1. `docs/01-business` — зачем фича нужна;
2. `docs/02-system` — что система должна делать;
3. `docs/04-domain` — какие сущности/правила меняются;
4. `docs/05-components` — какие компоненты участвуют;
5. `docs/06-api` и `contracts/proto` — какие интерфейсы меняются;
6. `docs/07-data` — какие таблицы/события/retention меняются;
7. код в `cpp/`;
8. тесты;
9. `docs/11-operations` — runbook/monitoring, если нужно;
10. `CHANGELOG.md`.

Если изменение маленькое и документация уже полностью покрывает его,
достаточно сослаться на существующий документ в комментарии/PR summary
(класс S/M — см. `.claude/rules/agent-routing.md`).
