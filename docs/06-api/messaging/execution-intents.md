# Kafka Topic: execution.intents

## Purpose

Намерения хеджирования. После batch clearing (F-04) или ручного запуска оператором формируется `ExecutionIntent`, описывающий цель: какой объём, в каком направлении, с какой urgency и в каких venues исполнить. Топик является входом для Execution Planning.

## Producer

- [matching-fob-core](../../05-components/matching-fob-core/overview.md) — auto-hedge after batch (через `HedgeExecutionIntentsPublisher`).
- [gateway](../../05-components/gateway/overview.md) — manual operator override (POST `/api/v1/hedge/intents/manual`, planned).
- [execution-planning](../../05-components/execution-planning/overview.md) — retry intents (urgency upgrade при reconciliation gap; planned).

## Consumers

- [execution-planning](../../05-components/execution-planning/overview.md) — primary consumer, формирует routing plan.
- [observability-reporting](../../05-components/observability-reporting/overview.md) — audit + metrics.

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 7 дней (604_800_000 ms) — `infra/kafka/create_topics.sh:59` |
| Partition key | `hedge_flow_id` (ensures ordered processing per hedge session) |
| Delivery | at-least-once, idempotent consumer (key=hedge_flow_id + client_order_id) |
| Schema | `fob.execution.v1.ExecutionIntent` (Protobuf) |
| Partitions | 3 (dev), увеличить в prod |

## Message Schema

См. [`contracts/proto/fob/execution/v1/execution.proto`](../../../contracts/proto/fob/execution/v1/execution.proto), сообщение `ExecutionIntent`.

Ключевые поля:

| Поле | Тип | Описание |
| --- | --- | --- |
| `meta` | `fob.common.v1.EventMeta` | event_id, event_time, source, correlation_id |
| `intent_id` | `string` (UUID) | идемпотентный ID intent'а |
| `hedge_flow_id` | `string` (UUID) | сессия хеджа (1:1 с HedgeFlow в PostgreSQL) |
| `batch_id` | `string` | связь с BatchResult (F-04) |
| `provider_id` | `string` | провайдер, для которого хеджируется |
| `source` | `HedgeSource` enum | AUTO_BATCH / MANUAL_OVERRIDE / BACKTEST |
| `instrument` | `fob.common.v1.Instrument` | symbol, base, quote |
| `venue_symbol` | `string` | venue-specific символ (опционально; вычисляется planning'ом) |
| `allowed_venues` | `repeated string` | white-list venue IDs |
| `side` | `fob.common.v1.Side` | BUY / SELL |
| `target_qty` | `fob.common.v1.Decimal` | целевой объём |
| `target_notional` | `fob.common.v1.Decimal` | targetQty × referenceMid |
| `reference_mid` | `fob.common.v1.Decimal` | clearing-price из F-04 |
| `limit_price` | `fob.common.v1.Decimal` | необяз.: верхняя/нижняя граница |
| `max_slippage_bps` | `int32` | макс. допустимый slippage |
| `urgency` | `ExecutionUrgency` enum | LOW / MEDIUM / HIGH |
| `strategy` | `ExecutionStrategy` enum | MARKET / LIMIT / TWAP / POST_ONLY |
| `tif` | `fob.common.v1.TimeInForce` | GTC / IOC / FOK / GTD |
| `timeout_ms` | `int64` | hedgeTimeoutMs |
| `client_order_id` | `string` | для idempotency на venue side |
| `reason` | `string` | человекочитаемая причина (для manual override / retry) |

## Idempotency

- Producer не должен публиковать дубликаты по `intent_id` (одной hedge_flow_id может соответствовать несколько intent'ов в случае retry).
- Consumer (`execution-planning`) должен дедуплицировать по `intent_id` (in-memory кэш + persistence в state store).

## Used In Features

- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/) — primary.

## Used In Use Cases

- [UC-F12-01](../../02-system/use-cases/UC-F12-01-auto-hedge-after-batch/use-case.md)
- [UC-F12-02](../../02-system/use-cases/UC-F12-02-manual-operator-hedge/use-case.md)
- [UC-F12-03](../../02-system/use-cases/UC-F12-03-partial-fill-retry/use-case.md) (retry intent)
- [UC-F12-04](../../02-system/use-cases/UC-F12-04-rejection-fallback/use-case.md) (fallback retry)
- [UC-F12-05](../../02-system/use-cases/UC-F12-05-timeout-underfilled-reconciliation/use-case.md) (auto-retry on underfill)

## Used In Sequence Diagrams

- [SEQ-F12-01-auto-hedge-services](../../05-components/sequences/SEQ-F12-01-auto-hedge-services.md)
- [SEQ-F12-02-rejection-fallback-services](../../05-components/sequences/SEQ-F12-02-rejection-fallback-services.md)
- [SEQ-F12-03-error-scenarios-services](../../05-components/sequences/SEQ-F12-03-error-scenarios-services.md)

## Source Fragments

- IN-005 §1 «ExecutionIntent (Kafka execution.intents)»
- IN-005 §9 «Kafka topics»
