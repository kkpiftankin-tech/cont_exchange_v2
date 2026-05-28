---
id: ADR-015
status: draft
date: 2026-05-28
owners:
  - architecture
  - core-team
related:
  - docs/02-system/features/F-20-live-venue-simulator/feature.yaml
  - incoming-docs/2026-05-26-F-20-live-venue-simulator-v1.md
  - incoming-docs/IN-010.fragment-map.md
  - docs/06-api/messaging/execution-venue.md
  - CLAUDE.md (§7.3 execution.venue vs execution.reports dual-publish)
---

# ADR-015: F-20 SimExecutionReport — strict topic isolation vs shared execution.venue + simMode flag

## Контекст

F-20 Live Venue Simulator (IN-010) генерирует синтетические
`SimExecutionReport` (ExecutionReport + поля симуляции `simMode`,
`simSessionId`, `lobSnapshotId`, `lobAge`, `impactBps`, `slippageBps`,
`latencySampleMs`).

Спека §1.2 листит два варианта приёмника:

- `execution.venue` — тот же топик, что и для боевых ExecutionReport,
  с маркером `simMode=true`;
- `sim.execution.venue` — опциональный изолированный топик только для
  sim-отчётов.

И прямо говорит «optional» про второй, оставляя выбор открытым.

В SHADOW-режиме (F-20 routingMode=SHADOW) **один** ChildOrderRequest
порождает **два** отчёта по одному `clientOrderId`: LIVE
(`simMode=false`) и SIM (`simMode=true`). Это и есть источник риска.

Текущие consumers `execution.venue`:

- Settlement Ledger (`cpp/ledger/src/infra/kafka_consumers.cpp`) —
  применяет ExecutionReport к **реальным** позициям с идемпотентностью
  по `report_id`.
- market_data → ClickHouse `execution_reports` (canonical history).
- Risk Manager (post-hedge exposure).

## Решение

**Строгая изоляция: SimExecutionReport публикуется ТОЛЬКО в
`sim.execution.venue`, и НИКОГДА в общий `execution.venue`.**

Боевые consumers (`execution.venue`) физически не видят sim-данные.
Sim-специфичные consumers подписываются на `sim.execution.venue`:

- sim-book ledger consumer (ADR-016) → пишет только в sim_* таблицы;
- sim CH ingestion → `sim_execution_reports`;
- Divergence Service (SHADOW) → читает ОБА топика и сопоставляет по
  `clientOrderId`.

Спековый вариант «execution.venue + simMode=true» **отклонён** (см.
ниже).

## Почему отклоняем shared execution.venue + simMode flag

1. **SHADOW double-apply.** В SHADOW обе версии (LIVE + SIM) идут с
   одним `clientOrderId`, но разными `report_id`. Идемпотентность
   ledger'а построена на `report_id`, поэтому **оба** пройдут фильтр
   дубликатов. Если sim-отчёт попадёт в `execution.venue`, боевой
   ledger применит его к реальной позиции — двойное применение и
   искажение реального P&L. Это класс ошибок «забыли отфильтровать по
   simMode».
2. **Опора на дисциплину каждого consumer'а.** Shared-топик требует,
   чтобы КАЖДЫЙ существующий и будущий consumer добавил
   `if (report.sim_mode) skip`. Один пропуск = боевое решение на
   sim-данных (margin, liquidation, withdrawal). Изоляция на уровне
   топика убирает весь класс ошибок by construction.
3. **Чистота истории.** Боевая CH `execution_reports` остаётся 100%
   реальной — quality-of-execution отчёты, audit, F-13 post-trade не
   нужно фильтровать.

## Почему НЕ дублируем в оба топика

Промежуточный вариант (publish в `execution.venue` И
`sim.execution.venue`) — как делает боевой dual-publish
`execution.venue`/`execution.reports` (CLAUDE.md §7.3) — здесь
**не подходит**: цель того dual-publish была backward-compat для
boevых consumers при миграции имени топика. Для sim же цель прямо
противоположная — НЕ дать боевым consumers увидеть sim. Дублирование
вернуло бы double-apply риск из п.1.

## Последствия

### Положительные

- Boevой ledger / CH / risk не требуют изменений и не могут случайно
  применить sim-данные.
- SHADOW-режим безопасен: LIVE идёт в `execution.venue`, SIM в
  `sim.execution.venue`, Divergence Service читает оба.
- Чёткая граница для retention/ACL (sim-топик можно держать
  short-retention, отдельный consumer-group).

### Отрицательные

- sim-book ledger — это **новый** consumer (новая consumer-group на
  `sim.execution.venue`), а не ветка в существующем. +1 deployment
  concern. (Смягчается тем, что sim-book и так нужен отдельно — ADR-016.)
- Divergence Service подписывается на 2 топика и джойнит — чуть
  сложнее, чем читать один. Приемлемо.

### Обратимость

Высокая. Если позже захотим единый топик — пришлось бы вернуть
дискриминатор (поле `simMode` или header) и добавить фильтр во все
boevые consumers. Обратно (из shared в isolated) — сложнее, поэтому
начинаем со строгой изоляции (safe default).

## Влияние на контракты

- Новый Kafka topic `sim.execution.venue` (в `infra/kafka/create_topics.sh`).
- **`execution.proto` ExecutionReport НЕ расширяется sim-полями.**
  Контракт venues↔ledger един для sim и live — симулятор эмитит тот же
  самый `ExecutionReport`, что и реальный venue, просто в другой топик.
  Дискриминатор sim/live — ТОПИК, а не поле (см. §ниже).
- F-20 feature.yaml kafkaTopics.produces: `sim.execution.venue`,
  `sim.alerts`, `sim.config`; НЕ `execution.venue` для sim.

## ExecutionReport остаётся неизменным; sim/live различаются топиком

Прямое следствие принципа «взаимодействие с симулятором и с реальными
биржами идёт по одинаковым контрактам»:

1. **`ExecutionReport` не меняется.** Он уже содержит все
   venue-agnostic поля качества исполнения: `filled_qty`,
   `remaining_qty`, `average_price`, `slippage_bps`, `hedge_pnl`,
   `reference_mid`, `fee_total`. Симулятор заполняет их так же, как
   реальный venue. Никаких `simMode` / `lobSnapshotId` / `impactBps` в
   этом сообщении.
2. **`simMode`-поле избыточно.** Раз потоки разделены топиком
   (`sim.execution.venue` vs `execution.venue`), consumer знает природу
   отчёта по своей подписке. Поле дублировало бы дискриминатор и давало
   лишнюю поверхность для ошибок (доверять полю вместо топика).
3. **Sim-телеметрия — в отдельном сайдкаре, не в execution-контракте.**
   `sim_session_id`, `lob_snapshot_id`, `lob_age_ms`, `impact_bps`,
   `latency_sample_ms` — это provenance / output моделей симуляции, а не
   факт исполнения. Они живут в отдельном сообщении
   `fob.sim.v1.SimExecutionAnnotation`, коррелированном по `report_id`,
   которое VenueSimulator эмитит в sim-only телеметрию и которое ложится
   колонками в ClickHouse `sim_execution_reports`. Боевой путь его не
   производит и не потребляет.

```proto
// fob/sim/v1/sim.proto — НЕ в execution.proto
message SimExecutionAnnotation {
  string report_id = 1;        // correlation -> ExecutionReport.report_id
  string sim_session_id = 2;
  string lob_snapshot_id = 3;
  uint32 lob_age_ms = 4;
  double impact_bps = 5;
  uint32 latency_sample_ms = 6;
}
```

Это уточнение **корректирует** исходную спеку IN-010 §1.0, где
SimExecutionReport описан как «ExecutionReport + дополнительные поля
статистировки». Дополнительные поля выносятся в сайдкар, чтобы общий
исполнительный контракт оставался venue-agnostic.

## Status

Draft. Резолвит F-20 knownIssue `sim-execution-topic-isolation-pending-adr`.
