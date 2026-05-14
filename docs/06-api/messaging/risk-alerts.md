# Kafka Topic: risk.alerts

## Purpose

События риска: pre-trade reject, post-trade margin breach, kill-switch активации, ликвидации.

## Producer

- [risk-manager](../../05-components/risk-manager/overview.md)

## Consumers

- [observability-reporting](../../05-components/observability-reporting/overview.md) — alerting / dashboards
- (planned) Operator console — UI feed

## Settings

| Параметр | Значение |
| --- | --- |
| Retention | 30 дней |
| Partition key | `user_id` или `alert_id` |
| Delivery | at-least-once, idempotent consumer |
| Schema | `fob.risk.v1.RiskAlert` (Protobuf) |

## Message schema

См. [contracts/proto/fob/risk/v1/risk.proto](../../../contracts/proto/fob/risk/v1/risk.proto).

## Used In Features

- [F-07. Pre-trade Risk](../../02-system/features/F-07-pretrade-risk/)
- [F-08. Liquidations](../../02-system/features/F-08-posttrade-risk-and-liquidations/)
- [F-16. Operator Console / Kill-Switch](../../02-system/features/F-16-operator-console/)
- [F-17. Monitoring](../../02-system/features/F-17-monitoring-and-alerts/)

## Used In Use Cases

- [UC-F07-01](../../02-system/use-cases/UC-F07-01-pretrade-risk-check/use-case.md)
- [UC-F08-01](../../02-system/use-cases/UC-F08-01-liquidate-position/use-case.md)
- [UC-F16-01](../../02-system/use-cases/UC-F16-01-trigger-kill-switch/use-case.md)
- [UC-F17-01](../../02-system/use-cases/UC-F17-01-fire-alert/use-case.md)

## Used In Sequence Diagrams

- [SEQ-F07-UC-F07-01-services](../../05-components/sequences/SEQ-F07-UC-F07-01-services.md)
- [SEQ-F08-UC-F08-01-services](../../05-components/sequences/SEQ-F08-UC-F08-01-services.md)
- [SEQ-F16-UC-F16-01-services](../../05-components/sequences/SEQ-F16-UC-F16-01-services.md)
- [SEQ-F17-UC-F17-01-services](../../05-components/sequences/SEQ-F17-UC-F17-01-services.md)
