# Domain Business Rules

Инварианты и бизнес-правила доменной модели. Правила именуются `R-<AREA>-NNN`.

## F-20 — Live Venue Simulator

Источник: IN-010. Feature — [F-20](../02-system/features/F-20-live-venue-simulator/README.md).
Базовый симулятор подтверждения хеджа уже реализован (`cpp/venues/src/app/venues_loop.cpp`,
intent→filled report); правила ниже — для расширенного live-LOB-симулятора.

### R-F20-001 Sim-book isolation (инвариант)

`SimExecutionReport` с `simMode=true` **никогда** не изменяет боевые позиции/PnL
провайдера. Ledger применяет sim-отчёты только к **изолированной sim-книге**. Нарушение
= порча боевого состояния (ср. §17: no phantom inventory для реальной книги).

### R-F20-002 Live-LOB source & staleness

Симуляция считается только на **актуальном** `VenueSnapshot`. Если
`lobAge = now − snapshot.timestamp > staleLobThresholdMs` — исполнение **не
рассчитывается**; результат `REJECTED / SIM_STALE_LOB`, алерт `SIM_STALE_LOB`. Каждый
sim-отчёт обязан ссылаться на `lobSnapshotId` (провенанс/аудит).

### R-F20-003 SHADOW dual-report

В `routingMode=SHADOW` каждый `ChildOrderRequest` порождает **ровно два** отчёта:
LIVE (`simMode=false`, реальное исполнение, боевая книга) и SIM (`simMode=true`,
sim-книга). Divergence считается по паре с одним `clientOrderId`.

### R-F20-004 Routing exclusivity

Для одного `venueId+symbol` в момент времени действует ровно один `routingMode`
(`SIM_ONLY` | `LIVE_ONLY` | `SHADOW`). В `SIM_ONLY` реальных вызовов EVC для
child-ордеров нет; в `LIVE_ONLY` симулятор не вызывается. Переключение — атомарно
(≤ 500 мс), без потери ордеров.

### R-F20-005 Sim parity (контрактная идентичность)

Логика симуляции fill/reject использует тот же контракт `ExecutionReport`, что и боевое
исполнение (F-12); отличие — только маркер `simMode` и sim-поля трассировки. Downstream
(Ledger/Risk/CH) не должен требовать спец-логику, кроме ветвления по `simMode`.

### R-F20-006 Reject taxonomy

Симулированные отказы — фиксированный набор: `SIM_STALE_LOB`, `SIM_NO_LIQUIDITY`,
`SIM_RANDOM_REJECT`, `SIM_TIMEOUT` (+ `SIM_LOB_SOURCE_DOWN` как алерт). F-12
обрабатывает их штатной fallback-логикой REJECTED.

### R-F20-007 LOB depletion корректность

`depletionMode` (поедание уровней стакана) корректен только для **серийных** ордеров по
одному инструменту; вне этого сценария depletion не применяется (ограничение точности).

## Source Fragments

- IN-010 §1–§4 — F-20 модели, режимы, изоляция, stale-LOB, SHADOW divergence.
