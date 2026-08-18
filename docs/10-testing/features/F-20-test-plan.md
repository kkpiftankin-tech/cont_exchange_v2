---
id: DOC-TEST-F-20
phase: 10-testing
status: draft
owner: core-team
source:
  - IN-010 §5 (нагрузочное тестирование), §6 (DoD), §2.6 (Acceptance), §4.4 (алгоритм)
related:
  - docs/02-system/features/F-20-live-venue-simulator/feature.yaml
  - docs/05-components/sequences/SEQ-F20-UC-F20-01-services.md
  - docs/05-components/sequences/SEQ-F20-UC-F20-02-services.md
  - docs/06-api/messaging/sim-topics.md
  - docs/07-data/sim-execution-reports.md
---

# F-20 Live Venue Simulator — план тестирования

Источник: [IN-010](../../../incoming-docs/IN-010.meta.md) (immutable-архив `incoming-docs/F-20 -Live-Venue-Simulator.md`).

> Легенда: ✅ выполнено, ⚠ частично, ❌ не выполнено.
>
> F-20 в статусе `planned` (расширение существующего наивного симулятора
> `venues_loop.cpp`) — все тесты ❌ (запланированы).

## Раздел 1. Unit-тесты VenueSimulator (U1–U10)

Чистая логика matching/моделей без Kafka/PG. Файлы — `cpp/venues/tests/`.

| # | Тест | Цель / инвариант | AC |
| --- | --- | --- | --- |
| U1 | HappyPathFilled | LEVEL_BY_LEVEL FILLED; `filledQty`/`avgPrice` корректны | AC-F20-08 |
| U2 | PartialFill | частичное исполнение при недостатке ликвидности | AC-F20-08 |
| U3 | StaleLob | `lobAge > threshold` → REJECTED/SIM_STALE_LOB (R-F20-002) | AC-F20-04 |
| U4 | LevelByLevelPrecision | ошибка объёма ≤ 1% при идентичном LOB | AC-F20-08 |
| U5 | ImpactLinear | LINEAR `Δp=α·qty`, `impactBps` корректен | AC-F20-08 |
| U6 | ImpactSqrt | SQRT `Δp=α·√qty` | AC-F20-08 |
| U7 | FeeMakerTaker | `fee = filledQty·avgPrice·feeRate` (maker/taker) | — |
| U8 | RejectionRandomPrice | random reject + price-constraint reject | — |
| U9 | LatencySampling | сэмпл из распределения; `sample>timeoutMs`→SIM_TIMEOUT | AC-F20-09 |
| U10 | ShadowForkIsolation + OverfillGuard | SIM/LIVE изоляция; trim `filledQty>targetQty` | AC-F20-02 |

## Раздел 2. Интеграционные тесты (IT-1..IT-5)

| # | Сценарий | Критерий | AC |
| --- | --- | --- | --- |
| IT-1 | SIM_ONLY полный цикл (venue.snapshots→sim report→CH) | нет вызовов EVC; simMode=true; запись в CH ≤ 1с | AC-F20-01/06 |
| IT-2 | SHADOW dual-report + divergence | ровно 2 отчёта; запись в `sim_divergence_log` | AC-F20-02/10 |
| IT-3 | Stale LOB + alert | REJECTED/SIM_STALE_LOB ≤ 200мс; алерт в sim.alerts | AC-F20-04 |
| IT-4 | Hot reload при нагрузке | применение ≤ 500мс без потери ордеров | AC-F20-11 |
| IT-5 | SIM→LIVE атомарное переключение | следующие ордера в EVC; session=COMPLETED | AC-F20-07 |

## Раздел 3. Изоляция sim-книги (Ledger)

| # | Тест | Инвариант |
| --- | --- | --- |
| L1 | SimReportNoRealPositionChange | `simMode=true` не меняет боевые позиции (R-F20-001, AC-F20-05) |

## Раздел 4. Нагрузочные (LT-1..LT-4) + SLA

| # | Сценарий | Критерий |
| --- | --- | --- |
| LT-1 | 500 child/сек, SIM_ONLY, 5 мин | p95 latency < 50мс; нет ошибок Kafka publish (AC-F20-09) |
| LT-2 | 200 child/сек, SHADOW, 5 мин | оба отчёта доставлены; LIVE delta overhead p95 < 10мс |
| LT-3 | LOB freeze 5 сек | все ордера SIM_STALE_LOB ≤ 200мс |
| LT-4 | Hot reload при 300/сек | применение ≤ 500мс, без потери ордеров |
| SLA | latency по этапам | p50/p95/p99 соответствуют TRD 5.3; overhead p95 < 50мс |

## Раздел 5. Регрессия существующего симулятора

- R1: базовый intent→report путь (`venues_loop.cpp`) продолжает работать как `LIVE_ONLY`/fallback, если SimSession не активна (AC: error-flow «SimSession не активна» → LIVE).

## Трассировка

- Feature: [F-20](../../02-system/features/F-20-live-venue-simulator/README.md)
- UC: [UC-F20-01](../../02-system/use-cases/UC-F20-01-sim-only-execution/use-case.md), [UC-F20-02](../../02-system/use-cases/UC-F20-02-shadow-compare/use-case.md)
- Rules: [business-rules §F-20](../../04-domain/business-rules.md)
- Plan: [F-20 tasks](../../implementation-plan/F-20-live-venue-simulator.tasks.md)
