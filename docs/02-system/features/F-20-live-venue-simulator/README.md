---
id: F-20-live-venue-simulator
phase: 02-system
status: draft
owner: core-team
---

# F-20 — Live Venue Simulator

## Что это

Гибридный режим исполнения: живой LOB из F-11 + симулятор исполнения
вместо реальных биржевых ордеров. Включает три режима маршрутизации:

- **SIM_ONLY** — pre-production проверка на живых данных без выхода на рынок.
- **LIVE_ONLY** — обычный F-12 поток без участия симулятора (overhead <= 5 мс).
- **SHADOW** — dual-send: реальный ордер + параллельная симуляция; сравнение
  через Divergence Service.

## Conflict Note (feature ID)

Исходная спека внутри тела ссылается на ID "F-16", но в этом репозитории
**F-16 уже занят** под "Operator Console & Kill-Switch"
(см. [feature-index.md](../feature-index.md)). Имя файла спеки
("F-20 -Live-Venue-Simulator.md") — авторитетный ID.
**Live Venue Simulator зарегистрирован как F-20**.

## Источники

- **Спека**: [`incoming-docs/2026-05-26-F-20-live-venue-simulator-v1.md`](../../../../incoming-docs/2026-05-26-F-20-live-venue-simulator-v1.md)
  (placeholder; mojibake unrecoverable — структурный контент восстановлен).
- **Ингест-meta**: [`incoming-docs/IN-010.meta.md`](../../../../incoming-docs/IN-010.meta.md)
- **Fragment map**: [`incoming-docs/IN-010.fragment-map.md`](../../../../incoming-docs/IN-010.fragment-map.md)

## Статус

`draft` — только документация. На момент создания этого README:

- 19 DoD пунктов — все `not-implemented`.
- 12 acceptance criteria — все `not-tested`.
- 5 UX экранов — все запланированы.
- Кода (cpp/venues/src/app/venue_simulator.*, и т.д.) пока нет.

Реализация разбита на фазы — см. [implementation-plan](../../../implementation-plan/F-20-live-venue-simulator.tasks.md).

## Cross-feature

- **F-11** (External Venues LOB → FOB) — поставщик `venue.snapshots`;
  F-20 потребитель без модификаций F-11.
- **F-12** (Execution Hedge) — поставщик `ChildOrderRequest`; F-20
  встраивает `VenueSimRouter` между Venue Execution Adapter и External
  Venues Connector. F-12 не модифицируется (только Ledger расширяется
  под sim-book — это shared concern).
- **F-15** (Backtest / Replay) — переиспользует форматы `LatencyModel`
  и `FeeModel`. Не зависимость, а соглашение по конфигу.

## Open questions

Все открытые вопросы зарегистрированы как `knownIssues` в
[feature.yaml](feature.yaml#knownIssues). Перед началом реализации
требуются:

- ADR по выбору single-topic (`execution.venue` с simMode) vs dual-topic
  (`sim.execution.venue` mirror).
- Решение по Ledger sim-book: новые таблицы vs namespace в существующих.
- Решение по сосуществованию с legacy `cpp/venues/src/infra/venue_sim_adapter.*`.

## DoD progress

См. `definitionOfDone` в [feature.yaml](feature.yaml#definitionOfDone).
Прогресс обновляется по мере реализации в PR.
