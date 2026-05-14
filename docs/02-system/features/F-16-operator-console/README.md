# F-16 — Operator Console & Kill-Switch

> **Статус:** частично реализовано (kill-switch есть в risk-сервисе, UI отсутствует).

Часть kill-switch уже работает на стороне risk (gRPC `SetKillSwitch` + публикация `risk.alerts`). Полная панель оператора требует:

- веб-UI для просмотра метрик и управления;
- персистентного хранилища состояния (`risk_limits` в PostgreSQL);
- журнала `risk_events` в ClickHouse для аудита.

См. [feature.yaml](feature.yaml).

## Acceptance Criteria (IN-001)

Система должна давать операторам обзор состояния модулей и рынка и возможность приостанавливать торги или изменять ключевые параметры при аномалиях.

- Operator console показывает live-метрики solver, fill rate, latency, active alerts.
- Kill-switch активируется глобально или по инструменту; событие в `risk.alerts(KILL_SWITCH)`.
- Изменения `risk_limits`, `solver_config`, `fee_model` без рестарта.
- Все действия логируются для audit.

Источник: IN-001 §6 FR-OPS-001/002.

## Source Fragments

- IN-001-FR-027, IN-001-FR-028
