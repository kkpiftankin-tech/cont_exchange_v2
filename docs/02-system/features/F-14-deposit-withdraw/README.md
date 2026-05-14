# F-14 — Deposit & Withdraw

> **Статус:** Not implemented.

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
