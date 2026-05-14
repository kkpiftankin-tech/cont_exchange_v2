# Data Model: PostgreSQL OLTP + ClickHouse OLAP

Полная модель данных делится на **три слоя**:

1. **Proto contracts** ([contracts/proto/fob/](../../contracts/proto/)) — wire-формат для gRPC и Kafka. Source of truth для сообщений между сервисами.
2. **PostgreSQL (OLTP)** — оперативные данные с ACID: пользователи, активные ордера, балансы, лимиты, конфиг solver'а. **Планируется**, в текущем MVP — in-memory.
3. **ClickHouse (OLAP)** — историческая аналитика: fills, batch_results, marketdata, risk_events, execution_reports, agent_logs. **Планируется**, в текущем MVP — отсутствует.

Полные схемы таблиц — в [specs/domain/storage-schema.yaml](../../specs/domain/storage-schema.yaml).

## Принцип разделения

- **PostgreSQL — источник истины** для оперативных решений: что разрешено, кто владелец, какой текущий баланс. ACID-гарантии, малая латентность.
- **ClickHouse — источник истины** для исторических метрик: что фактически произошло, с какими параметрами solver, какие были fills. Только append. Используется для отчётности, backtest, мониторинга SLA.
- **Kafka** — между ними как event log: каждое доменное событие (FlowOrder создан, batch выполнен, fill, risk alert) попадает в Kafka и оттуда консьюмится в ClickHouse через Kafka table engine.

## Таблицы PostgreSQL (OLTP)

| Таблица | Назначение | Главные writer'ы | Главные reader'ы |
|---|---|---|---|
| `users` | Учётные записи пользователей | Auth & Identity | Auth, API Gateway, Risk |
| `sessions` | Активные сессии и токены | Auth & Identity | API Gateway |
| `accounts` | Балансы по парам (user, asset): free/reserved/venue-allocated | Ledger | Ledger, Risk, Gateway |
| `flow_orders` | Активные потоковые ордера | Order Flow, Matching | Matching, Risk, Ledger, Gateway |
| `positions` | Текущие позиции и PnL | Ledger | Ledger, Risk, Gateway |
| `risk_limits` | Лимиты по user/role/symbol/global + kill_switch | Risk, Operator UI | Risk, Matching |
| `risk_snapshots` | Снимки margin-состояния | Risk | Ledger, Observability |
| `collateral_transfers` | Запросы на перевод средств между venues | Ledger | Ledger, Custody Adapter, Risk |
| `solver_config` | Параметры solver'а (versioned) | Operator UI | Matching, Backtest |

## Таблицы ClickHouse (OLAP)

| Таблица | Назначение | Источник | Главные consumer'ы |
|---|---|---|---|
| `fills` | Все исполнения (FillEvent) | Kafka `batch.outputs` / `fills` | Observability, Backtest, Market Data |
| `batch_results` | Диагностика каждого batch | Kafka `batch.outputs` | Observability (SLA), Backtest |
| `marketdata` | Исторические тикеры/order book | Kafka `marketdata.raw` | Market Data, Backtest, Observability |
| `risk_events` | Лог решений risk-движка | Kafka `risk.alerts` | Observability, Backtest, Risk (тюнинг) |
| `execution_reports` | Отчёты по hedge-сделкам | Kafka `execution.reports` | Observability, Backtest, Ledger (тригер) |
| `agent_logs` | State-action-reward для агентов | Сервисные процессы | Backtest, Research, Observability |

## Связь Proto ↔ Storage

| Proto-сущность | OLTP-таблица | OLAP-таблица |
|---|---|---|
| `FlowOrder` | `flow_orders` | (нет, состояние) |
| `OrdersNormalized` (Kafka payload) | — | (можно отразить в `agent_logs`) |
| `BatchResult` | — | `batch_results` |
| `Fill` (внутри BatchResult) | — | `fills` |
| `Balance`/`Reservation` (домен ledger) | `accounts` | — |
| `RiskAlert` | — | `risk_events` |
| `Ticker`/`MarketDataRaw` | — | `marketdata` |
| `ExecutionIntent`/`ExecutionReport` | — | `execution_reports` |

## Где это сейчас в коде

| Концептуальная сущность | Текущая реализация |
|---|---|
| `users`, `sessions` | Не реализовано (gateway без auth) |
| `accounts` | [cpp/ledger/src/app/ledger_uc.cpp](../../cpp/ledger/src/app/ledger_uc.cpp) — `std::unordered_map<user, std::unordered_map<currency, Balance>>` (in-memory) |
| `flow_orders` | [cpp/order_flow/src/app/order_flow_uc.hpp](../../cpp/order_flow/src/app/order_flow_uc.hpp) — `std::unordered_map<order_id, FlowOrder>` (in-memory) и [cpp/matching/src/app/matching_loop.hpp](../../cpp/matching/src/app/matching_loop.hpp) — отдельная копия в матчинге |
| `positions` | Не реализовано (только балансы) |
| `risk_limits` | [cpp/risk/src/app/risk_uc.hpp](../../cpp/risk/src/app/risk_uc.hpp) — только `kill_switch` (in-memory) |
| `risk_snapshots` | Не реализовано |
| `collateral_transfers` | Не реализовано |
| `solver_config` | [cpp/matching/src/main.cpp](../../cpp/matching/src/main.cpp) — только `BATCH_INTERVAL_MS` из env |
| `fills` | События публикуются в Kafka `batch.outputs`, но ClickHouse-ingestion нет |
| `batch_results` | Аналогично |
| `marketdata` | Аналогично через `marketdata.raw` |
| `risk_events` | Только JSON-логи в stdout |
| `execution_reports` | Публикуются в Kafka, но не сохраняются |
| `agent_logs` | Не реализовано |

## Roadmap

1. **MVP+1** — поднять PostgreSQL для `accounts` и `flow_orders`, мигрировать ledger и order_flow с in-memory.
2. **MVP+2** — поднять ClickHouse с Kafka table engine, начать наливать `fills` и `batch_results`.
3. **MVP+3** — реализовать `risk_limits`, `solver_config`, `collateral_transfers`.
4. **MVP+4** — `positions`, `risk_snapshots`, `agent_logs`, `execution_reports`.
