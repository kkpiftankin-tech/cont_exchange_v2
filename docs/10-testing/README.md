# 10-testing

## Назначение

Стратегия тестирования, инструменты, тестовые данные и acceptance-критерии.

## Состав

- [test-strategy.md](test-strategy.md) — пирамида тестов, виды, инструменты.
- [features/](features/) — per-feature планы тестирования (юнит-тесты, интеграция, SLA, ручные сценарии). Полный индекс — ниже.

## Per-feature test plans

| Feature | План | Статус |
| --- | --- | --- |
| F-01 Auth and Identity | (нет файла) | planned |
| F-02 Create FlowOrder | (нет файла) | planned |
| F-03 Order Lifecycle | (нет файла) | planned |
| F-04 Batch Clearing | [features/F-04-test-plan.md](features/F-04-test-plan.md) | draft (IN-003: U1–U10 + SLA + integration + manual) |
| F-05 Live Market Data | (нет файла) | planned |
| F-06 Positions / PnL / Margin | (нет файла) | planned |
| F-07 Pre-Trade Risk | (нет файла) | planned |
| F-08 Post-Trade Risk and Liquidations | (нет файла) | planned |
| F-09 Batch / Combo Orders | (нет файла) | planned |
| F-10 MM Curves | (нет файла) | planned |
| F-11 External Venues LOB→FOB | (нет файла) | planned |
| F-12 Execution Hedge | (нет файла) | planned |
| F-13 Post-Trade Report | (нет файла) | planned |
| F-14 Deposit / Withdraw | (нет файла) | planned |
| F-15 Backtest / Replay | (нет файла) | planned |
| F-16 Operator Console | (нет файла) | planned |
| F-17 Monitoring and Alerts | (нет файла) | planned |

Coverage по тестам отражается в [../traceability/coverage-matrix.md](../traceability/coverage-matrix.md) (колонка `Tests`).

## Acceptance-критерии

Источник истины — блок `acceptance:` в `feature.yaml` каждой фичи (см. [test-strategy.md §Acceptance](test-strategy.md#acceptance-критерии)). Расширенные блоки `Acceptance Criteria (IN-001/IN-003)` — в `README.md` соответствующей фичи (например, [F-04](../02-system/features/F-04-batch-clearing/README.md)).

Отдельный каталог `10-testing/acceptance/` намеренно не создаётся — навигация идёт через [../02-system/features/](../02-system/features/).

## Test data / fixtures

Fixture-таблицы (например, U1–U10 для солвера F-04) живут внутри per-feature планов: см. [features/F-04-test-plan.md §1](features/F-04-test-plan.md). Отдельного `test-data.md` или каталога `data/` нет; при необходимости вынести fixtures в JSON/YAML — создавать `features/F-XX-fixtures/`.

## Связанные разделы

- [../02-system/features/](../02-system/features/) — фичи и их acceptance-критерии (источник истины).
- [../../tests/](../../tests/) — исполняемые тесты (`contract/`, `e2e/`, `integration/`, `performance/`, `replay/`, `unit/`).
- [../11-operations/runbook.md](../11-operations/runbook.md) — рантайм-проверки.
- [../traceability/coverage-matrix.md](../traceability/coverage-matrix.md) — статусы покрытия по фичам.
