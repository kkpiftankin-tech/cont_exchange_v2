# F-20 — Kafka topics & messages (Live Venue Simulator)

Источник: IN-010. Feature: [F-20](../../02-system/features/F-20-live-venue-simulator/README.md).
Деньги/цены — `Decimal` в целевой proto-модели (§9); в JSON-примерах — числа для наглядности.

## Топики

| Topic | Producer | Consumer | Key | Retention | Назначение |
|---|---|---|---|---|---|
| `venue.snapshots` (F-11, **reuse**) | Venue MD Normalizer | VenueSimulator | `venueId+symbol` | short | Живой LOB — источник ликвидности симулятора |
| `execution.venue` (**reuse**) | VenueSimulator / EVC | Ledger, ClickHouse, Divergence | `clientOrderId` | long (audit) | (Sim)ExecutionReport; `simMode` различает SIM/LIVE |
| `sim.execution.venue` (**new**) | VenueSimulator | (опц.) изолированные консюмеры sim | `clientOrderId` | 90d | Дубль только симулированных отчётов |
| `sim.config` (**new**) | SimSession Manager | VenueSimRouter, VenueSimulator | `simSessionId` | compacted | Hot reload конфигурации SimSession |
| `sim.alerts` (**new**) | VenueSimulator | Observability, operator UI | `venueId` | 30d | `SIM_STALE_LOB` / `SIM_TIMEOUT` / `SIM_LOB_SOURCE_DOWN` |

> `venue.snapshots` и `execution.venue` — существующие/боевые топики F-11/F-12; F-20 их
> переиспользует. Новые топики (`sim.*`) добавляются в `infra/kafka/create_topics.sh`.

## Сообщения

### ChildOrderRequest (VEA → VenueSimRouter; in-process/gRPC)

```json
{ "childOrderId":"uuid", "hedgeFlowId":"uuid", "venueId":"binance", "symbol":"BTCUSDT",
  "side":"SELL", "orderType":"LIMIT", "qty":0.5, "price":68000.0, "timeInForce":"GTC",
  "clientOrderId":"hf-abc123-001", "simSessionId":"uuid-or-null" }
```

### SimExecutionReport (`execution.venue`, `sim.execution.venue`)

Стандартные поля `ExecutionReport` (F-12) **+** sim-поля:

```json
{ "executionId":"uuid", "venueId":"binance_sim", "symbol":"BTCUSDT", "side":"SELL",
  "filledQty":0.5, "avgPrice":67982.15, "fee":6.798, "status":"FILLED",
  "timestamp":"2026-04-30T10:00:00.125Z", "clientOrderId":"hf-abc123-001", "hedgeFlowId":"uuid",
  "simMode":true, "simSessionId":"uuid", "lobSnapshotId":"uuid", "lobAge":42,
  "impactBps":2.8, "slippageBps":3.1, "latencySampleMs":68 }
```

`status ∈ {FILLED, PARTIALLY_FILLED, CANCELLED, REJECTED}`;
`rejectReason ∈ {SIM_STALE_LOB, SIM_NO_LIQUIDITY, SIM_RANDOM_REJECT, SIM_TIMEOUT}`.

### SimConfigEvent (`sim.config`)

```json
{ "simSessionId":"uuid", "routingMode":"SIM_ONLY|LIVE_ONLY|SHADOW",
  "scopeVenues":["binance_sim"], "scopeInstruments":["BTCUSDT"],
  "latencyModel":{...}, "impactModel":{...}, "feeModel":{...}, "rejectionModel":{...},
  "staleLobThresholdMs":2000, "partialFillMode":"LEVEL_BY_LEVEL", "status":"ACTIVE" }
```

### SimAlert (`sim.alerts`)

```json
{ "alertType":"SIM_STALE_LOB|SIM_LOB_SOURCE_DOWN|SIM_TIMEOUT_RATE_HIGH",
  "venueId":"binance_sim", "symbol":"BTCUSDT", "simSessionId":"uuid",
  "detail":"lobAge=5300ms > threshold=2000ms", "timestamp":"..." }
```

## Proto (CN-F20-02, TODO)

Целевая реализация: расширить `contracts/proto/fob/execution/v1/execution.proto`
(`ExecutionReport` + optional sim-поля, backward-compatible) **или** отдельный
`fob/marketdata/v1/sim_execution.proto`. Решение — ADR при реализации контракта.
Деньги (`avgPrice`, `fee`) — `fob.common.v1.Decimal`.

## Links

- Feature [F-20](../../02-system/features/F-20-live-venue-simulator/README.md) · UC [F20-01](../../02-system/use-cases/UC-F20-01-sim-only-execution/use-case.md)/[F20-02](../../02-system/use-cases/UC-F20-02-shadow-compare/use-case.md)
- Data: [sim-execution-reports](../../07-data/sim-execution-reports.md)
