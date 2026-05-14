# Компонент: matching

Сервис непрерывного клиринга. Читает `orders.normalized` из Kafka, поддерживает реестр активных ордеров, периодически запускает solver и публикует `BatchResult` в `batch.outputs`.

## Код

- [cpp/matching/](../../../cpp/matching/) — каталог
- [src/main.cpp](../../../cpp/matching/src/main.cpp) — entrypoint
- [src/app/matching_loop.cpp](../../../cpp/matching/src/app/matching_loop.cpp) — два потока: consumer + batch timer
- [src/domain/solver.hpp](../../../cpp/matching/src/domain/solver.hpp) — placeholder интерфейс для будущего LP/QP solver

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `KAFKA_BROKERS` | redpanda:9092 | Kafka |
| `BATCH_INTERVAL_MS` | 1000 | Период работы солвера |

## Текущая реализация (MVP-симулятор, НЕ настоящий solver)

[matching_loop.cpp:148-203](../../../cpp/matching/src/app/matching_loop.cpp#L148-L203) — для каждого ордера независимо:
- `dq = min(remaining_qty, max_speed * batch_interval_ms)`
- `price = midpoint(price_low, price_high)`
- `notional = dq * price`

Затем по символу считается среднее midpoint'ов и публикуется как `clear_prices`.

> **Это не F-04 в полном смысле.** F-04 предписывает единую клиринговую цену на инструмент через решение системы `D(p) = 0`. Гэпы перечислены в [features/F-04-batch-clearing/](../../02-system/features/F-04-batch-clearing/).

## Связанные фичи

- F-04 (Batch Clearing Cycle) — основная фича
- F-09 (Batch and Combo Orders) — multi-leg, не реализовано
- F-11 (External Venues LOB → FOB) — не реализовано
- F-12 (Execution Hedge) — не реализовано

## Известные несоответствия F-04

1. **Источник входа: Kafka, а не PostgreSQL `floworders`.**
2. **Нет вызова Market Data Service** за reference prices.
3. **Цена fill — per-order midpoint**, не единая клиринговая.
4. **`residualNorm = 0.0` и `solveTimeMs = 1` захардкожены.**
5. **`executed_rates` показывает `max_speed`**, а не фактическую скорость.
6. **Нет fallback при non-convergence, нет kill-switch при SLA breach.**
7. **Нет multi-leg / portfolio orders.**
8. **Нет полей `liquidity_source`, `fees`, `fill_id` в Fill.**
9. **Один Kafka топик `batch.outputs` вместо двух (нет `fills`).**

Полный анализ — в [F-04-batch-clearing/](../../02-system/features/F-04-batch-clearing/).

## Participates In Features

- [F-02](../../02-system/features/F-02-create-floworder/), [F-03](../../02-system/features/F-03-order-lifecycle/), [F-04](../../02-system/features/F-04-batch-clearing/), [F-08](../../02-system/features/F-08-posttrade-risk-and-liquidations/), [F-09](../../02-system/features/F-09-batch-combo-orders/), [F-10](../../02-system/features/F-10-mm-curves/), [F-11](../../02-system/features/F-11-external-venues-lob-to-fob/), [F-12](../../02-system/features/F-12-execution-hedge/), [F-15](../../02-system/features/F-15-backtest-replay/), [F-16](../../02-system/features/F-16-operator-console/)

## Participates In Use Cases

- [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md), [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md), [UC-F04-01](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md), [UC-F08-01](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md), [UC-F09-01](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md), [UC-F10-01](../../02-system/use-cases/UC-F10-01-publish-mm-curve/use-case.md), [UC-F11-01](../../02-system/use-cases/UC-F11-01-ingest-external-marketdata/use-case.md), [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md), [UC-F15-01](../../02-system/use-cases/UC-F15-01-replay-historical-batch/use-case.md), [UC-F16-01](../../02-system/use-cases/UC-F16-01-trigger-kill-switch/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F03-UC-F03-01-services](../sequences/SEQ-F03-UC-F03-01-services.md), [SEQ-F04-UC-F04-01-services](../sequences/SEQ-F04-UC-F04-01-services.md), [SEQ-F08-UC-F08-01-services](../sequences/SEQ-F08-UC-F08-01-services.md), [SEQ-F09-UC-F09-01-services](../sequences/SEQ-F09-UC-F09-01-services.md), [SEQ-F10-UC-F10-01-services](../sequences/SEQ-F10-UC-F10-01-services.md), [SEQ-F11-UC-F11-01-services](../sequences/SEQ-F11-UC-F11-01-services.md), [SEQ-F12-UC-F12-01-services](../sequences/SEQ-F12-UC-F12-01-services.md), [SEQ-F15-UC-F15-01-services](../sequences/SEQ-F15-UC-F15-01-services.md), [SEQ-F16-UC-F16-01-services](../sequences/SEQ-F16-UC-F16-01-services.md)

## Owned Contracts

- `fob.matching.v1.BatchResult`, `Fill`, `OrderUpdate`, `BatchDiagnostics` — [../../06-api/grpc/](../../06-api/grpc/)

## Produced Events

- [batch.outputs](../../06-api/messaging/batch-outputs.md)
- (planned) [execution.intents](../../06-api/messaging/execution-intents.md)

## Consumed Events

- [orders.normalized](../../06-api/messaging/orders-normalized.md)

## Data Access

- (planned) `solver_config`, `flow_orders` (snapshot) — [../../07-data/data-overview.md](../../07-data/data-overview.md)
- (planned) `fills`, `batch_results` (write to ClickHouse via observability)
