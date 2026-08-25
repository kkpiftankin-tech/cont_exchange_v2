---
id: DOC-API-VENUE-TOPICS
phase: 06-api
status: draft
owner: core-team
source:
  - IN-004 §«Kafka-топики»
  - IN-004 §«JSON-схемы (примеры)»
related:
  - docs/02-system/features/F-11-external-venues-lob-to-fob/
  - docs/02-system/features/F-12-execution-hedge/
  - infra/kafka/create_topics.sh
  - contracts/proto/fob/venue/v1/venue.proto
  - contracts/proto/fob/execution/v1/execution.proto
  - contracts/proto/fob/orders/v1/orders.proto
---

# Kafka Topics: External Venues (F-11)

Источник истины для имён, retention и партиционирования — [`infra/kafka/create_topics.sh`](../../../infra/kafka/create_topics.sh). Источник истины для payload — proto-файлы в [`contracts/proto/fob/venue/v1/`](../../../contracts/proto/fob/venue/v1/), [`contracts/proto/fob/execution/v1/`](../../../contracts/proto/fob/execution/v1/), [`contracts/proto/fob/orders/v1/`](../../../contracts/proto/fob/orders/v1/).

| Topic                    | Producer                              | Consumer(s)                                                          | Message type                              | Retention | Partition key                |
| ------------------------ | ------------------------------------- | -------------------------------------------------------------------- | ----------------------------------------- | --------- | ---------------------------- |
| `venue.snapshots`        | cpp/venues (Normalizer)               | market-data, cpp/venue_health, observability                          | `fob.venue.v1.VenueSnapshot`              | 1 h       | `{venue_id}|{instrument_symbol}` |
| `venue.liquidity.fob`    | cpp/venues (Curve Builder)            | matching, Execution Planning (F-12)                                  | `fob.venue.v1.VenueLiquidityCurve`        | 24 h      | `{venue_id}|{instrument_symbol}` |
| `venue.synthetic`        | cpp/venues (Curve Builder, optional)  | matching                                                              | `fob.orders.v1.SyntheticFlowOrder`        | 24 h      | `{venue_id}|{instrument_symbol}` |
| `venue.health`           | cpp/venues (RAW), cpp/venue_health (AGGREGATED) | cpp/venue_health, Execution Planning, risk (pending), observability | `fob.venue.v1.VenueHealth`                | 7 d       | `{venue_id}`                 |
| `execution.venue`        | cpp/venues (Execution Adapter)        | ledger, observability                                                 | `fob.execution.v1.ExecutionReport`        | 7 d       | `{hedge_flow_id}`            |
| `execution.intents`      | matching / risk (via Execution Planning) | cpp/venues (Execution Adapter)                                       | `fob.execution.v1.ExecutionIntent`        | 7 d       | `{intent_id}`                |
| `backtest.execution.venue` | cpp/venues (Backtest mode)          | backtest, observability                                               | `fob.execution.v1.ExecutionReport`        | 7 d       | `{hedge_flow_id}`            |

## Принципы

- **At-least-once.** Все consumers коммитят offset после успешной обработки.
- **Idempotency.** Через `EventMeta.event_id`/`intent_id`/`curve_id`/`snapshot_id`.
- **Partition stability.** Один логический ключ всегда уходит в одну партицию (`{venue}|{symbol}` гарантирует order per-pair).
- **Schema versioning.** `VenueLiquidityCurve` несёт `schema_version`, `min_compatible_schema_version`, `producer_version` — для backtest (F-15) и rolling upgrade.

## venue.snapshots

### Назначение

Нормализованный снимок ордербука с внешней площадки. Публикуется на каждое значимое изменение depth/best/trade.

### Producer

[venue-market-data-normalizer](../../05-components/venue-market-data-normalizer/overview.md) (runtime `cpp/venues`).

### Consumers

- [market-data](../../05-components/market-data/overview.md) — наполняет ticker cache.
- [venue-health-routing](../../05-components/venue-health-routing/overview.md) — косвенно через RAW heartbeat.
- observability — снапшоты для дашбордов.

### Settings

| Параметр       | Значение                                                |
| -------------- | ------------------------------------------------------- |
| Retention      | 1 час (Kafka); ≥ 90 дней в ClickHouse                   |
| Partition key  | `{venue_id}|{instrument_symbol}`                        |
| Delivery       | at-least-once                                           |
| Schema         | `fob.venue.v1.VenueSnapshot`                            |

### Payload (proto)

См. [`contracts/proto/fob/venue/v1/venue.proto`](../../../contracts/proto/fob/venue/v1/venue.proto).

### Payload (JSON example)

```json
{
  "snapshotId": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "venueId": "binance",
  "venueType": "cex",
  "symbol": "BTCUSDT",
  "timestamp": "2026-03-13T10:00:00.123Z",
  "midPrice": 87250.50,
  "bestBid": 87250.00,
  "bestAsk": 87251.00,
  "spread": 1.00,
  "volume24h": 42150.75,
  "bidDepth": [{"price": 87250.00, "qty": 1.5}],
  "askDepth": [{"price": 87251.00, "qty": 1.8}],
  "fees": {"maker": 0.001, "taker": 0.001},
  "tickSize": 0.01,
  "lotSize": 0.001,
  "status": "connected"
}
```

## venue.liquidity.fob

### Назначение

Непрерывное FOB-представление ликвидности площадки: $p(q)$, $S(q)$, $L(v)$, дуальная пара $S^{*}(p)$, $q^{*}(p)$.

### Producer

[venue-liquidity-curve-builder](../../05-components/venue-liquidity-curve-builder/overview.md).

### Consumers

- matching (F-04 via external_venue_filter).
- Execution Planning (F-12).

### Settings

| Параметр       | Значение                                                       |
| -------------- | -------------------------------------------------------------- |
| Retention      | 24 часа (Kafka); planned ≥ 90 дней в ClickHouse                 |
| Partition key  | `{venue_id}|{instrument_symbol}`                                |
| Delivery       | at-least-once                                                  |
| Schema         | `fob.venue.v1.VenueLiquidityCurve` (schema_version + producer_version) |

### Payload (JSON example)

```json
{
  "curveId": "f6af5b7f-8d23-4a11-90b3-7dbfae4b8b72",
  "venueId": "binance",
  "symbol": "BTCUSDT",
  "side": "buy",
  "level": "L2",
  "tauSec": 1.0,
  "qGrid": [0.0, 1.0, 2.0, 5.0, 10.0],
  "pOfQ": [87251.0, 87251.3, 87251.8, 87253.1, 87256.4],
  "sOfQ": [0.0, 87251.0, 174502.3, 436260.1, 872640.8],
  "vGrid": [0.0, 1.0, 2.0, 5.0, 10.0],
  "lOfV": [0.0, 87251.0, 174502.3, 436260.1, 872640.8],
  "epsilon1": 0.0042,
  "epsilon2": 0.0,
  "epsilon3": 0.012,
  "confidence": 0.94,
  "createdAt": "2026-03-13T10:00:00.180Z",
  "schemaVersion": 1,
  "minCompatibleSchemaVersion": 1,
  "producerVersion": "cpp-venues/0.1"
}
```

## venue.synthetic

### Назначение

Производная виртуальная `FlowOrder` от имени площадки, материализованная из `VenueLiquidityCurve`. Используется matching v1, у которого нативный consumer — `FlowOrder`.

### Producer

[venue-liquidity-curve-builder](../../05-components/venue-liquidity-curve-builder/overview.md), при `venue_config.synthetic_enabled=true`.

### Consumers

- matching (F-04, F-09).

### Settings

| Параметр       | Значение                                                |
| -------------- | ------------------------------------------------------- |
| Retention      | 24 часа                                                 |
| Partition key  | `{venue_id}|{instrument_symbol}`                        |
| Schema         | `fob.orders.v1.SyntheticFlowOrder`                      |

## venue.health

### Назначение

Health-score и circuit-breaker state площадок. **Bidirectional**: и RAW heartbeat от cpp/venues, и AGGREGATED от cpp/venue_health публикуются в один топик. Разделение через `event_type` (`RAW` / `AGGREGATED`).

### Producers

- cpp/venues (RAW) — VenueObservabilityProducer.
- cpp/venue_health (AGGREGATED) — Health Service.

### Consumers

- cpp/venue_health (фильтрует RAW).
- Execution Planning (F-12) — фильтрует AGGREGATED.
- risk (pending — T-F11-200).
- observability.

### Settings

| Параметр       | Значение                                                |
| -------------- | ------------------------------------------------------- |
| Retention      | 7 дней                                                  |
| Partition key  | `{venue_id}`                                            |
| Schema         | `fob.venue.v1.VenueHealth` (с `event_type`, `breaker_state`, `status`, `health_score`, `routing_recommendation`) |

### Payload (JSON example)

```json
{
  "venueId": "binance",
  "timestamp": "2026-03-13T10:00:00.500Z",
  "status": "connected",
  "latencyMs": 45,
  "snapshotsPerSec": 12.5,
  "errorRate": 0.0,
  "staleRate": 0.0,
  "circuitBreakerState": "CLOSED",
  "healthScore": 0.97,
  "lastSnapshotAge": 78,
  "routingRecommendation": "ALLOW",
  "eventType": "AGGREGATED"
}
```

## execution.venue

### Назначение

Отчёты об исполнении внешних child-orders. Принадлежит F-12 (Execution Hedge), но adapter publish'ит из cpp/venues (F-11).

### Producer

[venue-execution-adapter](../../05-components/venue-execution-adapter/overview.md) (runtime `cpp/venues`).

### Consumers

- ledger (apply to hedge balance, idempotent by `intent_id`).
- observability.

### Settings

| Параметр       | Значение                                                |
| -------------- | ------------------------------------------------------- |
| Retention      | 7 дней                                                  |
| Partition key  | `{hedge_flow_id}`                                       |
| Schema         | `fob.execution.v1.ExecutionReport`                      |

См. также legacy [execution-reports.md](execution-reports.md) — параллельный канал для backward compat.

## Conflict Notes

### C-1. Дублирование `marketdata.raw` и `venue.snapshots`

**Источник:** IN-004 § «Kafka-топики».

**Реальность:** [venues_loop.cpp](../../../cpp/venues/src/app/venues_loop.cpp) публикует ticker/order book одновременно в legacy `marketdata.raw` (для F-05) и в новый `venue.snapshots` (для F-11). Дублирование оправдано переходным периодом.

**Resolution plan:** Постепенная миграция consumers F-05 на `venue.snapshots` (задача T-F11-310). До тех пор оба топика поддерживаются.

### C-2. `venue.health` — два producer'а

**Источник:** IN-004 описывает `venue.health` как односторонний канал от Health Service.

**Реальность:** Поле `VenueHealthEventType` в proto явно различает `RAW` (от cpp/venues) и `AGGREGATED` (от cpp/venue_health). Оба пишут в `venue.health`; consumer Health Service фильтрует по `event_type=RAW`, остальные consumers — по `AGGREGATED`.

**Resolution plan:** Альтернатива (разделить на `venue.health.raw` + `venue.health.aggregated`) — обратимо через ADR, если возникнут проблемы с consumer-group offsets или mis-routing.

### C-3. `execution.venue` принадлежит F-12

**Источник:** IN-004 §«Kafka-топики» декларирует `execution.venue` как один из топиков F-11.

**Реальность:** Канонический owner топика — F-12 (Execution Hedge). F-11 только реализует adapter, который пишет в этот топик. Этот документ показывает топик в обоих местах для удобства навигации.

**Resolution plan:** В traceability оба feature.yaml ссылаются на `execution.venue`; основной payload-contract документируется в F-12.

## Used In Features

- [F-11. External Venues / LOB → FOB](../../02-system/features/F-11-external-venues-lob-to-fob/) — producer всех `venue.*` топиков.
- [F-04. Batch Clearing](../../02-system/features/F-04-batch-clearing/) — consumer `venue.liquidity.fob`, `venue.synthetic`.
- [F-09. Combo Orders](../../02-system/features/F-09-batch-combo-orders/) — consumer `venue.synthetic`.
- [F-12. Execution Hedge](../../02-system/features/F-12-execution-hedge/) — owner `execution.venue` + consumer `venue.health` aggregated.
- [F-15. Backtest/Replay](../../02-system/features/F-15-backtest-replay/) — использует `backtest.execution.venue` и `schema_version`/`producer_version`.
- [F-17. Monitoring](../../02-system/features/F-17-monitoring-and-alerts/) — consumer `venue.health`.

## Used In Use Cases

- [UC-F11-02](../../02-system/use-cases/UC-F11-02-publish-snapshot/use-case.md)
- [UC-F11-03](../../02-system/use-cases/UC-F11-03-build-liquidity-curve/use-case.md)
- [UC-F11-04](../../02-system/use-cases/UC-F11-04-venue-health-degradation/use-case.md)
- [UC-F11-05](../../02-system/use-cases/UC-F11-05-execute-hedge-on-venue/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F11-02-publish-snapshot-services](../../05-components/sequences/SEQ-F11-02-publish-snapshot-services.md)
- [SEQ-F11-03-build-curve-services](../../05-components/sequences/SEQ-F11-03-build-curve-services.md)
- [SEQ-F11-04-health-routing-services](../../05-components/sequences/SEQ-F11-04-health-routing-services.md)
- [SEQ-F11-05-execute-on-venue-services](../../05-components/sequences/SEQ-F11-05-execute-on-venue-services.md)
