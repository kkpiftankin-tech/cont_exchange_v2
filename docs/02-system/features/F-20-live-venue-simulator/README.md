# F-20 — Live Venue Simulator (Симулятор исполнения на внешних площадках)

Источник: **IN-010** ([meta](../../../../incoming-docs/IN-010.meta.md) · immutable-архив `incoming-docs/F-20 -Live-Venue-Simulator.md`).
Спека: [feature.yaml](feature.yaml).

## Суть

Гибридный режим: система **боевым образом** принимает живые LOB-стаканы внешних
CEX/DEX/AMM через F-11 (EVC → Normalizer → `VenueSnapshot` → `venue.snapshots`), но
исполнение child-ордеров F-12 **симулируется** без реального выхода на рынок.
`VenueSimRouter` перехватывает `ChildOrderRequest` от Venue Execution Adapter и в
одном из режимов направляет его:

- **SIM_ONLY** — только в `VenueSimulator` (EVC не вызывается);
- **LIVE_ONLY** — прозрачный проксинг в EVC (симулятор не вызывается);
- **SHADOW** — одновременно в EVC (LIVE) и в `VenueSimulator` (SIM); оба отчёта
  публикуются, `Divergence Service` сравнивает SIM vs LIVE.

`VenueSimulator` на актуальном `VenueSnapshot` делает LEVEL_BY_LEVEL matching и
применяет модели **latency / impact / fee / rejection**, публикуя синтетический
`SimExecutionReport` в стандартном формате F-12 (`execution.venue`) с маркером
`simMode=true`. Downstream (Ledger/Risk/ClickHouse) потребляет без изменений, но
sim-отчёты идут в **изолированную sim-книгу**.

Отличие от **F-15 (Backtest/Replay)**: F-20 работает на **живом** стакане в
реальном времени, F-15 — на историческом.

## Use cases

- [UC-F20-01 — SIM_ONLY execution](../../use-cases/UC-F20-01-sim-only-execution/use-case.md) (happy path)
- [UC-F20-02 — SHADOW compare](../../use-cases/UC-F20-02-shadow-compare/use-case.md) (SIM vs LIVE)

## Sequences

- L0 (system): [SEQ-UC-F20-01-system](../../use-cases/UC-F20-01-sim-only-execution/sequences/SEQ-UC-F20-01-system.md), [SEQ-UC-F20-02-system](../../use-cases/UC-F20-02-shadow-compare/sequences/SEQ-UC-F20-02-system.md)
- L1 (services): [SEQ-F20-UC-F20-01-services](../../../05-components/sequences/SEQ-F20-UC-F20-01-services.md), [SEQ-F20-UC-F20-02-services](../../../05-components/sequences/SEQ-F20-UC-F20-02-services.md)

## Requirements / domain / contracts / data / tests

- FR: [functional-requirements.md §FR-F20](../../functional-requirements.md#fr-f20-live-venue-simulator) · NFR: [§NFR-F20](../../non-functional-requirements.md#nfr-f20-live-venue-simulator)
- Domain: [entities §Live Venue Simulator](../../../04-domain/entities.md) · rules: [business-rules §F-20](../../../04-domain/business-rules.md)
- Contracts: [messaging/sim-topics.md](../../../06-api/messaging/sim-topics.md) · [rest/sim-sessions-admin-api.md](../../../06-api/rest/sim-sessions-admin-api.md)
- Data: [07-data/sim-sessions.md](../../../07-data/sim-sessions.md) (PG) · [07-data/sim-execution-reports.md](../../../07-data/sim-execution-reports.md) (CH)
- Tests: [10-testing/features/F-20-test-plan.md](../../../10-testing/features/F-20-test-plan.md)
- Plan: [implementation-plan/F-20-live-venue-simulator.tasks.md](../../../implementation-plan/F-20-live-venue-simulator.tasks.md)

## Границы ответственности (не дублировать)

- **F-11** владеет живым LOB / `VenueSnapshot` / `venue.snapshots` / `venue.health` — F-20 их **потребляет**, не модифицирует.
- **F-12** владеет `ExecutionIntent`/child-order-генерацией и форматом `ExecutionReport` — F-20 переиспользует формат и встраивается **между** Venue Execution Adapter и EVC через `VenueSimRouter`.
- **F-20** добавляет: маршрутизацию SIM/LIVE/SHADOW, модели симуляции, sim-книгу, divergence-анализ.

## Conflict Notes

- **CN-F20-01 (labeling).** Источник IN-010 нумерует acceptance-критерии и участника
  sequence как `F16-*` / `F16`. F-16 в проекте — **Operator Console & Kill-Switch**.
  Это опечатка источника; нормализовано в `F-20` / `AC-F20-*`. Оригинальные метки
  сохранены в архиве IN-010 (immutable).
- **CN-F20-02 (proto).** `SimExecutionReport` = `ExecutionReport` (F-12) + sim-поля.
  Требуется backward-compatible расширение `execution.proto` (или отдельный
  `sim_execution.proto`). Решение — ADR при реализации контракта.
- **CN-F20-03 (ledger).** Изолированная sim-книга — новый слой учёта в Ledger;
  `simMode=true` не должен затрагивать боевые позиции (инвариант §17).
- **CN-F20-04 (топики + существующий симулятор).** Базовая имитация подтверждения
  хеджа **уже реализована**: [cpp/venues/src/app/venues_loop.cpp](../../../../cpp/venues/src/app/venues_loop.cpp)
  (Поток-2) читает `execution.intents` и на каждый `ExecutionIntent` сразу публикует
  «filled» `ExecutionReport` в `execution.reports`. F-20 **расширяет** этот наивный
  симулятор (мгновенный полный fill) до live-LOB-симулятора с моделями и SHADOW.
  Кроме того, источник IN-010 называет топик `execution.venue`, тогда как в репозитории
  фактически `execution.intents`/`execution.reports` — привести к существующим именам
  при реализации (документация ниже использует имена источника с этой пометкой).
