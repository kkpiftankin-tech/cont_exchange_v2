# gRPC API Contracts

Этот раздел содержит документацию контрактов gRPC уровня **отдельного метода** — по одному markdown-файлу на каждый RPC. Каждый файл фиксирует: назначение метода, caller/callee, идемпотентность, связанные фичи/use cases/sequence diagrams.

**Источник истины** для proto-схем — файлы в `contracts/proto/` (см. таблицу ниже). Документация лишь ссылается и интерпретирует.

## Файлы proto (источник истины)

| Файл | Назначение |
| --- | --- |
| [common/v1/common.proto](../../../contracts/proto/fob/common/v1/common.proto) | Общие типы: `Decimal`, `Side`, `Instrument`, `OrderStatus`, `EventMeta`, `ErrorInfo` |
| [orders/v1/orders.proto](../../../contracts/proto/fob/orders/v1/orders.proto) | `FlowOrder`, `OrdersNormalized` |
| [orders/v1/order_flow_service.proto](../../../contracts/proto/fob/orders/v1/order_flow_service.proto) | gRPC `OrderFlowService` |
| [matching/v1/batch.proto](../../../contracts/proto/fob/matching/v1/batch.proto) | `BatchResult`, `Fill`, `OrderUpdate` |
| [ledger/v1/ledger.proto](../../../contracts/proto/fob/ledger/v1/ledger.proto) | gRPC `LedgerService` |
| [risk/v1/risk.proto](../../../contracts/proto/fob/risk/v1/risk.proto) | gRPC `RiskService`, `RiskAlert` |
| [marketdata/v1/marketdata_raw.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_raw.proto) | `MarketDataRaw`, `Ticker`, `Trade`, `OrderBookL2Update` |
| [marketdata/v1/marketdata_service.proto](../../../contracts/proto/fob/marketdata/v1/marketdata_service.proto) | gRPC `MarketDataService` |
| [execution/v1/execution.proto](../../../contracts/proto/fob/execution/v1/execution.proto) | `ExecutionIntent`, `ExecutionReport` |
| [observability/v1/observability.proto](../../../contracts/proto/fob/observability/v1/observability.proto) | Метрики, алерты |
| [agent/v1/agent.proto](../../../contracts/proto/fob/agent/v1/agent.proto) | Контракты для агентов/backtest |

## Индекс per-method контрактов

Легенда статусов:

- `draft` — метод определён в `.proto`, документация описывает текущую форму;
- `TODO / planned` — метод упомянут в sequence diagrams, но не реализован в `.proto`. Файл фиксирует предложенную схему до её добавления в контракты.

### `fob.orders.v1.OrderFlowService`

| Метод | Статус | Документ |
| --- | --- | --- |
| `CreateFlowOrder` | draft | [order-flow-create-flow-order.md](order-flow-create-flow-order.md) |
| `CancelFlowOrder` | draft | [order-flow-cancel-flow-order.md](order-flow-cancel-flow-order.md) |
| `GetFlowOrder` | draft | [order-flow-get-flow-order.md](order-flow-get-flow-order.md) |
| `AmendFlowOrder` | TODO / planned | [order-flow-amend-flow-order.md](order-flow-amend-flow-order.md) |
| `CreateComboOrder` | TODO / planned | [order-flow-create-combo-order.md](order-flow-create-combo-order.md) |
| `UpsertCurve` | TODO / planned | [order-flow-upsert-curve.md](order-flow-upsert-curve.md) |

### `fob.risk.v1.RiskService`

| Метод | Статус | Документ |
| --- | --- | --- |
| `CheckNewOrder` (alias `PreTradeCheck`) | draft | [risk-check-new-order.md](risk-check-new-order.md) |
| `SetKillSwitch` (alias `Activate`/`DeactivateKillSwitch`) | draft | [risk-set-kill-switch.md](risk-set-kill-switch.md) |
| `OnBatchResult` | draft | [risk-on-batch-result.md](risk-on-batch-result.md) |
| `GetRiskSnapshot` | TODO / planned | [risk-get-risk-snapshot.md](risk-get-risk-snapshot.md) |

### `fob.ledger.v1.LedgerService`

| Метод | Статус | Документ |
| --- | --- | --- |
| `GetBalances` | draft | [ledger-get-balances.md](ledger-get-balances.md) |
| `ReserveFunds` (alias `Reserve`) | draft | [ledger-reserve-funds.md](ledger-reserve-funds.md) |
| `ReleaseFunds` (alias `ReleaseReservation`) | draft | [ledger-release-funds.md](ledger-release-funds.md) |
| `ApplyBatchResult` | draft | [ledger-apply-batch-result.md](ledger-apply-batch-result.md) |
| `ApplyExecutionReport` | draft | [ledger-apply-execution-report.md](ledger-apply-execution-report.md) |
| `AdjustReservation` | TODO / planned | [ledger-adjust-reservation.md](ledger-adjust-reservation.md) |
| `GetPositions` | TODO / planned | [ledger-get-positions.md](ledger-get-positions.md) |
| `GetFreeBalance` | TODO / planned | [ledger-get-free-balance.md](ledger-get-free-balance.md) |
| `ApplyDeposit` | TODO / planned | [ledger-apply-deposit.md](ledger-apply-deposit.md) |

### `fob.marketdata.v1.MarketDataService`

| Метод | Статус | Документ |
| --- | --- | --- |
| `GetLastTicker` | draft | [marketdata-get-last-ticker.md](marketdata-get-last-ticker.md) |
| `StreamTickers` | TODO / planned | [marketdata-stream-tickers.md](marketdata-stream-tickers.md) |
| `GetReferencePrices` | TODO / planned | [marketdata-get-reference-prices.md](marketdata-get-reference-prices.md) |

### `fob.auth.v1.AuthService` (planned package)

| Метод | Статус | Документ |
| --- | --- | --- |
| `Login` | TODO / planned | [auth-login.md](auth-login.md) |
| `ValidateToken` | TODO / planned | [auth-validate-token.md](auth-validate-token.md) |

### `fob.custody.v1.CustodyService` (planned package)

| Метод | Статус | Документ |
| --- | --- | --- |
| `CreateDepositAddress` | TODO / planned | [custody-create-deposit-address.md](custody-create-deposit-address.md) |

### `fob.observability.v1.ObservabilityService` (planned package)

| Метод | Статус | Документ |
| --- | --- | --- |
| `GetPostTradeReport` | TODO / planned | [observability-get-post-trade-report.md](observability-get-post-trade-report.md) |

## Сборка proto

[contracts/CMakeLists.txt](../../../contracts/CMakeLists.txt) собирает таргет `contracts_proto`, к которому линкуются все C++ сервисы.

## Правила

См. [../api-overview.md](../api-overview.md).

- Все proto-файлы должны быть учтены в [specs/contracts/proto-map.yaml](../../../specs/contracts/proto-map.yaml).
- Breaking changes — только через новую major-версию (`v2/`).
- Любой метод, упомянутый в service sequence diagram, должен иметь либо `.proto`-определение, либо TODO contract в этом разделе.
- Sequence diagrams ссылаются на конкретный per-method файл, а не на папку.
- Aliases (`PreTradeCheck` ↔ `CheckNewOrder`, `Reserve` ↔ `ReserveFunds` и т.п.) допустимы в sequence diagrams для краткости, но всегда явно резолвятся к каноническому методу в этом индексе.
