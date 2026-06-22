# F-14 — Deposit & Withdraw

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
| [UC-F14-01](../../use-cases/UC-F14-01-deposit-funds/use-case.md) | Deposit Funds | [SEQ-UC-F14-01-system](../../use-cases/UC-F14-01-deposit-funds/sequences/SEQ-UC-F14-01-system.md) | [SEQ-F14-UC-F14-01-services](../../../05-components/sequences/SEQ-F14-UC-F14-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending) |
| [ledger](../../../05-components/ledger/overview.md) | (L2 sequences pending) |
| `custody-adapter` (overview pending) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

Депозиты/выводы средств (on-chain и fiat), AML/KYC, сверка с custody. См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна безопасно обрабатывать ввод и вывод средств, обеспечивая согласованность внутренних балансов с blockchain/custody.

- `collateral_transfers` фиксирует все операции со статусами `pending → processing → confirmed | failed | cancelled`.
- Депозит зачисляется в `accounts.free_balance` после on-chain confirmation.
- Вывод проверяется по доступному балансу и risk-флагам.
- При rebalance между venue корректируется `venue_allocated`.

Источник: IN-001 §6 FR-LEDGER-003 + раздел «Перемещение коллатерала» (см. Conflict Note в feature-index).

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
