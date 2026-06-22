---
id: DOC-API-TOPICS
phase: 06-api
status: draft
owner: core-team
related:
  - infra/kafka/create_topics.sh
  - docs/03-architecture/communication.md
---

# Kafka Topics

Источник истины — скрипт [../../../infra/kafka/create_topics.sh](../../../infra/kafka/create_topics.sh).
Схемы — proto-файлы в `contracts/proto/`.

## Core flow (F-02 / F-04 / F-05 / F-07 / F-08)

| Topic | Producer | Consumer(s) | Message type | Retention | Partition key |
| --- | --- | --- | --- | --- | --- |
| `marketdata.raw` | venues | market-data | `fob.marketdata.v1.MarketDataRaw` | 1 h | venue\|symbol |
| `orders.normalized` | order-flow | matching | `fob.orders.v1.OrdersNormalized` | 7 d | symbol |
| `batch.outputs` | matching | ledger, observability, backtest (parity) | `fob.matching.v1.BatchOutputs` (wrapper of BatchResult + FillEvent\[\]) | 7 d | batch_id |
| `fills` | matching | (analytics, planned consumers) | `fob.matching.v1.FillEvent` (F-09 extends with parentOrderId / executionGroupId / legId) | 7 d | order_id |
| `risk.alerts` | risk | observability | `fob.risk.v1.RiskAlert` | 30 d | user_id или alert_id |

## Execution and grouped execution (F-09 / F-12)

| Topic | Producer | Consumer(s) | Message type | Retention | Partition key |
| --- | --- | --- | --- | --- | --- |
| `execution.intents` | matching | venues | `fob.execution.v1.ExecutionIntent` | 7 d | hedge_flow_id |
| `execution.venue` | venues | ledger, observability, risk | `fob.execution.v1.ExecutionReport` | 7 d | hedge_flow_id |
| `execution.reports` | venues (legacy mirror of `execution.venue`) | ledger | same as `execution.venue` | 7 d | intent_id |
| `execution.groups` | matching | ledger, risk, order-flow, observability, market_data (CH `grouped_*` sink), backtest | `fob.matching.v1.ExecutionGroup` (F-09, ADR-033) | 7 d | parentOrderId |

## External venues (F-11)

| Topic | Producer | Consumer(s) | Message type | Retention | Partition key |
| --- | --- | --- | --- | --- | --- |
| `venue.snapshots` | venues | market-data, backtest | normalized venue LOB snapshot | 1 h | venue\|symbol |
| `venue.liquidity.fob` | venues (LOB→FOB builder) | matching, market-data, backtest | venue liquidity curve (F-11) | 1 d | venue\|symbol |
| `venue.health` | venues | matching, risk, observability | `fob.venue.v1.VenueHealth` (circuit-breaker + routing hint) | 7 d | venue |
| `venue.synthetic` | venues (synthetic LOB feed) | matching, market-data | synthetic LOB curve | 1 d | venue\|symbol |

## Simulator (F-20, ADR-015 sim/live isolation)

See [sim-topics.md](sim-topics.md) for the bundled contract.

| Topic | Producer | Consumer(s) | Message type | Retention | Partition key |
| --- | --- | --- | --- | --- | --- |
| `sim.config` | venues | venues (hot reload), observability | `fob.sim.v1.SimConfigEvent` | 7 d | sim_session_id |
| `sim.execution.venue` | venues (VenueSimRouter) | ledger (sim namespace), observability | `fob.execution.v1.ExecutionReport` (isolated from live execution.venue) | 7 d | hedge_flow_id |
| `sim.execution.annotations` | venues | observability, frontend-api | `fob.sim.v1.SimExecutionAnnotation` | 7 d | report_id |
| `sim.alerts` | venues (sim watchdogs) | observability, risk (advisory) | `fob.sim.v1.SimAlert` | 30 d | sim_session_id |

## Backtest / Replay (F-15)

See [replay-topics.md](replay-topics.md) and [backtest-topics.md](backtest-topics.md).

| Topic | Producer | Consumer(s) | Message type | Retention | Partition key |
| --- | --- | --- | --- | --- | --- |
| `replay.commands` | gateway / replay control | backtest | `CreateReplayCommand` / `CancelReplayCommand` / `RetryReplayCommand` / `PauseReplayCommand` | 7 d | replay_session_id |
| `replay.results` | backtest | observability, frontend-api | `ReplayProgressEvent` / `ReplayCompletedEvent` / `ReplayFailedEvent` / `ReplayCancelledEvent` | 7 d | replay_session_id |
| `backtest.execution.groups` | backtest (replay engine) | backtest (`grouped_replay_results` CH sink) | `fob.matching.v1.ExecutionGroup` (isolated from live `execution.groups`) | 7 d | parentOrderId |

## Infrastructure

| Topic | Producer | Consumer(s) | Message type | Retention | Partition key |
| --- | --- | --- | --- | --- | --- |
| `logs` | all services | observability | structured JSON | 7 d | — |
| `metrics` | all services | observability | metric envelope | 7 d | — |

> F-09 (IN-011, ADR-033): grouped execution envelope. Per-leg `LegFill` дублируется в `fills` (расширенные поля `parentOrderId`/`executionGroupId`/`legId`). Регистрация в `infra/kafka/create_topics.sh` — devops #14. Расширение схемы `fills` для F-09 — потенциальный breaking change (см. F-09 tasks open question).

## Принципы

- **At-least-once.** `enable.auto.commit=false`, ручной `commitSync`.
- **Idempotency.** Через `event_id` или специальный ключ.
- **Partition stability.** Один логический ключ всегда в одну партицию.

## Per-topic документы

Individual:

- [orders-normalized.md](orders-normalized.md)
- [batch-outputs.md](batch-outputs.md)
- [marketdata-raw.md](marketdata-raw.md)
- [execution-intents.md](execution-intents.md)
- [execution-venue.md](execution-venue.md)
- [execution-reports.md](execution-reports.md) (legacy mirror)
- [execution-groups.md](execution-groups.md) (F-09, ADR-033)
- [risk-alerts.md](risk-alerts.md)

Bundled (multiple related topics in one file):

- [venue-topics.md](venue-topics.md) — `venue.snapshots`, `venue.liquidity.fob`, `venue.health`, `venue.synthetic`.
- [sim-topics.md](sim-topics.md) — F-20 simulator: `sim.config`, `sim.execution.venue`, `sim.execution.annotations`, `sim.alerts`.
- [replay-topics.md](replay-topics.md) — F-15 replay commands/results: `replay.commands`, `replay.results`.
- [backtest-topics.md](backtest-topics.md) — F-15 replay isolation: `backtest.execution.groups`.

Planned:

- `agent-logs.md`.
- `fills.md` — Conflict Note C-1 below, tracked by [AUDIT-001 T-AUDIT-002](../../00-methodology/audits/AUDIT-001-feature-development-process.md).

## Conflict Notes

### C-1. Раздельные `batch.outputs` и `fills` (IN-003)

**Источник:** IN-003 §4.4.3 предполагает два отдельных Kafka топика:

- `batch.outputs` — только `BatchResult`;
- `fills` — только `FillEvent`.

**Текущее состояние (MVP):** один топик `batch.outputs` несёт оба типа сообщений (BatchResult с встроенными fills).

**Резолюция:** оставить single-topic как MVP-схему. Разделение — отдельный future task [T-F04-008](../../implementation-plan/F-04-batch-clearing.tasks.md#t-f04-008-conflict-resolution-split-batchoutputs-→-batchoutputs--fills-future) (post-MVP):

- партиционирование `fills` по `order_id` для idempotency;
- партиционирование `batch.outputs` по `batch_id`;
- миграция consumer'ов Ledger / Risk / Observability.

После выполнения T-F04-008 — обновить эту таблицу, удалить conflict note, добавить per-topic документ `fills.md`.
