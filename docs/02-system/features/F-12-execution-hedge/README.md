# F-12 — Execution Hedge

> **Статус:** Not implemented (есть только симулятор fill-репортов в venues).

## Описание

Хедж "остатка" торгов на внешних площадках. После внутреннего клиринга matching формирует `ExecutionIntent` для непокрытой части, venues исполняет на реальной бирже, `ExecutionReport` возвращается и применяется к ledger как хедж-позиция.

## TODO

1. Генерация `ExecutionIntent` в matching после batch-цикла.
2. Реальные venue-коннекторы (см. F-11).
3. Хедж-учёт в ledger (`ApplyExecutionReport` сейчас только логирует).
4. Pre-hedge risk-checks (отдельный RPC в risk).

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна уметь выводить сделки на внешние площадки (CEX/DEX/AMM) для хеджирования риска биржи и клиентов.

- `ExecutionIntent` формируется при превышении inventory threshold.
- External Venues Connector размещает child orders на venue.
- `ExecutionReport` применяется к `accounts.venue_allocated`.
- Slippage и rejection rate доступны через post-trade analytics.

Источник: IN-001 §6 FR-HEDGE-001/002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
