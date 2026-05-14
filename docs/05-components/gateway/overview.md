# Компонент: gateway

Edge HTTP-сервис: принимает REST/JSON от UI/клиентов, конвертирует в proto и шлёт через gRPC в `order_flow`.

## Код

- [cpp/gateway/](../../../cpp/gateway/) — каталог
- [src/main.cpp](../../../cpp/gateway/src/main.cpp) — entrypoint
- [src/transport/http_gateway.cpp](../../../cpp/gateway/src/transport/http_gateway.cpp) — HTTP-маршруты (Crow)
- [src/infra/order_flow_client.cpp](../../../cpp/gateway/src/infra/order_flow_client.cpp) — gRPC-клиент к order_flow

## Зависимости

- Crow (header-only HTTP) — из [third_party/crow](../../../third_party/crow)
- Boost.Asio, Boost.Thread
- `cex_common`, `contracts_proto`

## HTTP API (MVP)

| Метод | Путь | Что делает |
|---|---|---|
| GET | `/healthz` | Liveness probe |
| GET | `/readyz` | Readiness probe |
| POST | `/v1/flow-orders` | Создание FlowOrder. Транслирует JSON → `fob.orders.v1.FlowOrder`, шлёт `CreateFlowOrder` в order_flow |

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `GATEWAY_HTTP_PORT` | 8080 | Порт HTTP-сервера |
| `ORDER_FLOW_GRPC_ADDR` | order_flow:50051 | Адрес order_flow для gRPC |

## Граничные случаи

- Decimal на границе HTTP: входы конвертятся через `dec_from_double` с фиксированными scale (qty=8, price=2). При больших значениях возможна потеря точности — это компромисс на edge.
- Авторизации и rate-limiting **не реализованы**. На проде сюда должны встать middleware.

## Связанные фичи

- F-02 (Create FlowOrder)
- F-05 (Live Market Data) — пока endpoint'ы не добавлены, но логически шлюз
- F-06 (Positions/PnL/Margin) — TODO endpoint'ы

## Participates In Features

- [F-01](../../02-system/features/F-01-auth-and-identity/), [F-02](../../02-system/features/F-02-create-floworder/), [F-03](../../02-system/features/F-03-order-lifecycle/), [F-05](../../02-system/features/F-05-live-market-data/), [F-06](../../02-system/features/F-06-positions-pnl-margin/), [F-09](../../02-system/features/F-09-batch-combo-orders/), [F-10](../../02-system/features/F-10-mm-curves/), [F-13](../../02-system/features/F-13-posttrade-report/), [F-14](../../02-system/features/F-14-deposit-withdraw/), [F-16](../../02-system/features/F-16-operator-console/)

## Participates In Use Cases

- [UC-F01-01](../../02-system/use-cases/UC-F01-01-authenticate-user/use-case.md), [UC-F02-01](../../02-system/use-cases/UC-F02-01-create-flow-order/use-case.md), [UC-F03-01](../../02-system/use-cases/UC-F03-01-amend-cancel-order/use-case.md), [UC-F05-01](../../02-system/use-cases/UC-F05-01-stream-market-data/use-case.md), [UC-F06-01](../../02-system/use-cases/UC-F06-01-show-positions/use-case.md), [UC-F09-01](../../02-system/use-cases/UC-F09-01-create-combo-order/use-case.md), [UC-F10-01](../../02-system/use-cases/UC-F10-01-publish-mm-curve/use-case.md), [UC-F13-01](../../02-system/use-cases/UC-F13-01-generate-posttrade-report/use-case.md), [UC-F14-01](../../02-system/use-cases/UC-F14-01-deposit-funds/use-case.md), [UC-F16-01](../../02-system/use-cases/UC-F16-01-trigger-kill-switch/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F01-UC-F01-01-services](../sequences/SEQ-F01-UC-F01-01-services.md), [SEQ-F02-UC-F02-01-services](../sequences/SEQ-F02-UC-F02-01-services.md), [SEQ-F03-UC-F03-01-services](../sequences/SEQ-F03-UC-F03-01-services.md), [SEQ-F05-UC-F05-01-services](../sequences/SEQ-F05-UC-F05-01-services.md), [SEQ-F06-UC-F06-01-services](../sequences/SEQ-F06-UC-F06-01-services.md), [SEQ-F09-UC-F09-01-services](../sequences/SEQ-F09-UC-F09-01-services.md), [SEQ-F10-UC-F10-01-services](../sequences/SEQ-F10-UC-F10-01-services.md), [SEQ-F13-UC-F13-01-services](../sequences/SEQ-F13-UC-F13-01-services.md), [SEQ-F14-UC-F14-01-services](../sequences/SEQ-F14-UC-F14-01-services.md), [SEQ-F16-UC-F16-01-services](../sequences/SEQ-F16-UC-F16-01-services.md)

## Owned Contracts

- REST endpoints — см. [../../06-api/rest/](../../06-api/rest/)
- gRPC clients to internal services — см. [../../06-api/grpc/](../../06-api/grpc/)

## Produced Events

- (none directly)

## Consumed Events

- (none directly; routes to downstream services)

## Data Access

- (proxy only; no direct DB access)
