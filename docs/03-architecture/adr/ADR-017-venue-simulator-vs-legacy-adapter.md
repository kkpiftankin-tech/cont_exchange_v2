---
id: ADR-017
status: draft
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-20-live-venue-simulator/feature.yaml
  - docs/03-architecture/adr/ADR-012-venue-execution-adapter-decomposition.md
  - cpp/venues/src/infra/venue_sim_adapter.cpp
  - cpp/venues/src/infra/simulated_venue_adapter.cpp
  - cpp/venues/src/domain/venue_adapter.hpp
---

# ADR-017: F-20 VenueSimulator — новый класс vs переиспользование legacy venue_sim_adapter

## Контекст

В `cpp/venues/src/infra/` уже существуют:

- `venue_sim_adapter.{cpp,hpp}` — реализация `domain::VenueAdapter`,
  используется в F-04 integration-тестах и F-12 backtest (детерминированные
  fills против записанных VenueSim событий);
- `simulated_venue_adapter.cpp` — аналогичный helper.

Обе — это `VenueAdapter`: их вызывает `ExecuteOnVenue::Run(intent,
adapter)`, они возвращают `VenueOrderResult`. Они НЕ держат LOB-кэш, НЕ
применяют impact/latency/rejection модели, НЕ знают про SimSession.

F-20 вводит `VenueSimulator` (IN-010 §1.0) — принципиально другой
компонент:

- stateful LOB-matcher, подписан на живой `venue.snapshots`;
- LEVEL_BY_LEVEL matching + ImpactModel + FeeModel + RejectionModel +
  LatencyModel (async delay);
- сидит на уровне `VenueSimRouter` (между Venue Execution Adapter и
  EVC), а не реализует `VenueAdapter`;
- управляется `SimSession` (routingMode, scope, hot-reload через
  `sim.config`).

Вопрос: переиспользовать имя/код `venue_sim_adapter` или ввести новый
класс `VenueSimulator`?

## Решение

**Новый класс `VenueSimulator`** в `cpp/venues/src/app/venue_simulator.{cpp,hpp}`.
Legacy `venue_sim_adapter` / `simulated_venue_adapter` остаются
**как есть** (backtest/F-04 helper), не трогаются и не переименовываются.

## Почему не переиспользовать

1. **Разные абстракции.** `venue_sim_adapter` — это `VenueAdapter`
   (точка интеграции «как сходить на одну биржу»). `VenueSimulator` —
   это движок матчинга с моделями и состоянием LOB-кэша, живущий на
   слое роутинга. Натянуть второе на интерфейс первого = протечка
   абстракции (VenueSimulator не отвечает на `Connect()/Heartbeat()/
   Subscribe()` семантику VenueAdapter осмысленно).
2. **Хрупкость существующих тестов.** F-04 full-cycle + backtest parity
   тесты зависят от детерминированного поведения legacy-адаптера
   (фиксированные fills без impact/latency). Переименование/изменение
   сломает их и смешает «backtest replay determinism» с «live sim
   realism» — две разные гарантии.
3. **Разный источник ликвидности.** Legacy adapter берёт fills из
   записанных событий (backtest) или простой симуляции. VenueSimulator
   берёт **живой** `venue.snapshots` (F-11). Это разные входы; общий
   класс заставил бы ветвиться по источнику внутри.

## Что можно разделить (без форсирования)

Чисто-математические утилиты (VWAP по уровням, impact-формулы LINEAR/
SQRT/POWER_LAW) могут со временем переехать в общий header
`cpp/venues/src/domain/liquidity_math.hpp`, если обнаружится реальное
дублирование между VenueSimulator и depth_curve_builder. Но это
**emergent refactor**, не предусловие F-20. На старте — отдельная
реализация в VenueSimulator.

## Связь с ADR-012 / ADR-013

ADR-012 (venue-execution-adapter decomposition) и ADR-013 (execution
planning placement) уже зафиксировали, что Venue Execution Adapter и
Execution Planning живут в `cpp/venues/` как модули на MVP. VenueSimulator
и VenueSimRouter — ещё два модуля в `cpp/venues/src/app/` на MVP, с тем
же условием возможного split в отдельный сервис позже (как в ADR-013):

| Phase | Размещение |
| --- | --- |
| F-20 MVP | `cpp/venues/src/app/venue_simulator.*` + `venue_sim_router.*` (тот же бинарник venues) |
| F-20 v1.1 | возможный split в `cpp/venue_simulator/` если нужна независимая шкала |

## VenueSimRouter и legacy adapter

`VenueSimRouter` вставляется между `Venue Execution Adapter` и выбором
adapter'а. В режиме `LIVE_ONLY` он прозрачно вызывает реальный
EVC-адаптер (включая, в backtest-контексте, legacy `venue_sim_adapter`).
В `SIM_ONLY` — `VenueSimulator`. То есть legacy-адаптер остаётся валидным
LIVE-таргетом роутера в backtest, что подтверждает: это разные роли, и
сосуществование естественно.

## Последствия

### Положительные

- F-04 / backtest тесты не ломаются.
- Чистое разделение «детерминированный replay helper» vs «live realism
  simulator».
- Возможность вынести VenueSimulator в отдельный сервис позже.

### Отрицательные

- Два «sim» в `cpp/venues/` (legacy adapter + новый VenueSimulator) —
  потенциальная путаница по имени. Смягчается комментарием-header'ом в
  обоих файлах + этой ADR.

### Обратимость

Высокая для извлечения общих утилит (можно сделать когда/если появится
дублирование). Низкая для обратного слияния абстракций (не планируется).

## Status

Draft. Резолвит F-20 knownIssue `existing-venue-sim-adapter-relationship`.
