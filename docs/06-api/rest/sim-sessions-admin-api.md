# F-20 — Admin REST API (SimSession Manager)

Источник: IN-010. Feature: [F-20](../../02-system/features/F-20-live-venue-simulator/README.md).
Через API Gateway. Все операции — операторские, требуют авторизации и audit-log (§22).

## Endpoints

| Method | Path | Назначение | SLA |
|---|---|---|---|
| `POST` | `/sim/sessions` | Создать SimSession (создаёт запись `sim_sessions`, публикует `sim.config`) | p95 < 200 мс |
| `GET` | `/sim/sessions` | Список сессий (фильтры: status, venue) | p95 < 200 мс |
| `GET` | `/sim/sessions/{id}` | Одна сессия | p95 < 200 мс |
| `PATCH` | `/sim/sessions/{id}` | routingMode / hot reload моделей / pause / complete | применение ≤ 500 мс (AC-F20-07) |

## POST /sim/sessions — тело

```json
{ "name":"BTC_SIM_2026Q2", "routingMode":"SIM_ONLY",
  "scopeVenues":["binance_sim"], "scopeInstruments":["BTCUSDT"],
  "latencyModel":{"distribution":"LOGNORMAL","p50Ms":35,"p95Ms":90,"p99Ms":150,"timeoutMs":5000},
  "impactModel":{"modelType":"SQRT","impactCoeff":0.8,"depletionMode":true},
  "feeModel":{"makerBps":1.0,"takerBps":5.0,"minFee":0.0},
  "rejectionModel":{"insufficientLiquidityEnabled":true,"randomRejectionRate":0.0,"minLiquidityThreshold":0.001},
  "staleLobThresholdMs":2000, "partialFillMode":"LEVEL_BY_LEVEL" }
```

Ответ: `201 { "simSessionId":"uuid", "status":"ACTIVE", "activatedAt":"..." }`.

## PATCH /sim/sessions/{id} — примеры

```json
{ "routingMode":"LIVE_ONLY" }          // SIM→LIVE go-live (AC-F20-07)
{ "impactModel":{"modelType":"LINEAR","impactCoeff":0.5} }  // hot reload (AC-F20-11)
{ "status":"PAUSED" }
```

Изменение публикуется в `sim.config`; `VenueSimRouter`/`VenueSimulator` применяют hot
reload без перезапуска (AC-F20-11).

## Links

- Data: [sim-sessions](../../07-data/sim-sessions.md) · Messaging: [sim-topics](../messaging/sim-topics.md)
- UC: [F20-01](../../02-system/use-cases/UC-F20-01-sim-only-execution/use-case.md)
