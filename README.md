# Continuous Exchange (C++ microservices) — project skeleton

This repository is a **ready-to-run MVP skeleton** of a *continuous-time exchange* split into microservices **according to the attached methodology** (`Методика разработки.md`).

> **Important:** business logic is intentionally simplified (it is a simulator), but the **contracts (Proto), Kafka topics, and service choreography** are set up so you can iteratively replace the simplified parts with real solver / real venue adapters.

## 1) Services

| Service | Role | Main I/O |
|---|---|---|
| `gateway` | REST edge gateway (HTTP JSON → gRPC). Auth/rate-limit can be added here. | HTTP `POST /v1/flow-orders` → gRPC `OrderFlowService` |
| `order_flow` | Core order lifecycle + orchestration: Risk + Ledger + publish normalized order events | gRPC `OrderFlowService`; produces `orders.normalized` |
| `risk` | Pre-trade checks + kill-switch; emits alerts | gRPC `RiskService`; produces `risk.alerts` |
| `ledger` | Balances + reservations + applying fills; consumes events | gRPC `LedgerService`; consumes `batch.outputs`, `execution.reports` |
| `matching` | Periodic *batch clearing simulator* (placeholder for a real solver) | consumes `orders.normalized`; produces `batch.outputs` |
| `market_data` | Stores last ticker for (venue,symbol) | consumes `marketdata.raw`; gRPC `MarketDataService` |
| `venues` | Venue adapters (here: simulator) | produces `marketdata.raw`; consumes `execution.intents`; produces `execution.reports` |
| `observability` | Reads important topics and prints structured summaries | consumes `risk.alerts`, `batch.outputs`, `execution.reports` |

## 2) Kafka topics

Created automatically by `infra/kafka/create_topics.sh`:

- `marketdata.raw`
- `orders.normalized`
- `batch.outputs`
- `execution.intents`
- `execution.reports`
- `risk.alerts`

In production you will tune:
- partition keys (`user_id`, `symbol`, `intent_id`, ...)
- retention (market data short, audit topics long)
- compaction (for snapshots, not for event logs)

## 3) Contracts (Proto)

All **contracts are in `contracts/proto/`** and compiled into `contracts_proto` C++ library.

Main mapping to “natural CCXT shapes”:
- `marketdata.raw` → `Ticker`, `OrderBookSnapshot`, `Trade`
- `execution.*` → `ExecutionIntent`, `ExecutionReport`
- `orders.normalized` → `FlowOrder` and command envelopes
- `batch.outputs` → `BatchResult` with fills and order updates
- `risk.alerts` → `RiskAlert`

## 4) Quick start (Docker Compose)

### Run everything
```bash
cd infra
docker compose -f docker-compose.dev.yml up --build
```

Gateway will be on:
- `http://localhost:8088/healthz`
- `http://localhost:8088/v1/flow-orders`

### Place an example flow order
```bash
curl -X POST "http://localhost:8088/v1/flow-orders" \
  -H "Content-Type: application/json" \
  -d '{
    "user_id":"demo-user",
    "symbol":"BTC/USDT",
    "side":"buy",
    "total_qty":0.010,
    "price_low":99.00,
    "price_high":101.00,
    "max_speed":0.002
  }'
```

Then watch logs:
- `matching` will emit `batch.outputs` every `BATCH_INTERVAL_MS`
- `ledger` will consume `batch.outputs` and update balances

## 5) Build locally (without Docker)

Requires system packages: gRPC, protobuf, librdkafka, boost.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Binaries appear in `build/bin/`.

## 6) Legacy MVP reference

The original MVP from `repo.zip` is copied into `legacy_mvp/` **for code reuse/reference**.
