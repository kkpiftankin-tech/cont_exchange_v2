# F-13 — Post-Trade Report

> **Статус:** Not implemented.

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
| [UC-F13-01](../../use-cases/UC-F13-01-generate-posttrade-report/use-case.md) | Generate Posttrade Report | [SEQ-UC-F13-01-system](../../use-cases/UC-F13-01-generate-posttrade-report/sequences/SEQ-UC-F13-01-system.md) | [SEQ-F13-UC-F13-01-services](../../../05-components/sequences/SEQ-F13-UC-F13-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| `observability` (overview pending) | (L2 sequences pending) |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

Детальный отчёт по завершённым FlowOrder: VWAP, IS, декомпозиция, экспорт CSV/PDF. Требует ClickHouse и сервис-агрегатор. См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна предоставлять клиенту подробные отчёты по завершённым заявкам: фактический VWAP, IS (среднее, std, ключевые квантили), профиль исполнения во времени, риск-показатели.

- Доступны для скачивания (JSON / CSV / PDF).
- Декомпозиция IS: spread + temporary impact + permanent impact + volatility.
- Регуляторные выгрузки агрегируются по периоду.

Источник: IN-001 §6 FR-REPORT-001/002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
