# Continuous Exchange (C++ microservices) — project skeleton

This repository is a **ready-to-run MVP skeleton** of a *continuous-time exchange* split into microservices **according to the attached methodology** (`Методика разработки.md`).

> **Important:** business logic is intentionally simplified (it is a simulator), but the **contracts (Proto), Kafka topics, and service choreography** are set up so you can iteratively replace the simplified parts with real solver / real venue adapters.

## 1) Services

| Service | Role | Main I/O |
|---|---|---|
| `gateway` | REST edge gateway (HTTP JSON → gRPC). Auth/rate-limit can be added here. | HTTP `POST /v1/flow-orders` → gRPC `OrderFlowService` |
| `order_flow` | Core order lifecycle + orchestration: Risk + Ledger + publish normalized order events | gRPC `OrderFlowService`; produces `orders.normalized` |
| `risk` | Pre-trade checks + kill-switch; emits alerts | gRPC `RiskService`; produces `risk.alerts` |
| `ledger` | Balances + reservations + applying fills; consumes events | gRPC `LedgerService`; consumes `batch.outputs`, `execution.venue` (and legacy `execution.reports`) |
| `matching` | Periodic *batch clearing simulator* (placeholder for a real solver) | consumes `orders.normalized`; produces `batch.outputs` |
| `market_data` | Stores last ticker and writes batch analytics to ClickHouse | consumes `marketdata.raw`, `batch.outputs`; gRPC `MarketDataService` |
| `venues` | Venue adapters (simulated + CEX/DEX connectors) | produces `marketdata.raw`, `venue.health`, `logs`, `metrics`; consumes `execution.intents`; produces `execution.venue` (and legacy `execution.reports`) |
| `observability` | Reads important topics and prints structured summaries | consumes `risk.alerts`, `batch.outputs`, `execution.venue`, `venue.health`, `logs`, `metrics` |

## 2) Kafka topics

Created automatically by `infra/kafka/create_topics.sh`:

- `marketdata.raw`
- `orders.normalized`
- `batch.outputs`
- `execution.intents`
- `execution.venue`
- `execution.reports` (legacy mirror)
- `risk.alerts`
- `venue.health`
- `logs`
- `metrics`

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
- `market_data` will persist `batch.outputs` into ClickHouse tables `batchresults` and `fills`

ClickHouse smoke check:
```bash
curl -s "http://localhost:8123/?query=SELECT%20count()%20FROM%20default.batchresults"
curl -s "http://localhost:8123/?query=SELECT%20count()%20FROM%20default.fills"
```

## 5) Build locally (without Docker)

Requires system packages: gRPC, protobuf, librdkafka, boost, libcurl.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Binaries appear in `build/bin/`.

## 6) Run Tests (CI-friendly)

Preferred (runs tests in Docker builder + isolated ClickHouse):
```bash
make test-ci
```

Equivalent direct command:
```bash
./scripts/test_ci.sh
```

Notes:
- script starts isolated `clickhouse` container in a dedicated Docker network
- builds `docker/Dockerfile.service` (`builder` target)
- runs full `ctest --test-dir /tmp/build --output-on-failure`
- writes JUnit XML to `artifacts/test-results/ctest-junit.xml`

## 7) Legacy MVP reference

The original MVP from `repo.zip` is copied into `legacy_mvp/` **for code reuse/reference**.

## 8) Claude Code settings (optional)

The repository ships with shared `.claude/settings.json` (committed) — it registers two hooks
that work out of the box for every contributor:

- `UserPromptSubmit` → `tools/auto-archive-attachments.py` (auto-archives chat attachments
  to `incoming-docs/`; see [CLAUDE.md §0a](CLAUDE.md));
- `PostToolUse` → `scripts/hooks/post_edit_hint.sh` (post-edit reminders).

For personal preferences (rsync to dev host, pre-approved bash commands, extra hooks),
copy [`.claude/settings.example.json`](.claude/settings.example.json) to
`.claude/settings.local.json` (gitignored) and edit:

```bash
cp .claude/settings.example.json .claude/settings.local.json
```

Smoke-test the auto-archive parser (CI runs this on every PR):

```bash
python3 tools/auto-archive-attachments.py --self-test
```
