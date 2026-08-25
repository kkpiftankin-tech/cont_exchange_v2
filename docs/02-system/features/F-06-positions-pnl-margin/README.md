# F-06 — Positions / PnL / Margin

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
| [UC-F06-01](../../use-cases/UC-F06-01-show-positions/use-case.md) | Show Positions | [SEQ-UC-F06-01-system](../../use-cases/UC-F06-01-show-positions/sequences/SEQ-UC-F06-01-system.md) | [SEQ-F06-UC-F06-01-services](../../../05-components/sequences/SEQ-F06-UC-F06-01-services.md) |

## 🏗 Components Involved

| Component                                             | Drill-down → component overview / L2 sequences |
| ----------------------------------------------------- | ---------------------------------------------- |
| [ledger](../../../05-components/ledger/overview.md)   | (L2 sequences pending)                         |
| `risk` (overview pending)                             | (L2 sequences pending)                         |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending)                         |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

## Описание

Учёт балансов, позиций, PnL и маржинальных требований пользователей. Бэкенд: `cpp/ledger` (балансы) + `cpp/risk` (margin TODO).

## Что реализовано (MVP)

- Балансы `available` / `reserved` по парам (user, currency)
- Резерв и release под ордера (идемпотентно)
- Применение fills из `batch.outputs`

## Что НЕ реализовано

- Positions (open/closed)
- PnL (realised/unrealised)
- Margin (формула в risk — placeholder = 10% notional)
- Persistence (in-memory)

## Файлы

- [cpp/ledger/src/app/ledger_uc.cpp](../../../../cpp/ledger/src/app/ledger_uc.cpp)
- [cpp/risk/src/app/risk_uc.cpp](../../../../cpp/risk/src/app/risk_uc.cpp) — margin placeholder

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна в любой момент показывать клиенту его позиции, прибыль/убыток (реализованный + нереализованный, mark-to-market) и доступный/используемый коллатерал.

- `positions` обновляются после каждого `BatchResult`.
- `accounts.free_balance` + `reserved_balance` + `venue_allocated` + `pending_transfer` показываются в UI.
- Margin утилизация и leverage пересчитываются Risk Manager.

Источник: IN-001 §6 FR-LEDGER-001/002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
