# Компонент: order-flow

Application-сервис, оркеструющий жизненный цикл FlowOrder: валидация → risk-check → reserve funds → публикация в Kafka.

## Код

- [cpp/order_flow/](../../../cpp/order_flow/) — каталог
- [src/main.cpp](../../../cpp/order_flow/src/main.cpp) — entrypoint
- [src/app/order_flow_uc.cpp](../../../cpp/order_flow/src/app/order_flow_uc.cpp) — use-cases (бизнес-оркестрация)
- [src/transport/grpc_order_flow_service.cpp](../../../cpp/order_flow/src/transport/grpc_order_flow_service.cpp) — gRPC сервер
- [src/infra/risk_client.cpp](../../../cpp/order_flow/src/infra/risk_client.cpp) — gRPC клиент к risk
- [src/infra/ledger_client.cpp](../../../cpp/order_flow/src/infra/ledger_client.cpp) — gRPC клиент к ledger
- [src/infra/orders_kafka_publisher.cpp](../../../cpp/order_flow/src/infra/orders_kafka_publisher.cpp) — публикатор `orders.normalized`

## gRPC API

`fob.orders.v1.OrderFlowService` (см. [order_flow_service.proto](../../../contracts/proto/fob/orders/v1/order_flow_service.proto)):

| RPC | Что делает |
|---|---|
| `CreateFlowOrder` | Валидация → Risk.CheckNewOrder → Ledger.ReserveFunds → publish `orders.normalized` |
| `CancelFlowOrder` | publish cancel event → Ledger.ReleaseFunds |
| `GetFlowOrder` | Чтение из in-memory кэша |

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `ORDER_FLOW_GRPC_LISTEN` | 0.0.0.0:50051 | Адрес gRPC сервера |
| `RISK_GRPC_ADDR` | risk:50052 | Адрес risk сервиса |
| `LEDGER_GRPC_ADDR` | ledger:50053 | Адрес ledger сервиса |
| `KAFKA_BROKERS` | redpanda:9092 | Kafka |

## Известные баги (см. F-04 анализ)

1. **Утечка резерва при BUY** — резервируется `total_qty * price_high`, но при fill списывается только `executed_notional`, разница не освобождается. [order_flow_uc.cpp:99-103](../../../cpp/order_flow/src/app/order_flow_uc.cpp#L99-L103)
2. **Двойной учёт при cancel после partial fill** — `ReleaseFunds` снимает оригинальную сумму, а не остаток. [order_flow_uc.cpp:172-186](../../../cpp/order_flow/src/app/order_flow_uc.cpp#L172-L186)
3. **In-memory кэш ордеров не потокобезопасный** — `orders_` без мьютекса. [order_flow_uc.hpp](../../../cpp/order_flow/src/app/order_flow_uc.hpp).

## Связанные фичи

- F-02 (Create FlowOrder)
- F-07 (Pre-trade Risk Control)
- F-09 (Batch and Combo Orders) — TODO

## Participates In Features

- [F-02](../../02-system/features/F-02-create-floworder/), [F-03](../../02-system/features/F-03-order-lifecycle/), [F-07](../../02-system/features/F-07-pretrade-risk/), [F-08](../../02-system/features/F-08-posttrade-risk-and-liquidations/), [F-09](../../02-system/features/F-09-batch-combo-orders/), [F-10](../../02-system/features/F-10-mm-curves/), [F-16](../../02-system/features/F-16-operator-console/)

## Participates In Use Cases

- [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md), [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md), [UC-F07-01](../../02-system/use-cases/UC-F07-01-pretrade-risk-check/use-case.md), [UC-F08-01](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md), [UC-F09-01](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md), [UC-F10-01](../../02-system/use-cases/UC-F10-01-publish-mm-curve/use-case.md), [UC-F16-01](../../02-system/use-cases/UC-F16-01-trigger-kill-switch/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F02-UC-F02-01-services](../sequences/SEQ-F02-UC-F02-01-services.md), [SEQ-F03-UC-F03-01-services](../sequences/SEQ-F03-UC-F03-01-services.md), [SEQ-F07-UC-F07-01-services](../sequences/SEQ-F07-UC-F07-01-services.md), [SEQ-F08-UC-F08-01-services](../sequences/SEQ-F08-UC-F08-01-services.md), [SEQ-F09-UC-F09-01-services](../sequences/SEQ-F09-UC-F09-01-services.md), [SEQ-F10-UC-F10-01-services](../sequences/SEQ-F10-UC-F10-01-services.md), [SEQ-F16-UC-F16-01-services](../sequences/SEQ-F16-UC-F16-01-services.md)

## Owned Contracts

- `fob.orders.v1.OrderFlowService` (CreateFlowOrder, CancelFlowOrder, GetFlowOrder, AmendFlowOrder) — [../../06-api/grpc/](../../06-api/grpc/)

## Produced Events

- [orders.normalized](../../06-api/messaging/orders-normalized.md)

## Consumed Events

- (none — receives gRPC requests only)

## Data Access

- (planned) `flow_orders`, `reservations` в PostgreSQL — [../../07-data/data-overview.md](../../07-data/data-overview.md)
