<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F17-01. Сгенерировать alert и нотифицировать on-call

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-17-monitoring-and-alerts](../../features/F-17-monitoring-and-alerts/) |
| ☁️ L0 system sequence | [SEQ-UC-F17-01-system](sequences/SEQ-UC-F17-01-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F17-UC-F17-01-services](../../../05-components/sequences/SEQ-F17-UC-F17-01-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-17. Monitoring & Alerting](../../features/F-17-monitoring-and-alerts/)

## Primary Actor

System (Observability)

## Supporting Actors

- On-call Engineer

## Preconditions

- Метрика пересекает threshold (consumer lag, residual_norm, error rate, ledger mismatch).

## Trigger

Observability обнаруживает breach.

## Main Flow

1. Observability читает поток метрик (Kafka, Prometheus).
2. Сравнивает с threshold.
3. Формирует alert.
4. Шлёт в notifier (Slack / PagerDuty — планируется).
5. Запись в audit log.

## Postconditions

- Alert зарегистрирован.
- On-call оповещён.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F17-01-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F17-UC-F17-01-services.md)

## Related Contracts

- `fob.observability.v1` — обзор контрактов
- [risk.alerts](../../../06-api/messaging/risk-alerts.md) (для risk-related alerts)

## Related Components

- [observability-reporting](../../../05-components/observability-reporting/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)

## Related Data

- (планируется) `risk_events` в ClickHouse
