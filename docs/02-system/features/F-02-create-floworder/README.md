# F-02 — Create FlowOrder

## Описание

Создание flow-ордера (с диапазоном цены, скоростью и максимальным объёмом). Точка входа для всех клиентских торговых операций.

## Поток

```
Client → HTTP gateway (POST /v1/flow-orders)
       → gRPC order_flow.CreateFlowOrder
           → risk.CheckNewOrder (gRPC)
           → ledger.ReserveFunds (gRPC)
           → Kafka publish (orders.normalized)
       → response: {accepted, order_id}
```

## Ключевые файлы

- [cpp/gateway/src/transport/http_gateway.cpp](../../../../cpp/gateway/src/transport/http_gateway.cpp) — HTTP-маршрут
- [cpp/order_flow/src/app/order_flow_uc.cpp](../../../../cpp/order_flow/src/app/order_flow_uc.cpp) — оркестрация

## Связанные фичи

- F-04 (Batch Clearing) — consumer `orders.normalized`; источник `FillEvent`/`batch.outputs`
- F-07 (Pre-trade Risk) — синхронный гейт перед резервом
- F-12 (Execution Hedge) — Exchange-provider хеджирует полученные `FillEvent` на внешних venues

## v2 (IN-004): роль Exchange-provider

Обновление v2 добавляет **Exchange-provider** как отдельную роль/провайдера
(`providerType=provider`): он создаёт заявки через тот же API, а после матчинга **читает
`FillEvent`** (`batch.outputs`, F-04) и инициирует хедж позиции через Venue Execution
Adapter (F-12). Новое требование — **F2-17** ([functional-requirements §FR-ORDERS](../../functional-requirements.md)).
Цепочка: F-02 (создать) → F-04 (fills) → Exchange-provider → F-12 (хедж). Изменения
аддитивны к v1 (реестр правок — в [IN-004.meta](../../../../incoming-docs/IN-004.meta.md)).

## Acceptance / Issues

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна позволять клиенту создать потоковую заявку на покупку/продажу инструмента с указанием диапазона цен `[p_low, p_high]`, скорости `q_rate` и максимального объёма `q_max`, окна исполнения.

- Перед активацией показан preview прогнозируемого VWAP и IS (среднее, std, квантили).
- Заявка проходит pre-trade risk check (F-07).
- При accept резерв в `accounts.reserved_balance` создан, событие `orders.normalized` опубликовано.
- Клиент видит статус `active` и начало fills в WebSocket-стриме.

Источник: IN-001 §6 FR-ORDER-001/002, §5.1 сценарий ликвидного трейдера.

## Source Fragments

- IN-001-FR-015
- IN-001-FR-027
- IN-001-FR-028
