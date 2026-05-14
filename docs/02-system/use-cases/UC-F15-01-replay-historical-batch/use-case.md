# UC-F15-01. Replay исторического batch

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Primary Actor

Researcher / Operator

## Preconditions

- Имеется исторические `orders.normalized` и `batch.outputs` в Kafka long-retention или archive.
- Backtest engine сконфигурирован.

## Trigger

Researcher запускает replay по периоду.

## Main Flow

1. Researcher выбирает временной диапазон и параметры solver.
2. Backtest engine читает `orders.normalized` из архива.
3. Engine прогоняет matching deterministic-mode.
4. Engine публикует результаты в отдельный topic (`batch.outputs.replay`).
5. Observability сравнивает с историческим `batch.outputs`.

## Postconditions

- Получены метрики сравнения (residual_norm, fill rate, VWAP delta).

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F15-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F15-UC-F15-01-services.md)

## Related Contracts

- [orders.normalized](../../../06-api/messaging/orders-normalized.md)
- [batch.outputs](../../../06-api/messaging/batch-outputs.md)
- (планируется) `fob.agent.v1` для backtest replay

## Related Components

- (планируется) [backtest-replay](../../../05-components/backtest-replay/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [observability-reporting](../../../05-components/observability-reporting/overview.md)

## Related Data

- архив Kafka, (планируется) `agent_logs` в ClickHouse
