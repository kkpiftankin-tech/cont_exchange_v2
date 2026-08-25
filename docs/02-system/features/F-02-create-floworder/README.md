# F-02 — Create FlowOrder

## 🧭 Navigation Map (IN-013 drill-down)

Эта секция — **карта документации сверху вниз** для фичи.
Каждый уровень имеет свой ответ на «что/как», и каждая ссылка
ведёт на следующий уровень детализации.

```text
   ┌─ Уровень ──────────────┬─ Артефакт ─────────────────────────────────┐
☁️ L0 │ Что система делает    │ Эта страница + L0 system sequence(s) ниже  │
🌊 L1 │ Какие функции у фичи?  │ Use Cases (таблица ниже)                   │
   │ Какие сервисы участвуют?│ L1 service sequences (per-UC)              │
🐟 L2 │ Из каких классов       │ Component overviews + L2 sequences         │
   │ состоит сервис?        │                                            │
💻 src │ Код                    │ cpp/<component>/src/...                    │
   └────────────────────────┴────────────────────────────────────────────┘
```

## 📋 Use Cases (L1 🌊)

| UC | Имя | L0 sequence ☁️ | L1 sequence 🌊 |
| --- | --- | --- | --- |
| [UC-F02-01](../../use-cases/UC-F02-01-create-flow-order/use-case.md) | Create Flow Order | [SEQ-UC-F02-01-system](../../use-cases/UC-F02-01-create-flow-order/sequences/SEQ-UC-F02-01-system.md) | [SEQ-F02-UC-F02-01-services](../../../05-components/sequences/SEQ-F02-UC-F02-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending) |
| [order-flow](../../../05-components/order-flow/overview.md) | (L2 sequences pending) |
| `risk` (overview pending) | (L2 sequences pending) |
| [ledger](../../../05-components/ledger/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

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

## v2 (IN-015): роль Exchange-provider

Обновление v2 добавляет **Exchange-provider** как отдельную роль/провайдера
(`providerType=provider`): он создаёт заявки через тот же API, а после матчинга **читает
`FillEvent`** (`batch.outputs`, F-04) и инициирует хедж позиции через Venue Execution
Adapter (F-12). Новое требование — **F2-17** ([functional-requirements §FR-ORDERS](../../functional-requirements.md)).
Цепочка: F-02 (создать) → F-04 (fills) → Exchange-provider → F-12 (хедж). Изменения
аддитивны к v1 (реестр правок — в [IN-015.meta](../../../../incoming-docs/IN-015.meta.md)).

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
