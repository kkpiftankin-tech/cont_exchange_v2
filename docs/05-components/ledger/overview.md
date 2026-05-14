# Компонент: ledger

Учёт балансов пользователей. Хранит `available` / `reserved` по парам (user, currency), резервирует средства под ордера, применяет fills из батчей матчинга.

## Код

- [cpp/ledger/](../../../cpp/ledger/) — каталог
- [src/main.cpp](../../../cpp/ledger/src/main.cpp) — gRPC сервер + Kafka consumers
- [src/app/ledger_uc.cpp](../../../cpp/ledger/src/app/ledger_uc.cpp) — use-cases (in-memory state + mutex)
- [src/transport/grpc_ledger_service.cpp](../../../cpp/ledger/src/transport/grpc_ledger_service.cpp) — gRPC API
- [src/infra/kafka_consumers.cpp](../../../cpp/ledger/src/infra/kafka_consumers.cpp) — два фоновых потока (batch.outputs, execution.reports)

## gRPC API

`fob.ledger.v1.LedgerService`:

| RPC | Что делает |
|---|---|
| `GetBalances` | Вернуть available/reserved/total по пользователю |
| `ReserveFunds` | Резерв под ордер. Идемпотентен по `reservation_id` |
| `ReleaseFunds` | Снять резерв (отмена ордера) |
| `ApplyBatchResult` | Применить fills из батча (списать reserved, начислить available) |
| `ApplyExecutionReport` | Хедж-учёт от venue (в MVP — только лог) |

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `LEDGER_GRPC_LISTEN` | 0.0.0.0:50053 | Адрес gRPC сервера |
| `KAFKA_BROKERS` | redpanda:9092 | Kafka |

## Хранение

**In-memory**, mutex-защищено. Демо-баланс инициализируется в конструкторе [ledger_uc.cpp:16-26](../../../cpp/ledger/src/app/ledger_uc.cpp#L16-L26):
- `demo-user`: 10000 USDT (scale=2), 1 BTC (scale=8).

> При перезапуске состояние теряется. В проде нужно вынести в Postgres / RocksDB + event sourcing.

## Известные баги

1. **Не освобождает разницу между reserved и executed_notional** при FILLED BUY. См. [order-flow overview](../order-flow/overview.md).
2. **Scale-инфляция:** Decimal::mul удваивает scale, в результате после многих операций reserved/available могут иметь scale=10+. [ledger_uc.cpp:158-176](../../../cpp/ledger/src/app/ledger_uc.cpp#L158-L176).
3. **`ApplyExecutionReport` не делает хедж-учёт** — TODO.

## Связанные фичи

- F-04 (Batch Clearing) — потребитель `batch.outputs`
- F-06 (Positions / PnL / Margin) — основной владелец
- F-12 (Execution Hedge) — потребитель `execution.reports`

## Participates In Features

- [F-02](../../02-system/features/F-02-create-floworder/), [F-03](../../02-system/features/F-03-order-lifecycle/), [F-04](../../02-system/features/F-04-batch-clearing/), [F-06](../../02-system/features/F-06-positions-pnl-margin/), [F-07](../../02-system/features/F-07-pretrade-risk/), [F-08](../../02-system/features/F-08-posttrade-risk-and-liquidations/), [F-09](../../02-system/features/F-09-batch-combo-orders/), [F-12](../../02-system/features/F-12-execution-hedge/), [F-14](../../02-system/features/F-14-deposit-withdraw/)

## Participates In Use Cases

- [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md), [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md), [UC-F04-01](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md), [UC-F06-01](../../02-system/use-cases/UC-F06-01-show-positions/use-case.md), [UC-F07-01](../../02-system/use-cases/UC-F07-01-pretrade-risk-check/use-case.md), [UC-F08-01](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md), [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md), [UC-F14-01](../../02-system/use-cases/UC-F14-01-deposit-funds/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F02-UC-F02-01-services](../sequences/SEQ-F02-UC-F02-01-services.md), [SEQ-F03-UC-F03-01-services](../sequences/SEQ-F03-UC-F03-01-services.md), [SEQ-F04-UC-F04-01-services](../sequences/SEQ-F04-UC-F04-01-services.md), [SEQ-F06-UC-F06-01-services](../sequences/SEQ-F06-UC-F06-01-services.md), [SEQ-F07-UC-F07-01-services](../sequences/SEQ-F07-UC-F07-01-services.md), [SEQ-F08-UC-F08-01-services](../sequences/SEQ-F08-UC-F08-01-services.md), [SEQ-F12-UC-F12-01-services](../sequences/SEQ-F12-UC-F12-01-services.md), [SEQ-F14-UC-F14-01-services](../sequences/SEQ-F14-UC-F14-01-services.md)

## Owned Contracts

- `fob.ledger.v1.LedgerService` — [../../06-api/grpc/](../../06-api/grpc/)

## Produced Events

- (none directly)

## Consumed Events

- [batch.outputs](../../06-api/messaging/batch-outputs.md)
- [execution.reports](../../06-api/messaging/execution-reports.md)

## Data Access

- `accounts`, `reservations`, `positions`, `collateral_transfers` — [../../07-data/data-overview.md](../../07-data/data-overview.md)
