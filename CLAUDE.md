# CLAUDE.md — Continuous Exchange / Flow Order Book

Корневой контекст для Claude Code в `cont_exchange_v2.0`. Здесь — только то,
что нужно в **каждой** сессии и каждому субагенту. Подробности загружаются по
требованию: правила по путям — [`.claude/rules/`](.claude/rules/), процедуры —
[`.claude/skills/`](.claude/skills/), сервис-специфика — вложенные
`cpp/<service>/CLAUDE.md`, знания о системе — [`docs/`](docs/README.md).

Работай с проектом как с системой непрерывной биржи заявок: торговля
моделируется как поток объёма с конечной скоростью (Flow Order Book /
Continuous Matching), а не мгновенное исполнение дискретных ордеров.
Документация — первичный источник истины, код — её проверяемая реализация.
Текущий статус: C++20 microservices MVP skeleton, бизнес-логика местами
симуляционная; контракты и Kafka-топики уже заложены под замену симуляторов
реальным solver/persistence/venue-адаптерами.

## 1. Непереговорные правила

1. **Деньги — только `cex::common::Decimal` / protobuf `Decimal`.**
   `double`/`float` запрещены в domain-логике, ledger, risk, matching
   settlement, persistence. `double` допустим только для solver diagnostics,
   metrics, research/simulation, если результат не попадает в ledger.
   Подробности и конвертация — [`.claude/rules/data.md`](.claude/rules/data.md).
2. **Docs-first / contract-first.** Не пиши код фичи, пока не существует вся
   цепочка Business Requirement → Feature → Use Case → System/Service
   Sequence → Contracts → Data Objects → Tests (класс L/XL — см. §3). Полное
   правило, traceability-цепочка и conflict-rule —
   [`.claude/rules/docs-as-code.md`](.claude/rules/docs-as-code.md).
3. **Приоритет источников** при конфликте: явное указание пользователя →
   этот файл → `docs/` (особенно ADR и контракты) → `contracts/proto/` →
   `cpp/` → `legacy_mvp/` (справочник, не источник истины).
4. **Слои сервиса и границы.** `transport → app → domain ← infra`; `domain`
   не знает о gRPC/Kafka/HTTP/DB. `matching` не резервирует средства,
   `ledger` не принимает risk-решений, `venues` не принимает решений о
   хедже сам. Полностью — [`.claude/rules/cpp.md`](.claude/rules/cpp.md).
5. **Не редактировать** сгенерированные `.pb.cc/.pb.h/.grpc.pb.*`; не
   хранить секреты/credentials в git (`permissions.deny` в
   `.claude/settings.json` блокирует `Read` на `infra/.env`, `secrets/**`);
   `incoming-docs/` — immutable архив, не менять руками.
6. **ADR обязателен** при изменении: архитектурного стиля, границ сервисов,
   брокера/БД/протокола, схемы Kafka-топика, публичного gRPC/REST
   контракта, модели денежных расчётов, matching/clearing algorithm, risk
   /margin/liquidation policy, security/custody/KYC, ранее закреплённого
   решения. ADR в `docs/03-architecture/adr/`: контекст, решение,
   альтернативы, последствия, обратимость.
7. **Kill-switch и operator actions** — под авторизацией и audit trail;
   PII не должна попадать в Kafka topics без политики retention.
8. **Документация — на русском**, если не попросили иначе; identifiers/proto
   packages — на английском; формулы — `$...$` / `$$...$$` (перенос строки
   после `$$` и перед закрывающим `$$`).

## 2. Где что лежит

- `docs/README.md` — индекс документации (этапы 01–11 × уровни L0–L3).
- `docs/04-domain/core-concepts-and-invariants.md` — FlowOrder/CSLO/BatchResult,
  MVP-инварианты; `entities.md` — технические поля; `business-rules.md` —
  формулы клиринга/риска/hedge/backtest.
- `docs/04-domain/calculations/` — карточки вычислений `CALC-*` (формула →
  код → тест → экран); начни здесь любое замечание про число на экране.
- `docs/feedback/` — журнал замечаний с фронта (`inbox.md` → `/triage` →
  `FB-<дата>.md`).
- `contracts/proto/fob/` — контракты (proto); `infra/` — compose, postgres,
  kafka; `docker/` — образы.
- `cpp/<service>/` — сервисы: `gateway`, `order_flow`, `matching`, `risk`,
  `ledger`, `market_data`, `venues`, `venue_health`, `backtest`,
  `observability`, `common`. Таблица «сервис → ответственность → топики» —
  [`.claude/rules/cpp.md`](.claude/rules/cpp.md).
- `frontend/web/` — UI (React), `frontend/api/` — BFF (`server.js`).
- `legacy_mvp/` — справочный старый MVP, не источник истины.

Репозиторий использует **11-папочный layout** в `docs/` (не 10, как в
bootstrap-инструкциях) — не переименовывать без ADR, детали —
[`.claude/rules/docs-as-code.md`](.claude/rules/docs-as-code.md).

## 3. Как работать

1. Классифицируй задачу — класс **S/M/L/XL** и триггеры эскалации по
   [`.claude/rules/agent-routing.md`](.claude/rules/agent-routing.md) — и
   покажи короткий план ДО правок: что затронуто, какие инварианты сохранить,
   какие команды/тесты запустить, каких агентов (если каких-либо) вызовешь и
   почему. Не вызывай субагента без причины — минимально достаточная
   глубина, не «всё подряд» и не «ни одного».
2. Разбор замечаний с фронта/CLI (не с нуля, а по конкретному наблюдению) —
   навык `/triage` (`.claude/skills/triage/`), не вручную.
3. Перед «проверь в браузере/на дев-стенде» — сначала `/rebuild-service
   <сервис>` для затронутых сервисов.
4. Docs-first полностью — для L/XL; для S/M обнови только затронутые
   документы (карточку `CALC-*`, `feature.yaml`, `docs/feedback/`).
5. Если входящий текст/находка противоречит существующим docs — Conflict
   Note в затронутый документ, не молчаливый выбор одной версии.

## 4. Команды

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cd infra && docker compose -f docker-compose.dev.yml up --build
python3 tools/traceability-checker/check.py
python3 tools/proto-contract-auditor/check_proto_map.py
python3 tools/agents_report.py --days 7        # калибровка маршрутизации агентов
```

Dev gateway: `http://localhost:8088/healthz`, `POST /v1/flow-orders`. Полные
build/run детали и dev-хост — `.claude/rules/cpp.md`, навык
`rebuild-service`.

## 5. Минимальный ответ после изменения

1. Класс задачи (S/M/L/XL), сработавшие триггеры, вызванные агенты и причина
   для каждого (или «без агентов»).
2. Изменённые файлы; обновлённые документы/контракты.
3. Какие сборки/тесты запускались; какие нет и почему.
4. Unresolved TODO contracts / conflicts / coverage status.
5. Риски и следующие рекомендованные шаги.

## 6. При сжатии контекста сохраняй

Класс текущей задачи, список изменённых файлов, вызванных агентов и любые
открытые замечания `FB-*`/Conflict Notes, заведённые в этой сессии.

## 7. Куда перенесено из версии до 2026-09-28 (970 строк)

| Было (раздел) | Теперь |
|---|---|
| §0a, §0c docs-as-code, размещение артефактов | [`.claude/rules/docs-as-code.md`](.claude/rules/docs-as-code.md) |
| §0b repo map (полный) | там же + §2 здесь (сжато) |
| §2 цели продукта | [`docs/01-business/goals.md`](docs/01-business/goals.md) |
| §4 структура репозитория | удалено — видно из `ls`/репозитория, не хранить отдельно |
| §5 целевая структура документации | [`.claude/rules/docs-as-code.md`](.claude/rules/docs-as-code.md) |
| §6 сервисы и ответственность | [`.claude/rules/cpp.md`](.claude/rules/cpp.md) |
| §7 контракты и топики | [`.claude/rules/contracts.md`](.claude/rules/contracts.md) |
| §8 доменная модель (FlowOrder/CSLO/BatchResult/VWAP/IS) | [`docs/04-domain/core-concepts-and-invariants.md`](docs/04-domain/core-concepts-and-invariants.md) |
| §10, §12 архитектурный стиль, C++ | [`.claude/rules/cpp.md`](.claude/rules/cpp.md) |
| §11 development workflow | §3 здесь + [`.claude/rules/agent-routing.md`](.claude/rules/agent-routing.md) |
| §13 Kafka/Redpanda | [`.claude/rules/contracts.md`](.claude/rules/contracts.md) |
| §14 хранилища данных | [`.claude/rules/data.md`](.claude/rules/data.md) |
| §15–18 matching/risk/ledger/venues rules | `cpp/matching/CLAUDE.md`, `cpp/risk/CLAUDE.md`, `cpp/ledger/CLAUDE.md`, `cpp/venues/CLAUDE.md` |
| §19–20 observability, testing | [`.claude/rules/testing.md`](.claude/rules/testing.md) |
| §21 build/run | §4 здесь + навык `rebuild-service` |
| §22 security/compliance | §1 здесь (сжато) |
| §23 legacy_mvp | [`.claude/rules/legacy.md`](.claude/rules/legacy.md) |
| §25 шаблоны действий | навыки `create-feature`, `register-kafka-topic`, `register-pg-table`, чек-листы в `.claude/rules/contracts.md`/`data.md` |
| §26 known gaps | [`docs/00-methodology/known-gaps.md`](docs/00-methodology/known-gaps.md) |
| §26a sequence diagram placement | [`.claude/rules/sequences.md`](.claude/rules/sequences.md) |
| §27 минимальный ответ | §5 здесь |

Старые номера секций больше не действуют как якоря — используй заголовки
файлов выше. Обоснование самой реформы (экономика контекста, маршрутизация
агентов) — `incoming-docs/`, конспект «Золотая середина в вызове агентов
Claude Code».
