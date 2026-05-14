# Компонент: observability

Аггрегатор событий: подписан на ключевые топики и пишет структурированные JSON-логи. В продакшене сюда должны встать OpenTelemetry/Prometheus экспортёры.

## Код

- [cpp/observability/](../../../cpp/observability/) — каталог
- [src/main.cpp](../../../cpp/observability/src/main.cpp) — entrypoint
- [src/app/obs_loop.cpp](../../../cpp/observability/src/app/obs_loop.cpp) — один поток на все топики

## Что слушает

| Топик | Сообщение | Что логгируется |
|---|---|---|
| `risk.alerts` | `fob.risk.v1.RiskAlert` | severity, type, instrument, msg |
| `batch.outputs` | `fob.matching.v1.BatchResult` | batch_id, fills_count, num_active_orders |
| `execution.reports` | `fob.execution.v1.ExecutionReport` | intent_id, status, venue |

## Конфигурация

| env | Default | Назначение |
|---|---|---|
| `KAFKA_BROKERS` | redpanda:9092 | Kafka |

## Что НЕ реализовано

- Экспорт метрик (Prometheus/OTel).
- Заливка в Loki/Elasticsearch/ClickHouse.
- Алерты по порогам (residualNorm, solveTimeMs).
- Дашборды.

## Связанные фичи

- F-04, F-05, F-07, F-11, F-12, F-15 — все консьюмят телеметрию из observability

## Participates In Features

- [F-04](../../02-system/features/F-04-batch-clearing/), [F-08](../../02-system/features/F-08-posttrade-risk-and-liquidations/), [F-12](../../02-system/features/F-12-execution-hedge/), [F-13](../../02-system/features/F-13-posttrade-report/), [F-15](../../02-system/features/F-15-backtest-replay/), [F-16](../../02-system/features/F-16-operator-console/), [F-17](../../02-system/features/F-17-monitoring-and-alerts/)

## Participates In Use Cases

- [UC-F04-01](../../02-system/use-cases/UC-F04-01-run-batch-clearing/use-case.md), [UC-F08-01](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md), [UC-F12-01](../../02-system/use-cases/UC-F12-01-execute-hedge/use-case.md), [UC-F13-01](../../02-system/use-cases/UC-F13-01-generate-posttrade-report/use-case.md), [UC-F15-01](../../02-system/use-cases/UC-F15-01-replay-historical-batch/use-case.md), [UC-F16-01](../../02-system/use-cases/UC-F16-01-trigger-kill-switch/use-case.md), [UC-F17-01](../../02-system/use-cases/UC-F17-01-fire-alert/use-case.md)

## Participates In Sequence Diagrams

- [SEQ-F04-UC-F04-01-services](../sequences/SEQ-F04-UC-F04-01-services.md), [SEQ-F08-UC-F08-01-services](../sequences/SEQ-F08-UC-F08-01-services.md), [SEQ-F12-UC-F12-01-services](../sequences/SEQ-F12-UC-F12-01-services.md), [SEQ-F13-UC-F13-01-services](../sequences/SEQ-F13-UC-F13-01-services.md), [SEQ-F15-UC-F15-01-services](../sequences/SEQ-F15-UC-F15-01-services.md), [SEQ-F16-UC-F16-01-services](../sequences/SEQ-F16-UC-F16-01-services.md), [SEQ-F17-UC-F17-01-services](../sequences/SEQ-F17-UC-F17-01-services.md)

## Owned Contracts

- `fob.observability.v1` — [../../06-api/grpc/](../../06-api/grpc/)
- (planned) report endpoints — [../../06-api/rest/](../../06-api/rest/)

## Produced Events

- (planned) alert webhooks to Slack/PagerDuty

## Consumed Events

- [risk.alerts](../../06-api/messaging/risk-alerts.md)
- [batch.outputs](../../06-api/messaging/batch-outputs.md)
- [execution.reports](../../06-api/messaging/execution-reports.md)

## Data Access

- (planned) write: `fills`, `batch_results`, `risk_events`, `execution_reports`, `agent_logs` в ClickHouse — [../../07-data/data-overview.md](../../07-data/data-overview.md)
