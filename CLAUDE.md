# CLAUDE.md — Continuous Exchange / Flow Order Book

## 0. Назначение этого файла

Этот файл — корневой контекст для Claude Code при работе с репозиторием `cont_exchange_v2.0`.
Он задаёт продуктовый смысл, архитектурные границы, правила docs-as-code, стиль разработки и порядок действий при изменении кода или документации.

Работай с проектом как с системой непрерывной биржи заявок, где документация является первичным источником истины, а код является проверяемой реализацией этих документов.

## 0a. Docs-as-Code Workflow (canonical)

Repository is docs-as-code and contract-first. Documentation is the source of truth; code may be generated only after the full documentation chain exists.

### Required traceability chain

Business Requirement → Feature → Use Case → System Sequence → Service Sequence → Contracts → Data Objects → Components → Tests → Code.

### No-code-before-docs rule

Do not implement code for a feature until all of these exist:

- feature document in `docs/02-system/features/`;
- use case document in `docs/02-system/use-cases/{UC-ID}/use-case.md`;
- system-level sequence diagram in `docs/02-system/use-cases/{UC-ID}/sequences/`;
- service-level sequence diagram in `docs/05-components/sequences/`;
- contract binding table (in the service-level sequence);
- data binding table (in the service-level sequence);
- contracts in `docs/06-api/`;
- data objects referenced in `docs/07-data/`;
- acceptance criteria in `feature.yaml` or `docs/10-testing/`.

If anything is missing, create or update documentation first. Generate implementation tasks in `docs/implementation-plan/F-XX-name.tasks.md`, not code.

### Incoming documentation workflow

When the user supplies new documentation, never paste the source into a single target file. Use the skill:

- [`.claude/skills/ingest-docs/SKILL.md`](.claude/skills/ingest-docs/SKILL.md)

Pipeline: register → segment → classify → map → normalize → insert/merge → link bidirectionally → validate coverage → generate implementation tasks. Originals in `incoming-docs/` are immutable.

### Auto-archive of chat attachments

A `UserPromptSubmit` hook ([tools/auto-archive-attachments.py](tools/auto-archive-attachments.py), registered in [.claude/settings.local.json](.claude/settings.local.json)) парсит `<document>` блоки в каждом пользовательском промте и сохраняет их в `incoming-docs/YYYY-MM-DD-<slug>-<sha8>.md` **до** того, как ассистент увидит сообщение. Хук эмиттит `additionalContext` с отчётом, какие файлы были сохранены.

Поведение ассистента:

- При появлении сообщения с auto-archive-отчётом убедиться, что новые файлы в `incoming-docs/` соответствуют тому, что прислал пользователь.
- Сами файлы — immutable архив. Регистрация `IN-NNN` (meta + fragment-map) выполняется **только по явному запросу** пользователя через skill `ingest-docs`.
- Dedupe — по sha1 содержимого: повторная отправка того же файла не создаёт дубликата.
- Хук никогда не блокирует промт (exit 0 при любой ошибке).

Ограничения:

- Хук работает только с `<document>` блоками (текстовые аттачи). Изображения и бинарные файлы не обрабатываются.
- В чат-промте аттачи приходят как inline-текст; файла на диске нет до срабатывания хука.

### Conflict rule

If incoming text contradicts existing docs:

1. Add `Conflict Notes` to the affected target document.
2. If the conflict is architectural, create an ADR in `docs/03-architecture/adr/`.
3. Do not silently choose one version.

### Contract rule

Every service-level sequence arrow must have transport (REST / gRPC / Kafka / SQL / WebSocket / internal), a contract or message name, and a target contract document in `docs/06-api/` or a target table in `docs/07-data/`. Missing contracts become `TODO contract` files, not unlinked names.

### Mermaid rule

For interaction scenarios use Mermaid `sequenceDiagram`. Do not use `graph` / `flowchart` / informal arrows as the primary artifact.

### Current Work Rule

Before modifying files, show a plan. After modifying files, report: created / updated / unresolved TODO contracts / conflicts / coverage status / next recommended tasks (see §27).

## 0b. Stable Repository Map

- `incoming-docs/` — immutable archive of incoming source documents. Index: [`incoming-docs/index.md`](incoming-docs/index.md).
- `docs/00-methodology/` — methodology, repository rules, ingestion workflow, templates.
- `docs/01-business/` — vision, goals, stakeholders, glossary, business constraints.
- `docs/02-system/` — system behavior, actors, requirements, features, use cases.
- `docs/03-architecture/` — C4/C1/C2, architecture overview, ADR.
- `docs/04-domain/` — domain entities, invariants, domain events, ubiquitous language.
- `docs/05-components/` — services/components, sequence diagrams (service-level + internal).
- `docs/06-api/` — REST, gRPC, Kafka/message contracts.
- `docs/07-data/` — PostgreSQL, ClickHouse, data flow, retention.
- `docs/08-infrastructure/` — deployment, configuration, observability, CI/CD.
- `docs/09-implementation/` — implementation notes, shared libs, migration map.
- `docs/10-testing/` — acceptance, unit, integration, E2E, performance tests.
- `docs/11-operations/` — runbooks, incident response, onboarding.
- `docs/traceability/` — source-to-artifact maps and coverage matrices.
- `docs/implementation-plan/` — implementation tasks derived from completed documentation.

**Conflict Note (folder numbering).** The bootstrap instruction text references a 10-folder layout (`09-testing`, `10-operations`). This repository uses an **11-folder layout** approved by the project owner (added `09-implementation`). All paths in this CLAUDE.md and in `docs/00-methodology/` reflect the actual 11-folder layout. Do not rename folders without an ADR.

## 0c. Feature / Use Case / Sequence Placement Rules (summary)

Features live only in `docs/02-system/features/`. Do not create `docs/05-features/`.

Use cases live only in `docs/02-system/use-cases/{UC-ID}/use-case.md`.

System-level sequence diagrams (external actors ↔ Continuous Exchange System as black box) live only in `docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md`.

Service-level cross-component sequence diagrams live only in `docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md`.

Internal component sequence diagrams live only in `docs/05-components/{component-name}/sequences/`.

Detailed rules and required sections — in [`docs/00-methodology/sequence-diagram-rules.md`](docs/00-methodology/sequence-diagram-rules.md) and §26a below.

## 1. Идентичность проекта

**Название проекта:** Continuous Exchange / Flow Order Book.

**Краткое описание:** проект реализует MVP-скелет биржи непрерывных во времени заявок, где торговля моделируется как поток объёма с конечной скоростью, а не как мгновенное исполнение дискретных ордеров.

**Доменная идея:** вместо классического стакана лимитных заявок система оперирует потоковыми заявками и CSLO-кривыми. Участник задаёт диапазон цены, максимальный объём и максимальную скорость исполнения. Matching-сервис регулярно или непрерывно ищет равновесную цену и скорости исполнения.

**Целевой механизм:** Flow Order Book / Continuous Matching.

**Текущий статус репозитория:** C++20 microservices MVP skeleton. Бизнес-логика упрощена и местами симуляционная. Контракты, Kafka-топики и межсервисная хореография уже заложены так, чтобы постепенно заменить симуляторы реальным solver, реальным persistence-слоем и реальными venue-адаптерами.

## 2. Главные цели продукта

Система должна развиваться к следующим целям:

1. Снизить стоимость исполнения крупных и средних сделок за счёт контролируемого профиля скорости, прогнозируемого VWAP и оценки Implementation Shortfall.
2. Дать ликвидным трейдерам возможность задавать потоковые, портфельные и парные заявки с временным окном исполнения.
3. Дать маркет-мейкерам среду для публикации непрерывных кривых ликвидности «цена–скорость» и контроля inventory/PnL.
4. Обеспечить pre-trade и post-trade риск-контроль, маржу, лимиты, ликвидации и kill-switch.
5. Обеспечить внешнюю ликвидность и хеджирование через CEX/DEX/AMM adapters.
6. Обеспечить observability, replay, backtest и audit trail.
7. Поддерживать docs-as-code: любой код должен быть прослеживаем к требованиям, доменной модели, контрактам и тестам.

## 3. Непереговорные правила источников истины

### 3.1. Приоритет источников

При конфликте между артефактами используй следующий порядок:

1. Явное указание пользователя в текущей задаче.
2. `CLAUDE.md`.
3. Документация в `docs/`, особенно ADR и контрактные спецификации.
4. Контракты в `contracts/proto/` и схемы сообщений.
5. Реализация в `cpp/`.
6. `legacy_mvp/` — только справочник и источник reuse-идей, не источник истины для новой архитектуры.

### 3.2. Docs-first

Перед изменением кода проверь, какой документ описывает это изменение:

- бизнес-смысл: `docs/01-business/`;
- системное поведение: `docs/02-system/`;
- архитектура: `docs/03-architecture/`;
- домен: `docs/04-domain/`;
- компонент: `docs/05-components/{component}/`;
- API/контракт: `docs/06-api/` и `contracts/proto/`;
- данные: `docs/07-data/`;
- инфраструктура: `docs/08-infrastructure/`;
- тестирование: `docs/10-testing/` или `docs/09-testing/` в текущей переходной структуре;
- эксплуатация: `docs/11-operations/` или `docs/10-operations/` в текущей переходной структуре.

Если документа нет, создай или обнови его до реализации.

### 3.3. Когда нужен ADR

Создавай ADR в `docs/03-architecture/adr/`, если изменение затрагивает:

- архитектурный стиль;
- границы сервисов;
- выбор брокера, БД, протокола или framework;
- схему топика Kafka;
- публичный gRPC/REST контракт;
- модель денежных расчётов;
- matching/clearing algorithm;
- risk policy, margin policy, liquidation policy;
- безопасность, custody, KYC/AML, audit trail;
- изменение уже закреплённого решения.

ADR должен быть коротким, но обязан содержать: контекст, решение, альтернативы, последствия, обратимость.

## 4. Фактическая структура текущего репозитория

Текущий архив содержит следующий skeleton:

```text
cont_exchange_v2.0/
├── README.md
├── Development.md
├── CMakeLists.txt
├── Makefile
├── contracts/
│   ├── CMakeLists.txt
│   └── proto/fob/
│       ├── agent/v1/agent.proto
│       ├── common/v1/common.proto
│       ├── execution/v1/execution.proto
│       ├── ledger/v1/ledger.proto
│       ├── marketdata/v1/marketdata_raw.proto
│       ├── marketdata/v1/marketdata_service.proto
│       ├── matching/v1/batch.proto
│       ├── observability/v1/observability.proto
│       ├── orders/v1/order_flow_service.proto
│       ├── orders/v1/orders.proto
│       └── risk/v1/risk.proto
├── cpp/
│   ├── common/
│   ├── gateway/
│   ├── order_flow/
│   ├── risk/
│   ├── ledger/
│   ├── matching/
│   ├── market_data/
│   ├── venues/
│   └── observability/
├── infra/
│   ├── docker-compose.dev.yml
│   ├── env/.env-example
│   ├── kafka/create_topics.sh
│   └── k8s/
├── docker/Dockerfile.service
└── legacy_mvp/
```

Целевая docs-as-code структура должна быть добавлена поверх этого skeleton без разрушения текущей структуры исходного кода.

## 5. Целевая структура документации

Желаемая структура описана в файле `ЭТАПЫ.md`. Основная идея:

```text
docs/
├── 01-business/
├── 02-system/
├── 03-architecture/
├── 04-domain/
├── 05-components/
├── 06-api/
├── 07-data/
├── 08-infrastructure/
├── 09-implementation/
├── 10-testing/
└── 11-operations/
```

В переходной структуре допускается вариант из исходного плана: `docs/09-testing/` и `docs/10-operations/`, где реализация представлена через `src/`/`cpp/`. Предпочтительная будущая структура — 11 папок документации, чтобы каждый методологический этап имел свой doc-space.

## 6. Текущие сервисы и ответственность

| Сервис / каталог | Ответственность | Основные входы | Основные выходы |
|---|---|---|---|
| `cpp/gateway` | REST edge gateway, HTTP JSON → gRPC `OrderFlowService` | HTTP `POST /v1/flow-orders`, health | gRPC calls to `order_flow` |
| `cpp/order_flow` | Жизненный цикл FlowOrder, risk check, reserve funds, publish normalized orders | gRPC `CreateFlowOrder`, `CancelFlowOrder`, `GetFlowOrder` | Kafka `orders.normalized`, gRPC calls to risk/ledger |
| `cpp/risk` | Pre-trade checks, kill-switch, risk alerts | gRPC `RiskService` | Kafka `risk.alerts` |
| `cpp/ledger` | Балансы, резервы, применение fills/execution reports | gRPC `LedgerService`, Kafka `batch.outputs`, `execution.reports` | balance state, position state |
| `cpp/matching` | Периодический batch-clearing simulator, placeholder for real solver | Kafka `orders.normalized` | Kafka `batch.outputs` |
| `cpp/market_data` | Last ticker cache and market data read API | Kafka `marketdata.raw` | gRPC `MarketDataService` |
| `cpp/venues` | Симулятор external venue adapter | timer / execution intents | Kafka `marketdata.raw`, `execution.reports` |
| `cpp/observability` | Читает важные топики и пишет структурированные summaries | Kafka `risk.alerts`, `batch.outputs`, `execution.reports` | structured logs |
| `cpp/common` | Общие утилиты: env, log, uuid, time, Decimal/proto helpers, Kafka wrappers | internal use | shared library |

## 7. Контракты и топики

### 7.1. Protobuf packages

Контракты хранятся в `contracts/proto/fob/**/v1/*.proto`. Они компилируются через `contracts/CMakeLists.txt` в C++ library `contracts_proto`.

Не меняй generated files вручную. Меняй `.proto`, затем пересобирай проект.

### 7.2. Основные protobuf-сущности

| Сущность | Файл | Назначение |
|---|---|---|
| `EventMeta` | `common.proto` | event id, timestamp, source, correlation id, trace/span id, partition key, tags |
| `Decimal` | `common.proto` | fixed-point decimal: `value = units * 10^(-scale)` |
| `FlowOrder` | `orders.proto` | потоковая заявка с диапазоном цены и max speed |
| `OrdersNormalized` | `orders.proto` | Kafka envelope для create/cancel/amend |
| `BatchResult` | `batch.proto` | результат clearing cycle: prices, rates, fills, order updates, diagnostics |
| `RiskAlert` | `risk.proto` | событие риска / kill-switch / margin / limit breach |
| `ExecutionIntent` | `execution.proto` | намерение внешнего хеджирования |
| `ExecutionReport` | `execution.proto` | отчёт внешней площадки |
| `AgentLog` | `agent.proto` | state–action–reward log для политик matching/risk/execution |

### 7.3. Kafka / Redpanda topics

Dev topics создаются в `infra/kafka/create_topics.sh`:

| Topic | Producer | Consumer | Назначение |
|---|---|---|---|
| `marketdata.raw` | `venues` | `market_data`, потенциально `matching` | сырые внешние котировки / стаканы / trades |
| `orders.normalized` | `order_flow` | `matching` | нормализованные FlowOrder commands |
| `batch.outputs` | `matching` | `ledger`, `risk`, `observability`, UI stream | clearing result, fills, order updates |
| `execution.intents` | `risk`/`matching`/future execution policy | `venues` | child order / hedge instruction |
| `execution.reports` | `venues` | `ledger`, `observability` | результат внешнего исполнения |
| `risk.alerts` | `risk` | `observability`, operator UI | alert stream |
| `agent.logs` | будущие agent policies | `backtest`, `research`, `observability` | state–action–reward trail |

При добавлении нового топика обнови:

1. `docs/06-api/messaging/topics.md`;
2. schema/proto file;
3. `infra/kafka/create_topics.sh`;
4. producer/consumer code;
5. integration tests;
6. observability docs.

## 8. Доменная модель

### 8.1. Базовые понятия

- `Asset` — торгуемый актив: BTC, ETH, USDT.
- `Instrument` / `Symbol` — торговая пара, например BTC/USDT.
- `Base asset` — первый актив пары, объём сделки выражается в нём.
- `Quote asset` — второй актив пары, цена выражается в нём.
- `Price` — количество quote asset за единицу base asset.
- `Side` — buy или sell.
- `Trading speed` — поток объёма в единицу времени, в base units/sec.
- `Execution window` — интервал исполнения \([t_{\text{start}}, t_{\text{end}}]\).

### 8.2. FlowOrder

`FlowOrder` — центральная бизнес-сущность текущего MVP.

Минимальная параметризация:

- `order_id` — internal UUID;
- `client_order_id` — idempotency key от клиента;
- `user_id`, `account_id`;
- `instrument`;
- `side`;
- `total_qty`;
- `remaining_qty`;
- `price_low`;
- `price_high`;
- `max_speed`;
- `status`;
- `tif`;
- `tags`.

Инварианты:

1. `total_qty > 0`.
2. `remaining_qty >= 0`.
3. `remaining_qty <= total_qty`.
4. `price_low > 0`.
5. `price_high > 0`.
6. `price_low <= price_high`.
7. `max_speed > 0`.
8. `instrument.symbol`, `instrument.base`, `instrument.quote` не пустые.
9. Для `BUY` клиент платит quote и получает base.
10. Для `SELL` клиент отдаёт base и получает quote.
11. Любая команда create/amend/cancel должна быть идемпотентной.
12. Любая команда должна иметь `EventMeta.correlation_id`.

### 8.3. CSLO

Continuous Scaled Limit Order — кривая спроса/предложения «цена–скорость» или «цена–объём».

Типичная параметризация:

- \(P_L\) — нижняя граница цены;
- \(P_H\) — верхняя граница цены;
- \(Q\) — максимальный объём;
- \(U\) — максимальная скорость.

FlowOrder в текущем MVP — минимальная DTO-форма, совместимая с CSLO/FOB моделью.

### 8.4. BatchResult

`BatchResult` — результат одного clearing cycle.

Содержит:

- `batch_id`;
- timestamp;
- `clear_prices`: symbol → clearing price;
- `executed_rates`: order_id → executed rate;
- `fills`: список `FlowFill`;
- `order_updates`;
- `diagnostics`: residual norm, solve time, active orders, solver config version.

Инварианты:

1. Fill не должен превышать `remaining_qty` заявки.
2. Цена fill должна быть в допустимом диапазоне заявки, кроме явно описанных exceptional policies.
3. `batch_id` должен связывать fills, order updates, risk events и agent logs.
4. Любая ошибка solver должна давать диагностируемый результат, а не молчаливое повреждение состояния.

### 8.5. VWAP и IS

Для отчётности используй стандартную формулу VWAP:

\[
\text{VWAP} = \frac{\sum_i p_i q_i}{\sum_i q_i}
\]

Implementation Shortfall должен трактоваться как разница между идеальной стоимостью исполнения по цене принятия решения и фактической стоимостью с учётом spread, temporary impact, permanent impact, fees и volatility.

## 9. Финансовая точность

Запрещено использовать `double`/`float` для денежных величин в доменной логике, ledger, risk, matching settlement или persistence.

Используй protobuf `Decimal` или отдельный fixed-point/value-object слой.

`double` допустим только для:

- solver diagnostics;
- residual norm;
- metrics;
- временных research/simulation calculations, если результат не попадает в ledger.

При конвертации Decimal:

1. явно фиксируй scale;
2. не теряй sign;
3. покрывай unit tests;
4. не смешивай base amount, quote amount и price без типов/явных имен.

## 10. Архитектурный стиль

### 10.1. Общий стиль

Проект — event-driven microservices skeleton с контракт-first gRPC/protobuf и Kafka/Redpanda event bus.

Внутри сервиса придерживайся слоёв:

```text
transport/     HTTP/gRPC handlers, Kafka consumers/producers adapters
app/           use cases, orchestration, application services
domain/        entities, value objects, invariants, pure business rules
infra/         DB repositories, external clients, Kafka wrappers, gRPC clients
```

Текущий C++ MVP местами содержит упрощённую структуру. При развитии сервиса постепенно приводить его к этой схеме, не ломая поведение.

### 10.2. Правила зависимостей

- `domain` не знает о gRPC, Kafka, HTTP, DB, Docker, env.
- `app` знает о domain и интерфейсах портов.
- `transport` маппит внешние DTO в application commands.
- `infra` реализует порты: DB, Kafka, gRPC clients, external venue APIs.
- Общие утилиты живут в `cpp/common` только если они действительно cross-service.
- Не добавляй доменную бизнес-логику в `main.cpp`.

### 10.3. Сервисные границы

Не смешивай ответственность:

- `order_flow` не решает matching;
- `matching` не резервирует средства;
- `ledger` не принимает risk decisions;
- `risk` не мутирует ledger напрямую, кроме явно описанных liquidation/rebalance flows;
- `market_data` не торгует;
- `venues` не принимает бизнес-решения о хеджировании, а исполняет `ExecutionIntent`;
- `observability` не влияет на бизнес-состояние, кроме будущих operator workflows через отдельный control API.

## 11. Development workflow для Claude Code

### 11.1. Перед изменением файлов

Всегда сначала сделай короткий план:

1. какие документы/контракты будут затронуты;
2. какие сервисы будут затронуты;
3. какие инварианты нужно сохранить;
4. какие тесты/команды нужно запустить.

После плана переходи к изменениям.

### 11.2. Документарный порядок изменений

Для фичи используй такой порядок:

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

Если изменение маленькое и документация уже полностью покрывает его, достаточно сослаться на существующий документ в комментарии/PR summary.

### 11.3. Contract-first

При изменении API:

1. обнови `.proto` или OpenAPI/schema;
2. обнови документацию контракта;
3. обнови сервис-реализацию;
4. обнови клиента;
5. обнови тесты;
6. убедись, что старые consumers не ломаются без миграционного плана.

Для breaking changes нужен ADR.

### 11.4. Event-first для async flows

Для Kafka-flow:

1. опиши событие и topic;
2. зафиксируй partition key;
3. зафиксируй delivery semantics;
4. продумай idempotency;
5. продумай replay;
6. добавь producer;
7. добавь consumer;
8. добавь observability;
9. добавь test/replay scenario.

## 12. Правила C++

### 12.1. Версия и сборка

- C++ standard: C++20.
- Build: CMake `>= 3.24`.
- Основная сборка: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.
- Docker dev runtime: `cd infra && docker compose -f docker-compose.dev.yml up --build`.

### 12.2. Стиль

- Чистые доменные функции — максимально deterministic и unit-testable.
- Не использовать глобальное mutable-состояние, кроме явно контролируемых service runtime caches.
- Любой background thread должен иметь понятный lifecycle.
- Для production-изменений добавлять graceful shutdown, если сервис содержит consumer loop.
- Ошибки должны быть typed/stable: `Error.code`, `Error.message`, `Error.details`.
- Логи — structured JSON через общий logging layer.
- Для внешних IDs и idempotency использовать явные поля, не перегружать `order_id`.

### 12.3. Конфигурация

- Не хардкодить адреса сервисов, брокеров, тайминги, лимиты, secrets.
- Использовать env vars через `cex::common::Env`.
- Dev defaults допустимы, если явно безопасны и подходят для docker-compose.
- Prod-секреты никогда не хранить в git.

### 12.4. Generated code

Не редактировать protobuf-generated `.pb.cc`, `.pb.h`, `.grpc.pb.cc`, `.grpc.pb.h`.

Если generated files появляются в build directory, не добавлять их как source-of-truth.

## 13. Правила Kafka/Redpanda

- Kafka record key должен быть выбран осмысленно: `user_id`, `symbol`, `order_id`, `batch_id`, `intent_id`.
- Для financial event stream предпочитай at-least-once + idempotent consumer.
- Consumer должен коммитить offset после успешной обработки.
- Нельзя полагаться на exactly-once без явного проектного решения и тестов.
- Replay должен быть возможен для audit/backtest.
- События не должны содержать secrets.
- Retention должен соответствовать назначению: market data может быть short-retention, audit/risk/fills — long-retention.

## 14. Целевые хранилища данных

Текущий MVP использует in-memory состояние в ряде сервисов. Целевая документация описывает:

### 14.1. PostgreSQL / OLTP

Целевые таблицы:

- `users`;
- `sessions`;
- `accounts`;
- `flow_orders`;
- `positions`;
- `risk_limits`;
- `risk_snapshots`;
- `collateral_transfers`;
- `solver_config`.

PostgreSQL — источник истины для оперативного состояния: пользователи, сессии, счета, активные заявки, позиции, лимиты, маржа, конфиги.

### 14.2. ClickHouse / OLAP

Целевые таблицы:

- `fills`;
- `batch_results`;
- `marketdata`;
- `risk_events`;
- `execution_reports`;
- `agent_logs`.

ClickHouse — источник аналитики, истории, replay, quality-of-execution reports и observability queries.

### 14.3. Persistence changes

При добавлении persistence:

1. опиши схему в `docs/07-data/`;
2. создай migration strategy;
3. добавь репозиторий в `infra/` слоя сервиса;
4. покрой transactional boundaries;
5. добавь integration test с test DB;
6. обнови runbook и backup/retention docs.

## 15. Matching / clearing rules

Текущий `matching` — simulator. Целевой `matching` должен развиваться в FOB Core.

При изменении matching:

1. не ломай contract `BatchResult` без ADR;
2. сохраняй deterministic mode для replay;
3. отделяй solver algorithm от Kafka runtime;
4. добавляй diagnostics;
5. проверяй conservation constraints;
6. учитывай price interval каждой заявки;
7. учитывай speed caps;
8. учитывай risk caps, если они передаются в batch input;
9. сохраняй traceability: order → batch → fill → ledger → risk snapshot.

Минимальные тестовые сценарии для solver:

- один buyer и один seller с пересекающимися диапазонами;
- непересекающиеся диапазоны;
- частичное исполнение из-за speed cap;
- частичное исполнение из-за remaining_qty;
- несколько покупателей/продавцов;
- крайние цены;
- отмена заявки перед batch;
- replay того же batch input даёт тот же `BatchResult`.

## 16. Risk rules

Risk Manager должен сохранять следующие принципы:

- pre-trade checks до активации заявки;
- проверка notional, max position, leverage, order rate, whitelist;
- `RiskDecision` может быть ACCEPT, REJECT, RESIZE, HALT;
- kill-switch может быть global или instrument-specific;
- risk alerts должны быть observable и replayable;
- post-trade risk должен иметь связь с `batch_id`.

Любое изменение risk policy требует:

1. обновления `docs/02-system/non-functional-requirements.md` или `docs/02-system/functional-requirements.md`;
2. обновления `docs/04-domain/business-rules.md`;
3. обновления `docs/07-data/risk-schema.md` или соответствующего файла;
4. тестов на false positive/false negative cases;
5. operator runbook, если меняется kill-switch/liquidation.

## 17. Ledger / settlement rules

Ledger — источник истины по balances/positions.

Запрещено:

- применять fill дважды;
- освобождать резерв без idempotency key;
- смешивать user balance и exchange hedge balance;
- обновлять balances без audit trail;
- делать settlement на основе floating-point money.

Для ledger flows всегда фиксируй:

- reservation id;
- user id;
- order id;
- batch id или execution report id;
- source topic / source service;
- before/after balance snapshot, если это продовый ledger path.

## 18. External venues / execution hedge rules

`venues` сейчас симулятор. Целевой `External Venues Connector` должен:

- получать market data snapshots;
- нормализовать venue symbols;
- учитывать fees, tick size, lot size;
- принимать `ExecutionIntent`;
- отправлять child orders;
- возвращать `ExecutionReport`;
- корректно обрабатывать partial fill, reject, cancel, timeout;
- не принимать самостоятельных trading decisions без policy/intent.

Никогда не добавляй реальные credentials в репозиторий.

## 19. Observability rules

Для каждого нового production flow должны быть:

- correlation id;
- structured logs;
- metrics;
- error codes;
- alert thresholds, если есть риск деградации;
- dashboard/runbook reference.

Минимальные метрики:

- request latency;
- gRPC error rate;
- Kafka produce/consume failures;
- consumer lag;
- batch solve time;
- residual norm;
- number of active orders;
- fill rate;
- rejected/throttled orders;
- ledger apply failures;
- external venue rejection rate.

## 20. Testing policy

### 20.1. Минимальные уровни тестирования

- Unit tests для domain/app logic.
- Contract tests для `.proto` compatibility.
- Integration tests для gRPC service interactions.
- Kafka integration tests для producer/consumer flows.
- E2E tests для ключевых фич.
- Replay tests для deterministic matching.
- Performance tests для solver and critical endpoints.

### 20.2. Основные E2E-фичи

Покрывай фичи:

- F-01 registration/auth — пока в новом skeleton отсутствует полноценная реализация;
- F-02 create FlowOrder;
- F-03 amend/cancel FlowOrder;
- F-04 batch clearing;
- F-05 live market data;
- F-06 positions/PnL/margin;
- F-07 pre-trade risk;
- F-08 post-trade risk/liquidations;
- F-09 portfolio/pair order;
- F-10 market-maker curves;
- F-11 external market data;
- F-12 execution hedge;
- F-13 post-trade report;
- F-14 deposit/withdraw;
- F-15 backtest/replay;
- F-16 operator panel/kill-switch;
- F-17 observability.

Если feature ещё не реализована, документируй статус как `planned` или `stub`.

## 21. Build and run commands

### 21.1. Local build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 21.2. Docker Compose dev

```bash
cd infra
docker compose -f docker-compose.dev.yml up --build
```

Gateway endpoints in dev:

```text
http://localhost:8088/healthz
http://localhost:8088/v1/flow-orders
```

Example create order:

```bash
curl -X POST "http://localhost:8088/v1/flow-orders" \
  -H "Content-Type: application/json" \
  -d '{
    "user_id":"demo-user",
    "symbol":"BTC/USDT",
    "side":"buy",
    "total_qty":0.010,
    "price_low":99.00,
    "price_high":101.00,
    "max_speed":0.002
  }'
```

## 22. Безопасность и compliance

- Не хранить secrets, private keys, API keys, passwords в git.
- Dev `InsecureServerCredentials` допустимы только в dev skeleton; для production нужен mTLS/secure channel.
- KYC/AML, jurisdiction restrictions и regulatory exports должны быть описаны до реализации.
- Все operator actions должны иметь audit log.
- Kill-switch должен быть защищён авторизацией.
- PII не должна попадать в Kafka topics без необходимости и политики retention.

## 23. Работа с `legacy_mvp/`

`legacy_mvp/` — справочный исходный MVP.

Правила:

- не удалять;
- не смешивать с новым C++ skeleton без явного migration plan;
- можно использовать идеи, UI assets, DB diagrams, прежние сервисные паттерны;
- при переносе кода оформлять как отдельный refactoring/migration task;
- не считать legacy architecture источником истины, если она конфликтует с `docs/` или `contracts/proto`.

## 24. Документация и язык

- Документация проекта — на русском, если пользователь не попросит иначе.
- Имена сервисов, proto packages, code identifiers — на английском.
- Термины сохранять единообразно: FlowOrder, CSLO, FOB, BatchResult, FillEvent, RiskSnapshot, ExecutionIntent, ExecutionReport, AgentLog.
- Формулы в Markdown писать с LaTeX-delimiters `\(` `\)` или display `\[` `\]`.
- Для display math всегда ставь перенос строки после `\[` и перед `\]`.

## 25. Шаблон действий для типовой задачи

### 25.1. Добавить новую фичу

1. Создать `docs/02-system/features/F-XX-*.md` или обновить существующую фичу.
2. Обновить use case.
3. Обновить component docs.
4. Обновить API contract.
5. Обновить data docs.
6. Реализовать код.
7. Добавить тесты.
8. Обновить observability/runbook.
9. Обновить changelog.

### 25.2. Добавить gRPC method

1. Обновить `.proto`.
2. Проверить backward compatibility.
3. Обновить `docs/06-api/grpc/`.
4. Пересобрать contracts.
5. Реализовать server method в `transport/`.
6. Добавить application use case.
7. Добавить client wrapper, если нужен.
8. Добавить tests.

### 25.3. Добавить Kafka event

1. Описать topic and schema.
2. Обновить `create_topics.sh`.
3. Добавить producer.
4. Добавить consumer.
5. Добавить idempotency/replay considerations.
6. Добавить observability.
7. Добавить tests.

### 25.4. Добавить DB table

1. Описать таблицу в `docs/07-data/`.
2. Описать owner service.
3. Описать migration.
4. Добавить repository/DAO.
5. Добавить integration tests.
6. Обновить backup/retention docs.

## 26. Known gaps / planned evolution

Текущий skeleton не является полной продукционной биржей. Ожидаемые зоны развития:

- полноценный solver для FOB/CSLO вместо simulator;
- persistent PostgreSQL для OLTP;
- ClickHouse ingestion для истории и аналитики;
- полноценный Web UI / Trading Frontend;
- Auth & Identity / KYC integration;
- portfolio orders and multi-leg matching;
- market-maker curves;
- execution hedge policies;
- external venue adapters;
- backtest/replay engine;
- operator panel;
- production-grade observability;
- security hardening.

При любой задаче явно указывай, является ли изменение:

- `docs-only`;
- `contract change`;
- `MVP implementation`;
- `production hardening`;
- `research/simulation`;
- `migration from legacy_mvp`.

## 26a. Sequence diagram placement rules

1. **System-level sequence diagrams** (внешний участник ↔ Continuous Exchange System как black box) хранятся **только** в:

   ```text
   docs/02-system/use-cases/{UC-ID}/sequences/SEQ-{UC-ID}-system.md
   ```

2. **Service-level sequence diagrams** (cross-component, e.g. `API Gateway → Matching → Risk → Kafka → Ledger`) хранятся **только** в:

   ```text
   docs/05-components/sequences/SEQ-{F-ID}-{UC-ID}-services.md
   ```

3. **Internal component sequence diagrams** (внутренняя последовательность одного сервиса) хранятся **только** в:

   ```text
   docs/05-components/{component-name}/sequences/SEQ-{COMPONENT}-NNN-{topic}.md
   ```

4. Каждая sequence-диаграмма обязана ссылаться на:
   - Feature (`docs/02-system/features/`)
   - Use Case (`docs/02-system/use-cases/`)
   - Related Components (`docs/05-components/`)
   - Related Contracts (`docs/06-api/`)
   - Related Data Objects (`docs/07-data/`)

5. Каждая стрелка service-level диаграммы должна иметь backing-контракт: REST endpoint в `docs/06-api/rest/`, gRPC method в `docs/06-api/grpc/`, Kafka topic в `docs/06-api/messaging/`, или SQL/DDL в `docs/07-data/`.

6. **Запрещено** генерировать код, если у фичи нет:
   - Feature-файла,
   - Use Case-файла,
   - System-level и Service-level sequence diagram,
   - Контрактов в `docs/06-api/`,
   - Data schema в `docs/07-data/`.

7. **Запрещено** размещать system-level диаграммы вне `02-system/use-cases/`, service-level — вне `05-components/sequences/`. Любое нарушение — fail в traceability.

## 27. Минимальный ответ Claude Code после изменения

После выполнения задачи сообщай:

1. какие файлы изменены;
2. какие документы/контракты обновлены;
3. какие команды сборки/тестов запускались;
4. какие тесты не запускались и почему;
5. какие риски/следующие шаги остались.
