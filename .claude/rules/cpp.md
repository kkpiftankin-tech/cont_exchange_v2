---
paths: ["cpp/**", "docker/**", "CMakeLists.txt", "Makefile"]
---

# C++-сервисы: ответственность, слои, стиль (перенесено из CLAUDE.md §6, §10, §12)

## Текущие сервисы и ответственность

| Сервис / каталог | Ответственность | Основные входы | Основные выходы |
|---|---|---|---|
| `cpp/gateway` | REST edge gateway, HTTP JSON → gRPC `OrderFlowService` | HTTP `POST /v1/flow-orders`, health | gRPC calls to `order_flow` |
| `cpp/order_flow` | Жизненный цикл FlowOrder, risk check, reserve funds, publish normalized orders | gRPC `CreateFlowOrder`, `CancelFlowOrder`, `GetFlowOrder` | Kafka `orders.normalized`, gRPC calls to risk/ledger |
| `cpp/risk` | Pre-trade checks, kill-switch, risk alerts | gRPC `RiskService` | Kafka `risk.alerts` |
| `cpp/ledger` | Балансы, резервы, применение fills/execution reports; hedge PnL — [`cpp/ledger/CLAUDE.md`](../../cpp/ledger/CLAUDE.md) | gRPC `LedgerService`, Kafka `batch.outputs`, `execution.reports` | balance state, position state |
| `cpp/matching` | Периодический batch-clearing simulator/solver — [`cpp/matching/CLAUDE.md`](../../cpp/matching/CLAUDE.md) | Kafka `orders.normalized` | Kafka `batch.outputs` |
| `cpp/market_data` | Last ticker cache, market data read API, hedge PnL aggregate для ClickHouse | Kafka `marketdata.raw` | gRPC `MarketDataService` |
| `cpp/venues` | External venue adapter (симулятор + real REST) — [`cpp/venues/CLAUDE.md`](../../cpp/venues/CLAUDE.md) | timer / execution intents | Kafka `marketdata.raw`, `execution.reports` |
| `cpp/venue_health` | Venue health scoring / routing recommendations (F-11) | `venue.snapshots`, `venue.liquidity.fob` | Kafka `venue.health` |
| `cpp/backtest` | Replay/backtest engine, shadow ledger (F-15) | historical batch/execution data | replay reports |
| `cpp/observability` | Читает важные топики и пишет структурированные summaries | Kafka `risk.alerts`, `batch.outputs`, `execution.reports` | structured logs |
| `cpp/common` | Общие утилиты: env, log, uuid, time, Decimal/proto helpers, Kafka wrappers | internal use | shared library |

Не смешивай ответственность между сервисами:

- `order_flow` не решает matching;
- `matching` не резервирует средства;
- `ledger` не принимает risk decisions;
- `risk` не мутирует ledger напрямую, кроме явно описанных
  liquidation/rebalance flows;
- `market_data` не торгует;
- `venues` не принимает бизнес-решения о хеджировании, а исполняет
  `ExecutionIntent`;
- `observability` не влияет на бизнес-состояние, кроме будущих operator
  workflows через отдельный control API.

## Архитектурный стиль

Event-driven microservices skeleton с контракт-first gRPC/protobuf и
Kafka/Redpanda event bus. Внутри сервиса придерживайся слоёв:

```text
transport/     HTTP/gRPC handlers, Kafka consumers/producers adapters
app/           use cases, orchestration, application services
domain/        entities, value objects, invariants, pure business rules
infra/         DB repositories, external clients, Kafka wrappers, gRPC clients
```

Текущий C++ MVP местами содержит упрощённую структуру. При развитии сервиса
постепенно приводить его к этой схеме, не ломая поведение.

Правила зависимостей:

- `domain` не знает о gRPC, Kafka, HTTP, DB, Docker, env.
- `app` знает о domain и интерфейсах портов.
- `transport` маппит внешние DTO в application commands.
- `infra` реализует порты: DB, Kafka, gRPC clients, external venue APIs.
- Общие утилиты живут в `cpp/common` только если они действительно
  cross-service.
- Не добавляй доменную бизнес-логику в `main.cpp`.

## Версия и сборка

- C++ standard: C++20.
- Build: CMake `>= 3.24`.
- Основная сборка: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.
- Docker dev runtime: `cd infra && docker compose -f docker-compose.dev.yml up --build`.
- На dev-хосте локального toolchain нет — тесты одного таргета собираются
  через builder-образ (`docker build --target builder --build-arg TARGET=<test>`),
  см. навык `rebuild-service` и `docker/Dockerfile.service`.

## Стиль

- Чистые доменные функции — максимально deterministic и unit-testable.
- Не использовать глобальное mutable-состояние, кроме явно контролируемых
  service runtime caches.
- Любой background thread должен иметь понятный lifecycle.
- Для production-изменений добавлять graceful shutdown, если сервис содержит
  consumer loop.
- Ошибки должны быть typed/stable: `Error.code`, `Error.message`,
  `Error.details`.
- Логи — structured JSON через общий logging layer.
- Для внешних IDs и idempotency использовать явные поля, не перегружать
  `order_id`.

## Конфигурация

- Не хардкодить адреса сервисов, брокеров, тайминги, лимиты, secrets.
- Использовать env vars через `cex::common::Env`.
- Dev defaults допустимы, если явно безопасны и подходят для docker-compose.
- Prod-секреты никогда не хранить в git (правило в CLAUDE.md §3;
  `permissions.deny` в `.claude/settings.json` блокирует чтение `infra/.env`
  и `secrets/**` инструментом Read).

## Generated code

Не редактировать protobuf-generated `.pb.cc`, `.pb.h`, `.grpc.pb.cc`,
`.grpc.pb.h` — см. `.claude/rules/contracts.md`.
