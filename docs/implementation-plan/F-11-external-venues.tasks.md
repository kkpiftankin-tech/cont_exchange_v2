# Implementation Tasks: F-11 External Venues / LOB → FOB

Контекст: [F-11 feature](../02-system/features/F-11-external-venues-lob-to-fob/), [test plan](../10-testing/features/F-11-test-plan.md). Документная база полностью ингестирована из IN-004 — см. [fragment-map](../../incoming-docs/IN-004.fragment-map.md). Большинство кода уже импортировано из `origin/dev` (Connector, Normalizer, Curve Builder, Health & Routing); задачи ниже закрывают оставшиеся gap'ы.

## Source Artifacts

- Feature: [F-11 README](../02-system/features/F-11-external-venues-lob-to-fob/README.md)
- Acceptance: [acceptance-criteria.md](../02-system/features/F-11-external-venues-lob-to-fob/acceptance-criteria.md)
- Use Cases: [UC-F11-01..05](../02-system/use-cases/)
- Contracts: [venue-topics.md](../06-api/messaging/venue-topics.md), [REST /api/v1/venues](../06-api/rest/venues.md)
- Data: [venue-config](../07-data/venue-config.md), [synthetic-orders](../07-data/synthetic-orders.md), [venue-snapshots](../07-data/venue-snapshots.md), [venue-liquidity-curves](../07-data/venue-liquidity-curves.md)
- Source: IN-004

## Preconditions

- [x] Feature exists
- [x] Use cases exist (UC-F11-01..05)
- [x] System-level sequences exist
- [x] Service-level sequences exist (SEQ-F11-01..05-services)
- [x] Kafka contracts exist (venue-topics.md)
- [x] REST contract exists (venues.md)
- [x] Data schemas exist (venue-config, synthetic-orders, venue-snapshots, venue-liquidity-curves)
- [x] Acceptance criteria exist
- [x] Test plan exists

## Tasks

### Фаза 1 — Persistence (PostgreSQL + ClickHouse DDL)

#### T-F11-100. PostgreSQL DDL для `venue_config` и `synthetic_orders`

AC: AC-19 (hot reload), AC-11 (synthetic lifecycle).

- Добавить DDL из [venue-config.md](../07-data/venue-config.md) и [synthetic-orders.md](../07-data/synthetic-orders.md) в `infra/postgres/init.sql`.
- Согласовать поля runtime row vs DDL (см. open-questions.md §5):
  - либо расширить DDL новыми полями (`*_ms`/`*_enabled`/`routing_mode`);
  - либо принять JSONB `lobtofobmodel` и migrate runtime row → JSONB.
- Создать ENUM types: `venuetype_enum`, `side_enum`, `synthetic_status_enum`.
- Добавить индексы (`is_active`, `(venueid, symbol, status)`, `expiresat WHERE active`).
- Cleanup-job для `synthetic_orders` (TTL по 30 дней для `expired`/`used`).

Acceptance:

- compose up создаёт обе таблицы без ошибок;
- `cpp/venues` стартует с `VENUES_POSTGRES_DSN` и не делает `EnsureSchema` (DDL уже есть);
- интеграционный тест I6 (`Testing/f11_test2_e2e.sh`) проходит.

#### T-F11-110. ClickHouse DDL + Kafka ingestion для `venue_snapshots`, `venue_liquidity_curves`

AC: AC-4, AC-26, частично AC-20.

- Добавить DDL из [venue-snapshots.md](../07-data/venue-snapshots.md) и [venue-liquidity-curves.md](../07-data/venue-liquidity-curves.md) в `infra/clickhouse/init.sql`.
- Kafka engine MV pattern (аналогично F-04 fills/batchresults):
  - `venue_snapshots_kafka` (ENGINE=Kafka, topic `venue.snapshots`);
  - `venue_snapshots` (MergeTree, partition by day, TTL 90 days);
  - `venue_snapshots_mv` (Materialized View → MergeTree).
- То же для `venue_liquidity_curves`.
- Опционально: переключить `SnapshotClickHouseWriter` в read-only mode (если ingestion идёт через Kafka MV).

Acceptance:

- интеграционный тест: после публикации N snapshots в Kafka → ≥ N строк в CH через ≤ 5 секунд;
- `SELECT count() FROM venue_snapshots` ≥ N.

### Фаза 2 — Risk Manager интеграция

#### T-F11-200. Risk Manager consumer `venue.health`

AC: AC-13.

- Добавить Kafka consumer в `cpp/risk` для `venue.health` с filter `event_type=AGGREGATED`.
- Per-venue policy: при `BLOCK` или `circuit_breaker_state=OPEN` — выставить `RiskDecision` запрет для intents на эту площадку.
- Возможно отдельная risk_event_type `VENUE_DEGRADED`.

Acceptance:

- интеграционный тест: при mock-stale площадки матчинг intent на неё отклоняется с reason="venue_degraded".

### Фаза 3 — Routing policy + Execution Planning

#### T-F11-220. Формализовать routing matrix (allow/caution/avoid/block)

AC: связано с AC-14, open-questions.md §3, §4.

- Документировать матрицу в [venue-health-routing/overview.md](../05-components/venue-health-routing/overview.md) — текущая считается в коде в двух местах ([cpp/venues/src/main.cpp:170-184](../../cpp/venues/src/main.cpp#L170-L184), и FSM в cpp/venue_health).
- Решить: переместить ли в `venue_config` per-venue override (slippage_threshold, routing_thresholds) — open questions §3, §4.
- Привести cpp/venues и cpp/venue_health к единой реализации (одна функция).

Acceptance:

- routing-recommendation в cpp/venues admin API и в AGGREGATED venue.health совпадают;
- unit-тест на матрицу.

#### T-F11-230. Execution Planning consumer routing

AC: AC-14 (полное закрытие).

- F-12 cross-link: использовать `routing_recommendation` для allocation между площадками.
- Запретить intents на `BLOCK`; разрешить avoidance/scale-down при `CAUTION`/`AVOID`.

Acceptance:

- интеграционный e2e через [Testing/f11_test2_e2e.sh](../../Testing/f11_test2_e2e.sh) с эмуляцией degradation одной площадки.

### Фаза 4 — Качество и тесты

#### T-F11-300. L3 calibration: unit-тесты U8

AC: AC-9.

- Создать `cpp/venues/tests/l3_calibration_test.cpp`:
  - подать набор execution reports;
  - проверить, что `epsilon3` уменьшается при стабильных reports и растёт при шумных.
- Решить open-question §1 (источник reports, окно ретроспективы) — закрепить в [liquidity_curve_producer.hpp](../../cpp/venues/src/app/liquidity_curve_producer.hpp).

Acceptance:

- 3+ кейсов unit-теста проходят;
- AC-9 переходит ⚠️ → ✅.

#### T-F11-310. CI trigger для `Testing/f11_*.sh`

AC: AC-21..23 (perf), I1, I2, I6 (e2e).

- Завести GitHub Actions / GitLab CI job, который запускает:
  - `Testing/f11_test2_e2e.sh` — на каждом PR.
  - `Testing/f11_test3_load.sh` — nightly.
  - `Testing/f11_test4_quality.sh` — nightly.
- Сохранять метрики (`p95 latency`, `epsilon1`, `200/sec throughput`) в артефакты.
- Алертить при превышении SLA.

Acceptance:

- CI pipeline зелёный на main;
- регрессии на p95 latency / epsilon1 ловятся.

### Фаза 5 — Cleanup и deprecation

#### T-F11-320. Миграция consumers F-05 на `venue.snapshots`

AC: связан с Conflict Note C-1, AC-2.

- Обновить `cpp/market_data` consumer: вместо `marketdata.raw` подписаться на `venue.snapshots`.
- Adapter в cpp/venues остановить дублирование (env-флаг `VENUES_PUBLISH_LEGACY_MARKETDATA_RAW=false`).
- Документировать deprecation `marketdata.raw` в [topics.md](../06-api/messaging/topics.md).

Acceptance:

- market_data сервис работает без `marketdata.raw`;
- F-05 use cases без регрессий.

#### T-F11-330. Decide on Curve Builder split (если нужно)

AC: связан с ADR-014.

- Решить: оставить Curve Builder в одном бинаре с Connector или вынести в отдельный compose-сервис.
- Если split — сделать новый compose-сервис `venue-curve-builder` + Kafka consumer на `venue.snapshots`.
- Pros / cons — в [ADR-014](../03-architecture/adr/ADR-014-venues-binary-vs-components.md).

Acceptance:

- ADR-014 переведён в `Accepted` (либо `Rejected` с обоснованием).
- При split: e2e тесты по-прежнему проходят.

## Definition of Done (IN-004)

Чек-лист DoD из IN-004 §«Definition of Done»:

- [x] External Venues Connector подключается к ≥ 2 CEX и ≥ 1 DEX/AMM
- [x] Venue Market Data Normalizer публикует корректные VenueSnapshot
- [x] Venue Liquidity Curve Builder публикует VenueLiquidityCurve
- [x] В режиме совместимости генерируются SyntheticFlowOrder
- [⚠️] Matching Backend использует внешнюю ликвидность в FOB-форме (partial: F-09 + F-04 cross-link)
- [❌] Risk Manager учитывает venue.health — T-F11-200
- [⚠️] Execution Planning использует FOB-кривые и health-score для routing — T-F11-220, T-F11-230
- [x] Venue Execution Adapter публикует execution.venue, Ledger обновляет venue-балансы (F-12 owns)
- [x] Stale detection реализован и протестирован
- [x] Circuit breaker реализован per venue
- [x] Admin API CRUD venue_config + hot reload
- [⚠️] p95 latency VenueSnapshot < 500 ms — T-F11-310 (CI replay nightly)
- [⚠️] p95 LOB-FOB < 50 ms для L1/L2 — T-F11-310
- [⚠️] Ошибка стоимости LOB-FOB ≤ 1% на рабочем диапазоне — T-F11-300, T-F11-310
- [⚠️] Пройдены unit/integration/load tests — partial; load — T-F11-310
- [⚠️] Dashboard оператора показывает статусы, health-score, quality-метрики — F-16/F-17 scope
- [x] Документация API актуальна (после этого PR)
- [⏳] Code review пройден

Legend: [x]=done, [⚠️]=partial, [❌]=not started, [⏳]=in flight.

## Маппинг AC → задача

| AC      | Closing tasks                         |
| ------- | ------------------------------------- |
| AC-1..3 | (closed — code imported)              |
| AC-4    | T-F11-110                             |
| AC-5..8 | (closed)                              |
| AC-9    | T-F11-300                             |
| AC-10   | (closed)                              |
| AC-11   | T-F11-100 (DDL)                       |
| AC-12   | F-04 cross-link                       |
| AC-13   | T-F11-200                             |
| AC-14   | T-F11-220, T-F11-230                  |
| AC-15..20 | (closed; см. ⚠️ ниже)               |
| AC-21..25 | T-F11-310 (CI perf)                 |
| AC-26   | T-F11-100, T-F11-110                  |
| AC-27..28 | (closed)                            |

## Оценка трудозатрат (порядок величин)

| Фаза | Задачи             | Усилия              |
| ---- | ------------------ | ------------------- |
| 1    | T-F11-100..110     | 3–5 дней             |
| 2    | T-F11-200          | 2–3 дня              |
| 3    | T-F11-220..230     | 3–5 дней (F-12)      |
| 4    | T-F11-300..310     | 3–5 дней             |
| 5    | T-F11-320..330     | 2–4 дня + ADR        |

Итого: **13–22 рабочих дня**. Параллелизация даёт ~7–10 дней календарных.

## Source Fragments

- IN-004 §«Definition of Done»
- IN-004 §«Функциональные требования» F11-1..F11-20
- IN-004 §«Нефункциональные требования»
- IN-004 §«Тестовые кейсы»
