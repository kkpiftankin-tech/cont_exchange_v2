# F-07 — Pre-trade Risk Control

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
| [UC-F07-01](../../use-cases/UC-F07-01-pretrade-risk-check/use-case.md) | Pretrade Risk Check | [SEQ-UC-F07-01-system](../../use-cases/UC-F07-01-pretrade-risk-check/sequences/SEQ-UC-F07-01-system.md) | [SEQ-F07-UC-F07-01-services](../../../05-components/sequences/SEQ-F07-UC-F07-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| `risk` (overview pending) | (L2 sequences pending) |
| [order-flow](../../../05-components/order-flow/overview.md) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

## Описание

Синхронный гейт перед резервом средств. Cинусоидальный вызов из order_flow в risk перед каждым CreateFlowOrder.

## Проверки (текущая реализация)

1. Kill-switch (глобальный / по symbol) → `HALT`
2. `total_qty > 0` → иначе `BAD_QTY`
3. `price_low <= price_high` → иначе `BAD_PRICE_RANGE`
4. Margin = `qty * reference_price * 0.1` (placeholder)

[cpp/risk/src/app/risk_uc.cpp](../../../../cpp/risk/src/app/risk_uc.cpp)

## Связанные

- F-02 (Create FlowOrder) — caller
- F-12 (Execution Hedge) — pre-hedge checks

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна предотвращать выставление заявок, нарушающих лимиты по `max_notional`, `max_position`, `max_leverage`, `max_order_rate`, `asset_whitelist`. Возможные решения: `ACCEPT` / `REJECT` / `THROTTLE` (с скорректированными `q_rate`/`q_max`) / `HALT` (kill_switch).

- Любая новая или amend заявка проходит проверку синхронно.
- При REJECT и THROTTLE событие пишется в `risk.alerts`.
- Reference price берётся из Market Data Service.

Источник: IN-001 §6 FR-RISK-001, §5.1 сценарии.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
