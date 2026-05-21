# Компонент: venue-execution-adapter

Адаптер state-machine хеджирования. Принимает `ExecutionIntent` + `RoutingPlan` от Execution Planning, создаёт `HedgeFlow` и `child_orders`, делегирует исполнение External Venues Connector, нормализует raw execution events в `ExecutionReport`, публикует в Kafka `execution.venue`, управляет reconciliation, overfill guard и timeout.

## Implementation Status

Текущая реализация распределена внутри `cpp/venues/` (не выделена как отдельный микросервис). Сравнение целевого vs текущего:

| Целевая ответственность                                   | Реализовано? | Где                                                                                              |
| ---------------------------------------------------------- | ------------ | ------------------------------------------------------------------------------------------------ |
| Consume `execution.intents`                                | ✅           | [`cpp/venues/src/infra/execution_intents_consumer.cpp`](../../../cpp/venues/src/infra/execution_intents_consumer.cpp) |
| Build child intent через `ExecuteOnVenue`                  | ✅           | [`cpp/venues/src/app/execute_on_venue.cpp`](../../../cpp/venues/src/app/execute_on_venue.cpp)     |
| Delegate к `VenueAdapter` implementations                  | ✅           | [`cpp/venues/src/domain/venue_adapter.hpp`](../../../cpp/venues/src/domain/venue_adapter.hpp), implementations в `cpp/venues/src/infra/` |
| Publish ExecutionReport в Kafka                            | ⚠️           | [`cpp/venues/src/infra/execution_report_producer.cpp`](../../../cpp/venues/src/infra/execution_report_producer.cpp) — пишет в legacy topic `execution.reports` |
| `HedgeFlow` state в PostgreSQL                             | ❌           | DDL отсутствует; in-memory state                                                                 |
| `child_orders` в PostgreSQL                                 | ❌           | DDL отсутствует                                                                                  |
| Timeout watchdog (F12-5)                                   | ❌           | нет background-потока                                                                            |
| Overfill guard (F12-4)                                     | ❌           | enum в proto есть, логики нет                                                                    |
| Reconciliation loop                                        | ❌           | не реализовано                                                                                   |
| Idempotency по `client_order_id`                           | ❌           | требует PG persistence                                                                           |
| Routing fallback (F12-12)                                  | ❌           | требует Execution Planning                                                                       |
| Backtest mode через VenueSim                               | ✅           | [`cpp/venues/src/infra/venue_sim_adapter.cpp`](../../../cpp/venues/src/infra/venue_sim_adapter.cpp) |

## Код

- [`cpp/venues/src/app/execute_on_venue.cpp`](../../../cpp/venues/src/app/execute_on_venue.cpp) — основной use case: маппинг ExecutionIntent → child intent, вызов `VenueAdapter::PlaceOrder`, формирование ExecutionReport.
- [`cpp/venues/src/app/venues_loop.cpp`](../../../cpp/venues/src/app/venues_loop.cpp) — top-level loop: connector instance setup, consumer/producer wiring.
- [`cpp/venues/src/infra/execution_intents_consumer.cpp`](../../../cpp/venues/src/infra/execution_intents_consumer.cpp) — Kafka consumer для `execution.intents`.
- [`cpp/venues/src/infra/execution_report_producer.cpp`](../../../cpp/venues/src/infra/execution_report_producer.cpp) — Kafka producer для `execution.venue` (текущее имя — `execution.reports`; legacy).
- [`cpp/venues/src/domain/venue_adapter.hpp`](../../../cpp/venues/src/domain/venue_adapter.hpp) — интерфейс venue adapter.
- Implementations в `cpp/venues/src/infra/`:
  - [`cex_ws_rest_adapter.cpp`](../../../cpp/venues/src/infra/cex_ws_rest_adapter.cpp) — Binance/Coinbase REST/WS.
  - [`dex_amm_rpc_adapter.cpp`](../../../cpp/venues/src/infra/dex_amm_rpc_adapter.cpp) — DEX/AMM JSON-RPC.
  - [`venue_sim_adapter.cpp`](../../../cpp/venues/src/infra/venue_sim_adapter.cpp) — backtest VenueSim.
  - [`simulated_venue_adapter.cpp`](../../../cpp/venues/src/infra/simulated_venue_adapter.cpp) — MVP симулятор (instant fill).

## Конфигурация

| env                          | Default              | Назначение                                                |
| ---------------------------- | -------------------- | --------------------------------------------------------- |
| `KAFKA_BROKERS`              | redpanda:9092        | Kafka                                                     |
| `VENUE_EXECUTION_GROUP`      | venue-execution-adapter | consumer group для execution.intents                  |
| `VENUE_EXECUTION_TOPIC_IN`   | execution.intents    | input topic                                               |
| `VENUE_EXECUTION_TOPIC_OUT`  | execution.reports    | (планируется switch на `execution.venue`)                |
| `VENUES_POSTGRES_DSN`        | —                    | (планируется) DSN для hedgeflows / child_orders          |
| `HEDGE_TIMEOUT_MS_DEFAULT`   | 5000                 | дефолтный timeout (override через ExecutionIntent.timeout_ms) |
| `OVERFILL_THRESHOLD_BPS`     | 50                   | 0.5% от targetQty                                         |
| `RECONCILIATION_GAP_BPS`     | 1                    | 0.01% от targetQty                                        |

## State machine HedgeFlow

```text
        ┌────── PreHedgeCheck REJECT ──────┐
        │                                  ▼
   ┌─────────┐                       ┌─────────────┐
   │  OPEN   │ ────── timeout ─────▶ │ UNDERFILLED │
   └────┬────┘                       └─────────────┘
        │                                  ▲
        │ all child FILLED                 │ gap > threshold
        ▼                                  │
   ┌──────────┐                            │
   │COMPLETED │ ◀── gap ≤ threshold ───────┘
   └──────────┘
        │
        │ overfill detected
        ▼
   ┌──────────────┐
   │ OVERFILL_GRD │  (terminal; status в child_orders=CANCELLED)
   └──────────────┘

   ┌──────────┐
   │ REJECTED │  ◀── all venues unavailable / EVC error
   └──────────┘

   ┌────────────────┐
   │ RISK_REJECTED  │  ◀── RiskService.PreHedgeCheck=REJECT
   └────────────────┘

   ┌────────────┐
   │ CANCELLED  │  ◀── operator manual cancel (REST POST /hedge/flows/{id}/cancel)
   └────────────┘
```

## Что должно быть в проде

- PostgreSQL persistence (`hedgeflows`, `child_orders`).
- Timeout watchdog (отдельный thread / scheduled).
- Overfill detection в hot path обработки ExecutionReport.
- Полная reconciliation loop с retry-планированием через Execution Planning.
- Switch на канонический topic `execution.venue`.
- Идемпотентный consumer (commit offset после успешной обработки + idempotency по client_order_id).
- Metrics: `hedge_latency_ms`, `slippage_bps`, `fill_rate`, `cancel_rate`, `overfill_count`.

## Связанные фичи

- F-12 (Execution Hedge) — основная фича.
- F-11 (External Venues LOB → FOB) — поставщик venue.liquidity.fob (через Planning).
- F-15 (Backtest / Replay) — параллельный path через VenueSim.

## Participates In Features

- [F-12](../../02-system/features/F-12-execution-hedge/)

## Participates In Use Cases

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md)
- [UC-F12-02](../../02-system/use-cases/UC-F12-02-manual-operator-hedge/use-case.md)
- [UC-F12-03](../../02-system/use-cases/UC-F12-03-partial-fill-retry/use-case.md)
- [UC-F12-04](../../02-system/use-cases/UC-F12-04-rejection-fallback/use-case.md)
- [UC-F12-05](../../02-system/use-cases/UC-F12-05-timeout-underfilled-reconciliation/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F12-01-auto-hedge-services](../sequences/SEQ-F12-01-auto-hedge-services.md)
- [SEQ-F12-02-rejection-fallback-services](../sequences/SEQ-F12-02-rejection-fallback-services.md)
- [SEQ-F12-03-error-scenarios-services](../sequences/SEQ-F12-03-error-scenarios-services.md)

## Owned Contracts

- `fob.execution.v1.ExecutionReport`, `fob.hedge.v1.HedgeFlow`, `fob.hedge.v1.ChildOrder` (writer)

## Produced Events

- [execution.venue](../../06-api/messaging/execution-venue.md)
- [risk.alerts](../../06-api/messaging/risk-alerts.md) (HEDGE_UNDERFILL, HEDGE_STUCK)

## Consumed Events

- [execution.intents](../../06-api/messaging/execution-intents.md)

## Data Access

- [hedgeflows](../../07-data/hedgeflows.md) — PostgreSQL writer.
- [child_orders](../../07-data/child-orders.md) — PostgreSQL writer.
- [execution_reports](../../07-data/execution-reports.md) — ClickHouse writer (через Kafka).
