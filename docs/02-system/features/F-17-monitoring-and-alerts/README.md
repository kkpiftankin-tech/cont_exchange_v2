# F-17 — Monitoring & Alerting

> **Статус:** частично реализовано (есть JSON-логирование, нет экспорта метрик и алертов).

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
| [UC-F17-01](../../use-cases/UC-F17-01-fire-alert/use-case.md) | Fire Alert | [SEQ-UC-F17-01-system](../../use-cases/UC-F17-01-fire-alert/sequences/SEQ-UC-F17-01-system.md) | [SEQ-F17-UC-F17-01-services](../../../05-components/sequences/SEQ-F17-UC-F17-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| `observability` (overview pending) | (L2 sequences pending) |
| `risk` (overview pending) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

Сейчас [cpp/observability](../../../../cpp/observability/) только подписан на ключевые топики и пишет JSON в stdout. До прод-уровня нужны:

- Prometheus / OpenTelemetry экспортёры;
- ClickHouse-ingestion через Kafka table engine;
- алерты в Operator UI и внешние каналы;
- dashboard'ы по solve time, fill rate, IS, risk events.

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна непрерывно отслеживать показатели производительности и риска и оперативно уведомлять о сбоях, перегрузках и аномальном поведении рынка.

- Метрики: solve_time, residual_norm, fill_rate, IS distribution, consumer lag, venue connectivity.
- Алерты конфигурируемы по порогам.
- Дашборды доступны операторам и compliance.
- История метрик и алертов хранится в ClickHouse (`risk_events`, `batch_results`).

Источник: IN-001 §7 NFR-OBS-001..004.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
