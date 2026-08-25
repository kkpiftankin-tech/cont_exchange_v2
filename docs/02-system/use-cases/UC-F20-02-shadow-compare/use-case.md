# UC-F20-02 — SHADOW compare (параллельное SIM vs LIVE)

- **Feature:** [F-20 Live Venue Simulator](../../features/F-20-live-venue-simulator/README.md)
- **Actors:** Оператор/инженер (анализирует расхождение), Continuous Exchange System (F-12 инициатор child-ордера).
- **External systems:** внешняя площадка (получает **реальный** LIVE-ордер в этом режиме).
- **Trigger:** `ChildOrderRequest` для `venueId+symbol` с активной `SimSession`, `routingMode=SHADOW`.
- **Preconditions:** живой LOB (F-11), `VenueSimulator` со свежим кэшем, доступный EVC (F-11) для LIVE-ветки.
- **Goal:** для каждого child-ордера получить **два** отчёта (LIVE + SIM) и зафиксировать delta-метрики для калибровки моделей.

## Основной поток

1. F-12 → `VenueSimRouter`: `ChildOrderRequest`.
2. `VenueSimRouter` (`routingMode=SHADOW`) делает **fork**:
   - **LIVE fork:** → EVC → реальная площадка → `ExecutionReport{simMode=false}`.
   - **SIM fork:** → `VenueSimulator` → `SimExecutionReport{simMode=true}`.
3. Оба отчёта публикуются в `execution.venue`.
4. `Divergence Service` сопоставляет пару по `clientOrderId`: `delta fillRate`, `delta avgPrice (bps)`, `delta latency`, `delta fee`.
5. Запись в ClickHouse `sim_divergence_log`.
6. При `divergence > threshold` — алерт оператору (перекалибровать модели).

## Postconditions

- Ровно 2 отчёта на child-ордер (AC-F20-02); LIVE меняет боевые позиции, SIM — sim-книгу.
- `sim_divergence_log` содержит запись по `clientOrderId` (AC-F20-10).

## Acceptance

AC-F20-02, AC-F20-05 (изоляция sim-ветки), AC-F20-10.

## Links

- L0: [SEQ-UC-F20-02-system](sequences/SEQ-UC-F20-02-system.md) · L1: [SEQ-F20-UC-F20-02-services](../../../05-components/sequences/SEQ-F20-UC-F20-02-services.md)
- Data: [sim-execution-reports](../../../07-data/sim-execution-reports.md) (`sim_divergence_log`)
