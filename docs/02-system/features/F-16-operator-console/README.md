# F-16 — Operator Console & Kill-Switch

> **Статус:** частично реализовано (kill-switch есть в risk-сервисе, UI отсутствует).

## 🧭 Navigation Map (IN-013 drill-down)

Эта секция — **карта документации сверху вниз** для фичи.
Каждый уровень имеет свой ответ на «что/как», и каждая ссылка
ведёт на следующий уровень детализации.

```text
   ┌─ Уровень ──────────────┬─ Артефакт ─────────────────────────────────┐
☁️ L0 │ Что система делает    │ Эта страница + L0 system sequence(s) ниже  │
🌊 L1 │ Какие функции у фичи?  │ Use Cases (таблица ниже)                   │
   │ Какие сервисы участвуют?│ L1 service sequences (per-UC)              │
🐟 L2 │ Из каких классов       │ Component overviews + L2 sequences         │
   │ состоит сервис?        │                                            │
💻 src │ Код                    │ cpp/<component>/src/...                    │
   └────────────────────────┴────────────────────────────────────────────┘
```

## 📋 Use Cases (L1 🌊)

| UC | Имя | L0 sequence ☁️ | L1 sequence 🌊 |
| --- | --- | --- | --- |
| [UC-F16-01](../../use-cases/UC-F16-01-trigger-kill-switch/use-case.md) | Trigger Kill Switch | [SEQ-UC-F16-01-system](../../use-cases/UC-F16-01-trigger-kill-switch/sequences/SEQ-UC-F16-01-system.md) | [SEQ-F16-UC-F16-01-services](../../../05-components/sequences/SEQ-F16-UC-F16-01-services.md) |

## 🏗 Components Involved

| Component | Drill-down → component overview / L2 sequences |
| --- | --- |
| [gateway](../../../05-components/gateway/overview.md) | (L2 sequences pending) |
| `risk` (overview pending) | (L2 sequences pending) |
| `matching` (overview pending) | (L2 sequences pending) |
| `observability` (overview pending) | (L2 sequences pending) |

> См. также [`docs/00-methodology/functional-hierarchy-and-decomposition.md`](../../../00-methodology/functional-hierarchy-and-decomposition.md) — полное описание двухосевой модели IN-013.

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
