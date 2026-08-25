# F-08 — Post-trade Risk Monitoring & Liquidations

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
| [UC-F08-01](../../use-cases/UC-F08-01-liquidate-position/use-case.md) | Liquidate Position | [SEQ-UC-F08-01-system](../../use-cases/UC-F08-01-liquidate-position/sequences/SEQ-UC-F08-01-system.md) | [SEQ-F08-UC-F08-01-services](../../../05-components/sequences/SEQ-F08-UC-F08-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| `risk` (overview pending) | (L2 sequences pending) |
| [ledger](../../../05-components/ledger/overview.md) | (L2 sequences pending) |
| `matching` (overview pending) | (L2 sequences pending) |
| `observability` (overview pending) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

Перерасчёт margin/VaR после каждого batch, margin calls, принудительные ликвидации. Зависит от F-06 (positions/PnL) и F-04 (источник batch-событий).

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна отслеживать изменение рисков после сделок, выявлять нехватку маржи, формировать margin call и принудительно ликвидировать позиции при необходимости.

- Триггер — каждое `batch.outputs`.
- При `maintenance_margin` breach генерируется `risk.alerts(MARGIN_CALL)`.
- Если в течение grace period маржа не восстановлена — создаётся ликвидационный `FlowOrder`.
- Все события и решения логируются в `risk_events`.

Источник: IN-001 §6 FR-RISK-002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
