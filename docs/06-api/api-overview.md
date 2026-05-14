# Proto-контракты

`contracts/proto/fob/**` — **источник истины** для всех межсервисных контрактов (gRPC + Kafka payloadы). Документация лишь ссылается на эти файлы, дублирование запрещено.

## Файлы

| Файл | Назначение |
|---|---|
| [common/v1/common.proto](../../contracts/proto/fob/common/v1/common.proto) | Общие типы: `Decimal`, `Side`, `Instrument`, `OrderStatus`, `EventMeta`, `ErrorInfo` |
| [orders/v1/orders.proto](../../contracts/proto/fob/orders/v1/orders.proto) | `FlowOrder`, `OrdersNormalized` (create/cancel/amend) |
| [orders/v1/order_flow_service.proto](../../contracts/proto/fob/orders/v1/order_flow_service.proto) | gRPC `OrderFlowService`: CreateFlowOrder, CancelFlowOrder, GetFlowOrder |
| [matching/v1/batch.proto](../../contracts/proto/fob/matching/v1/batch.proto) | `BatchResult`, `Fill`, `OrderUpdate`, `BatchDiagnostics` |
| [ledger/v1/ledger.proto](../../contracts/proto/fob/ledger/v1/ledger.proto) | gRPC `LedgerService`: GetBalances, ReserveFunds, ReleaseFunds, ApplyBatchResult, ApplyExecutionReport |
| [risk/v1/risk.proto](../../contracts/proto/fob/risk/v1/risk.proto) | gRPC `RiskService`: CheckNewOrder, SetKillSwitch, OnBatchResult; `RiskAlert` |
| [marketdata/v1/marketdata_raw.proto](../../contracts/proto/fob/marketdata/v1/marketdata_raw.proto) | `MarketDataRaw`, `Ticker`, `Trade`, `OrderBookL2Update` |
| [marketdata/v1/marketdata_service.proto](../../contracts/proto/fob/marketdata/v1/marketdata_service.proto) | gRPC `MarketDataService`: GetLastTicker |
| [execution/v1/execution.proto](../../contracts/proto/fob/execution/v1/execution.proto) | `ExecutionIntent`, `ExecutionReport` |
| [observability/v1/observability.proto](../../contracts/proto/fob/observability/v1/observability.proto) | Метрики, алерты, health-статусы |
| [agent/v1/agent.proto](../../contracts/proto/fob/agent/v1/agent.proto) | Контракты для агентов / backtest / replay |

## Сборка

Сборка proto-файлов настроена в [contracts/CMakeLists.txt](../../contracts/CMakeLists.txt). Каждый C++ сервис линкуется к таргету `contracts_proto`.

## Правила изменения

1. Любое изменение `*.proto` сопровождается обновлением `specs/contracts/proto-map.yaml`.
2. **Breaking changes запрещены** в пределах одной major-версии (`v1/`). Новые поля только опциональные; удаления — через `reserved`.
3. CI gate [proto-contract-audit.yml](../../.github/workflows/proto-contract-audit.yml) проверяет, что каждый файл из `contracts/proto/**` упомянут в `proto-map.yaml`.
