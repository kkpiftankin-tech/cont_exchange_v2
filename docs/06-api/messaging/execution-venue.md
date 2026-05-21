# Kafka Topic: execution.venue

## Purpose

Отчёты об исполнении (`ExecutionReport`) от Venue Execution Adapter после нормализации raw execution events от внешних площадок. Топик является источником истины о фактическом исполнении хеджа для Ledger (HedgePnL), Risk Manager (post-hedge exposure), ClickHouse (audit/replay) и Observability.

> **Note (canonical name):** IN-005 §9 канонизирует имя `execution.venue`. В текущем `infra/kafka/create_topics.sh` параллельно существует legacy-топик `execution.reports`, который сохраняется для обратной совместимости в переходный период. Новые consumers должны подписываться на `execution.venue`. См. [topics.md](topics.md) для статуса миграции.

## Producer

- [venue-execution-adapter](../../05-components/venue-execution-adapter/overview.md) — формирует ExecutionReport из raw execution events.

## Consumers

- [ledger](../../05-components/ledger/overview.md) — обновляет positions, считает HedgePnL.
- [risk-manager](../../05-components/risk-manager/overview.md) — post-hedge exposure check.
- [observability-reporting](../../05-components/observability-reporting/overview.md) — lifecycle logging, latency/slippage metrics.
- ClickHouse `execution_reports` — через Kafka engine + materialized view.
- [backtest-replay](../../05-components/backtest-replay/overview.md) — replay для F-15.

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней (Kafka); ClickHouse хранит 90+ дней — `infra/kafka/create_topics.sh:60` |
| Partition key | `hedge_flow_id` |
| Delivery | at-least-once, idempotent consumer (key=`(hedge_flow_id, child_order_id, report_id)`) |
| Schema | `fob.execution.v1.ExecutionReport` (Protobuf) |
| Partitions | 3 (dev), увеличить в prod |

## Message Schema

См. [`contracts/proto/fob/execution/v1/execution.proto`](../../../contracts/proto/fob/execution/v1/execution.proto), сообщение `ExecutionReport`.

Ключевые поля:

| Поле | Тип | Описание |
| --- | --- | --- |
| `meta` | `fob.common.v1.EventMeta` | event_id, event_time, source, correlation_id |
| `report_id` | `string` (UUID) | идемпотентный ID отчёта |
| `intent_id` | `string` | back-ref к ExecutionIntent |
| `hedge_flow_id` | `string` | сессия хеджа |
| `child_order_id` | `string` | конкретный child order |
| `batch_id` | `string` | связь с F-04 BatchResult |
| `provider_id` | `string` | |
| `venue` | `string` | venue ID (binance, coinbase, uniswap_v3, venue_sim, ...) |
| `instrument` | `fob.common.v1.Instrument` | |
| `venue_symbol` | `string` | venue-specific symbol |
| `side` | `fob.common.v1.Side` | |
| `venue_order_id` | `string` | ID на стороне venue (для cancel/status) |
| `client_order_id` | `string` | idempotency на venue |
| `status` | `ExecutionReportStatus` enum | NEW / PARTIALLY_FILLED / FILLED / CANCELLED / REJECTED / EXPIRED / OVERFILL_GUARD / UNDERFILLED |
| `filled_qty` | `fob.common.v1.Decimal` | заполнено в этом отчёте |
| `remaining_qty` | `fob.common.v1.Decimal` | targetQty - cumulativeFilled |
| `average_price` | `fob.common.v1.Decimal` | средневзвешенная по этому child order |
| `slippage_bps` | `int32` | `|avgPrice - referenceMid| / referenceMid × 10^4` |
| `reference_mid` | `fob.common.v1.Decimal` | для расчёта slippage и HedgePnL |
| `hedge_pnl` | `fob.common.v1.Money` | реализованный PnL по child order (вычисляется Adapter или Ledger) |
| `trades` | `repeated ExecutionTrade` | детальный список trades (trade_id, ts, price, amount, cost, fee, maker) |
| `fee_total` | `fob.common.v1.Fee` | суммарный fee |
| `error` | `fob.common.v1.Error` | code + message при REJECTED |

## Idempotency

- Consumer должен дедуплицировать по `(hedge_flow_id, child_order_id, report_id)`.
- Повторная доставка одного и того же отчёта не должна изменять `positions` в Ledger или `execution_reports` в ClickHouse.

## ClickHouse Ingestion

Kafka engine table читает `execution.venue`, MV агрегирует в MergeTree `execution_reports`. См. [07-data/execution-reports.md](../../07-data/execution-reports.md).

## Used In Features

- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/) — primary.
- [F-06. Positions / PnL / Margin](../../02-system/features/F-06-positions-pnl-margin/) — обновление positions.
- [F-13. Post-Trade Report](../../02-system/features/F-13-posttrade-report/) — VWAP, IS reports.
- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/) — parity replay.
- [F-17. Monitoring](../../02-system/features/F-17-monitoring-and-alerts/) — alerts on slippage/fill rate.

## Used In Use Cases

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md)
- [UC-F12-03](../../02-system/use-cases/UC-F12-03-partial-fill-retry/use-case.md)
- [UC-F12-04](../../02-system/use-cases/UC-F12-04-rejection-fallback/use-case.md)
- [UC-F12-05](../../02-system/use-cases/UC-F12-05-timeout-underfilled-reconciliation/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F12-01-auto-hedge-services](../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md)
- [SEQ-F12-02-rejection-fallback-services](../../05-components/sequences/SEQ-F12-02-rejection-fallback-services.md)
- [SEQ-F12-03-error-scenarios-services](../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md)

## Source Fragments

- IN-005 §1 «ExecutionReports (ClickHouse execution_reports)»
- IN-005 §2 «Sequence diagram — основной happy path» (publish ExecutionReport)
- IN-005 §6 «slippageBps», «HedgePnL»
- IN-005 §9 «Kafka topics» (canonical name `execution.venue`)
