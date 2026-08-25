# UC-F20-01 — SIM_ONLY execution (симулированное исполнение child-ордера)

- **Feature:** [F-20 Live Venue Simulator](../../features/F-20-live-venue-simulator/README.md)
- **Actors:** Оператор (создаёт SimSession), Continuous Exchange System (F-12 Venue Execution Adapter как инициатор child-ордера).
- **External systems:** внешняя площадка (только как источник **живого** LOB через F-11; ордера на неё НЕ уходят).
- **Trigger:** F-12 сформировал `ChildOrderRequest`, для `venueId+symbol` активна `SimSession` с `routingMode=SIM_ONLY`.
- **Preconditions:**
  - F-11 боевым образом принимает живой LOB и публикует `VenueSnapshot` в `venue.snapshots`.
  - `SimSession` создана и `status=ACTIVE`, `VenueSimulator` держит свежий LOB-кэш.
- **Goal:** получить реалистичный синтетический `SimExecutionReport` без реального выхода на рынок; sim-книга обновлена, боевые позиции не затронуты.

## Основной поток

1. F-12 Venue Execution Adapter → `VenueSimRouter`: `ChildOrderRequest{childOrderId, hedgeFlowId, venueId, symbol, side, orderType, qty, price?, simSessionId}`.
2. `VenueSimRouter` определяет `routingMode=SIM_ONLY` → передаёт запрос в `VenueSimulator` (EVC не вызывается).
3. `VenueSimulator` загружает актуальный `VenueSnapshot` из LOB-кэша, проверяет `lobAge < staleLobThresholdMs`.
4. LEVEL_BY_LEVEL matching по нужной стороне стакана → `filledQty`, набор fills; VWAP `avgPrice`.
5. `ImpactModel` → сдвиг цены `Δp`, `avgPrice_impact`, `impactBps`.
6. `FeeModel` → `fee`. `RejectionModel` → проверка условий отказа.
7. `LatencyModel` → сэмпл `latencySampleMs`; async-задержка публикации.
8. `VenueSimulator` публикует `SimExecutionReport{simMode=true, …}` в `execution.venue` (+ дубль в `sim.execution.venue`).
9. Settlement Ledger видит `simMode=true` → обновляет **изолированную sim-книгу** (боевые позиции не меняются).
10. ClickHouse сохраняет отчёт в `sim_execution_reports` (≤ 1 сек).

## Альтернативные / ошибочные потоки

- **Stale LOB** (`lobAge > staleLobThresholdMs`): `status=REJECTED, rejectReason=SIM_STALE_LOB`; алерт `SIM_STALE_LOB` в `sim.alerts`; реальных ордеров нет. F-12 обрабатывает REJECTED штатным fallback.
- **Нет ликвидности** (LOB пуст / `filledQty=0` при `insufficientLiquidityEnabled`): `REJECTED, SIM_NO_LIQUIDITY`.
- **Random / rate-limit reject** (`RejectionModel`): `REJECTED, SIM_RANDOM_REJECT`.
- **Latency timeout** (`sample > timeoutMs`): `REJECTED, SIM_TIMEOUT`.
- **Overfill guard** (`filledQty > targetQty`): trim до `targetQty`, `FILLED` с `trimmedQty`.
- **`venue.snapshots` недоступна**: NO_LOB режим — все ордера `SIM_STALE_LOB`, алерт `SIM_LOB_SOURCE_DOWN`.

## Postconditions

- `SimExecutionReport` опубликован (`simMode=true`), связан с `lobSnapshotId`.
- sim-книга обновлена; боевые позиции неизменны (AC-F20-05).
- Отчёт в ClickHouse.

## Acceptance (см. feature.yaml)

AC-F20-01, AC-F20-03, AC-F20-04, AC-F20-05, AC-F20-06, AC-F20-08, AC-F20-09.

## Links

- System sequence (L0): [SEQ-UC-F20-01-system](sequences/SEQ-UC-F20-01-system.md)
- Service sequence (L1): [SEQ-F20-UC-F20-01-services](../../../05-components/sequences/SEQ-F20-UC-F20-01-services.md)
- Contracts: [sim-topics](../../../06-api/messaging/sim-topics.md), [sim-sessions-admin-api](../../../06-api/rest/sim-sessions-admin-api.md)
- Data: [sim-execution-reports](../../../07-data/sim-execution-reports.md), [sim-sessions](../../../07-data/sim-sessions.md)
