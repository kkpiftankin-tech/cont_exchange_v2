---
id: CMP-BACKTEST-REPLAY
phase: 05-components
status: not-implemented
component: backtest-replay
related:
  - docs/02-system/features/F-15-backtest-replay/
---

# Component: Backtest & Replay Engine

## Назначение

Воспроизведение исторических данных через matching/risk/ledger в test-режиме
для проверки изменений в solver, тюнинга risk-политик и регрессионных тестов.

## Ответственность

- Загрузка истории из ClickHouse (`fills`, `batch_results`, `marketdata`).
- Запуск matching/risk/ledger в replay-режиме.
- Сравнение результатов между прогонами.
- Сбор AgentLog для анализа.

## Не отвечает за

- Изменение прод-данных. Все операции в изолированной среде.

## Входы

- Исторические `marketdata`, `flow_orders`, `batch_results`.
- Тестовые конфигурации `solver_config`.

## Выходы

- Тестовые BatchResult, FillEvent.
- AgentLog в `agent_logs`.
- Отчёты сравнения политик.

## Связанная фича

- F-15 — [../../02-system/features/F-15-backtest-replay/](../../02-system/features/F-15-backtest-replay/).

## Participates In Features

- [F-15](../../02-system/features/F-15-backtest-replay/)

## Participates In Use Cases

- [UC-F15-01](../../02-system/use-cases/UC-F15-01-replay-historical-batch/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F15-UC-F15-01-services](../sequences/SEQ-F15-UC-F15-01-services.md)

## Owned Contracts

- (planned) `fob.agent.v1` — [../../06-api/grpc/](../../06-api/grpc/)

## Produced Events

- (planned) `agent.logs`, `batch.outputs.replay`

## Consumed Events

- [orders.normalized](../../06-api/messaging/orders-normalized.md) (archive)
- [batch.outputs](../../06-api/messaging/batch-outputs.md) (archive)

## Data Access

- (planned) read: `fills`, `batch_results`, `marketdata` (ClickHouse)
- (planned) write: `agent_logs`, `batch_results_replay` — [../../07-data/data-overview.md](../../07-data/data-overview.md)

## Зависимости

- Требует ClickHouse (см. ADR-004).
- Требует детерминированный matching (см. F-15 knownIssues).

## Статус

Not implemented.
