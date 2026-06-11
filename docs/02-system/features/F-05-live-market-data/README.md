# F-05 — Live Market Data

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
| [UC-F05-01](../../use-cases/UC-F05-01-stream-market-data/use-case.md) | Stream Market Data | [SEQ-UC-F05-01-system](../../use-cases/UC-F05-01-stream-market-data/sequences/SEQ-UC-F05-01-system.md) | [SEQ-F05-UC-F05-01-services](../../../05-components/sequences/SEQ-F05-UC-F05-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [market-data](../../../05-components/market-data/overview.md) | (L2 sequences pending) |
| `venues` (overview pending) | (L2 sequences pending) |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

## Описание

Потоковые рыночные данные: тикеры с внешних venues, кэшированные и доступные через gRPC.

## Поток

```
venues → Kafka(marketdata.raw) → market_data → cache → gRPC GetLastTicker
```

## Файлы

- [cpp/venues/src/app/venues_loop.cpp](../../../../cpp/venues/src/app/venues_loop.cpp) — синтетический поток (MVP)
- [cpp/market_data/src/app/market_data_uc.cpp](../../../../cpp/market_data/src/app/market_data_uc.cpp) — кэш
- [cpp/market_data/src/transport/grpc_market_data_service.cpp](../../../../cpp/market_data/src/transport/grpc_market_data_service.cpp) — API

## Связанные

- F-04 (Batch Clearing) — потребитель reference prices
- F-11 (External Venues LOB → FOB) — расширение источников

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна показывать пользователю актуальные цены, объёмы, скорости исполнения и общую картину ликвидности по каждому торгуемому инструменту.

- Источник внутренних данных — `batch.outputs` (clearing prices, executed rates).
- Источник внешних — `marketdata.raw` от CEX/DEX (см. F-11).
- Streaming по WebSocket с приемлемой latency (NFR-EXEC).
- Ticker cache в Redis для быстрого ответа.

Источник: IN-001 §6 FR-MD-001, §5 продуктовая модель.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
