# SEQ-F15-UC-F15-01-services. (Superseded) — Replay: service view

> **Статус:** superseded. Этот placeholder заменён более детальными
> service-уровневыми диаграммами после ингеста IN-006.

## Преемники

- [SEQ-F15-01-create-session-services](SEQ-F15-01-create-session-services.md) — создание сессии (REST/Kafka → backtest → PG → ack).
- [SEQ-F15-02-replay-cycle-services](SEQ-F15-02-replay-cycle-services.md) — основной replay-цикл (per-batch matching + risk + shadow ledger + AgentLog).
- [SEQ-F15-03-cancel-services](SEQ-F15-03-cancel-services.md) — отмена сессии (ICancellationToken, partial save).
- [SEQ-F15-04-audit-mode-services](SEQ-F15-04-audit-mode-services.md) — audit-mode single batch + diff.

## Feature

- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/)
