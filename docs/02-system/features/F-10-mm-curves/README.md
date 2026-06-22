# F-10 — Market-Maker Liquidity Curves

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
| [UC-F10-01](../../use-cases/UC-F10-01-publish-mm-curve/use-case.md) | Publish Mm Curve | [SEQ-UC-F10-01-system](../../use-cases/UC-F10-01-publish-mm-curve/sequences/SEQ-UC-F10-01-system.md) | [SEQ-F10-UC-F10-01-services](../../../05-components/sequences/SEQ-F10-UC-F10-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [order-flow](../../../05-components/order-flow/overview.md) | (L2 sequences pending) |
| `matching` (overview pending) | (L2 sequences pending) |
| [ledger](../../../05-components/ledger/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

Полноценная поддержка непрерывных CSLO-кривых от маркет-мейкеров с slope-функцией price↔speed. См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна позволять маркет-мейкеру публиковать и изменять непрерывные кривые цен и скоростей с параметрами `P_L`, `P_H`, `Q`, `U`, slope, а также видеть результат своего участия в реальном времени.

- MM может в любой момент изменить параметры кривой; обновление учитывается в следующем batch.
- В UI отображаются PnL, текущий inventory, доля toxic flow.
- Inventory caps работают как hard-stop.

Источник: IN-001 §6 FR-MM-001/002, §5.2 сценарии MM.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
