# F-03 — FlowOrder Lifecycle (Amend / Cancel)

> **Статус:** частично реализовано (только Cancel, с критическим багом).

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
| [UC-F03-01](../../use-cases/UC-F03-01-amend-cancel-order/use-case.md) | Amend Cancel Order | [SEQ-UC-F03-01-system](../../use-cases/UC-F03-01-amend-cancel-order/sequences/SEQ-UC-F03-01-system.md) | [SEQ-F03-UC-F03-01-services](../../../05-components/sequences/SEQ-F03-UC-F03-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending) |
| [order-flow](../../../05-components/order-flow/overview.md) | (L2 sequences pending) |
| `risk` (overview pending) | (L2 sequences pending) |
| [ledger](../../../05-components/ledger/overview.md) | (L2 sequences pending) |
| `matching` (overview pending) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

## Описание

Управление активной заявкой после её создания: изменение параметров (amend), отмена, автоматическое истечение по `time_in_force`. Каждое изменение проходит повторный risk-check.

## Связанные

- F-02 (Create FlowOrder) — родитель сущности
- F-04 (Batch Clearing) — потребитель cancel/amend событий
- F-07 (Pre-trade Risk) — повторный гейт

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна позволять клиенту изменять параметры активной заявки (`p_low`, `p_high`, `q_rate`, `q_max`, окно) и полностью её отменять.

- Изменения проходят повторный pre-trade risk check.
- При amend система пересчитывает preview VWAP/IS.
- При cancel неисполненная часть освобождает резерв в Ledger.
- История изменений сохраняется для audit.

Источник: IN-001 §6 FR-ORDER-003, §5.1.2.

## Source Fragments

- IN-001-FR-027
- IN-001-FR-028
