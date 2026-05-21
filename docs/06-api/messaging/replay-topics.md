# Kafka Topics — Replay (F-15)

## Topics

- `replay.commands` (in)
- `replay.results` (out)

Создание dev топиков — в [infra/kafka/create_topics.sh](../../../infra/kafka/create_topics.sh):

```bash
create_topic replay.commands 604800000   # retention 7d
create_topic replay.results  604800000   # retention 7d
```

## replay.commands

### Purpose

Опциональная альтернатива REST для управления replay-сессиями.
Полезна для batch-загрузок (CI / scheduled backtests) и интеграций.

### Producer

- gateway (REST → Kafka проксирование, optional)
- внешние системы (CI runners, scheduled jobs)

### Consumer

- [backtest-service](../../05-components/backtest-service/overview.md)
  (`ReplayCommandsKafkaConsumer`)

### Settings

| Параметр       | Значение           |
| -------------- | ------------------ |
| Retention      | 7 дней              |
| Partition key  | `session_id`       |
| Delivery       | at-least-once, idempotent consumer (idempotency через `session_id`) |
| Schema         | `fob.replay.v1.{CreateReplayCommand, CancelReplayCommand, RetryReplayCommand, PauseReplayCommand}` |

### Message types

- `fob.replay.v1.CreateReplayCommand` — эквивалент `POST /api/v1/replay/sessions`.
- `fob.replay.v1.CancelReplayCommand` — эквивалент `DELETE`.
- `fob.replay.v1.RetryReplayCommand` — эквивалент `POST /sessions/{id}/retry`.
- `fob.replay.v1.PauseReplayCommand` — опционально, ещё не реализовано.

### Schema

См. [contracts/proto/fob/replay/v1/replay.proto](../../../contracts/proto/fob/replay/v1/replay.proto).

## replay.results

### Purpose

Лайфцикл и progress-события сессии. Используется gateway для WebSocket
push в UI и observability-reporting для метрик.

### Producer

- [backtest-service](../../05-components/backtest-service/overview.md)
  (`KafkaReplayEventPublisher`)

### Consumer

- gateway (WebSocket bridge — planned)
- observability-reporting

### Settings

| Параметр       | Значение           |
| -------------- | ------------------ |
| Retention      | 7 дней              |
| Partition key  | `session_id`       |
| Delivery       | at-least-once      |
| Schema         | `fob.replay.v1.{ReplayProgressEvent, ReplayCompletedEvent, ReplayFailedEvent, ReplayCancelledEvent}` |

### Message types

- `ReplayProgressEvent` — каждые N батчей (`batch_seq`, `total_batches`, `partial_summary`).
- `ReplayCompletedEvent` — финальный `summary`, `total_batches`, `completed_at`.
- `ReplayFailedEvent` — `error_details`, `failed_at_batch_seq`, `partial_summary`.
- `ReplayCancelledEvent` — `reason`, `cancelled_at_batch_seq`, `partial_summary`.

### Schema

См. [contracts/proto/fob/replay/v1/replay.proto](../../../contracts/proto/fob/replay/v1/replay.proto).

## Used In Features

- [F-15. Backtest / Replay](../../02-system/features/F-15-backtest-replay/)

## Used In Use Cases

- [UC-F15-01 Create](../../02-system/use-cases/UC-F15-01-create-replay-session/use-case.md)
- [UC-F15-02 Cancel](../../02-system/use-cases/UC-F15-02-cancel-replay-session/use-case.md)
- [UC-F15-05 Retry](../../02-system/use-cases/UC-F15-05-retry-failed-session/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F15-01-create-session-services](../../05-components/sequences/SEQ-F15-01-create-session-services.md)
- [SEQ-F15-02-replay-cycle-services](../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md)
- [SEQ-F15-03-cancel-services](../../05-components/sequences/SEQ-F15-03-cancel-services.md)
