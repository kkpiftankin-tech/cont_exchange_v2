<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F15-01. Создать replay-сессию

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-15-backtest-replay](../../features/F-15-backtest-replay/) |
| ☁️ L0 system sequence | [SEQ-UC-F15-01-system](sequences/SEQ-UC-F15-01-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F15-UC-F15-01-services](../../../05-components/sequences/SEQ-F15-UC-F15-01-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Primary Actor

Analyst (роль `analyst`) или Administrator (`admin`).

## Supporting Actors

- Backtest Service — оркестратор.
- ClickHouse — источник исторических данных (`batchresults`, `fills`, `marketdata_snapshots`).
- PostgreSQL — конфигурации (`solver_config`, `risk_limits`, `fee_model`) и лайфцикл сессии (`replay_sessions`, `replay_summaries`).
- Matching Backend, Risk Manager, Shadow Collateral Ledger — invoked в replay-цикле.

## Preconditions

- Пользователь авторизован, имеет permission `replay:create`.
- Запрошенный `date_range_from`..`date_range_to` покрыт историческими данными в ClickHouse.
- `solver_config_id` и `risk_limits_id` валидны и доступны в PostgreSQL (или переданы inline JSON).
- `strategy` — непустой массив FlowOrder с инвариантами: $p_L > 0$, $p_L \le p_H$, $q_{\text{rate}} > 0$, $Q_{\max} > 0$.

## Trigger

- REST `POST /api/v1/replay/sessions`, или
- Kafka команда `CreateReplayCommand` в топик `replay.commands`.

## Main Flow

1. Backtest Service принимает запрос; `RbacEngine` проверяет permission `replay:create`.
2. Валидация `strategy` (F15-22..F15-25). При ошибке возвращается `400` + `error_code=validation_error`.
3. Проверка покрытия исторических данных через `HistoricalBatchLoader` (F15-2). При пустом результате — `400` + `error_code=no_historical_data`.
4. `ReplayConfigSnapshotBuilder` материализует snapshot: solver_config, risk_limits, fee_model, reward_config, random_seed (F15-40..F15-41).
5. Insert в `replay_sessions` со статусом `pending`, заполненным `session_config_snapshot` и `total_batches=NULL` (будет вычислен на старте run).
6. Возврат `201 Created` с `session_id`, `status=pending`, `created_at`.
7. `RunReplaySessionUC` отдельным worker'ом подхватывает pending сессии, переводит в `running`, инициализирует shadow namespace, запускает основной цикл (см. [UC-F15-04 audit](../UC-F15-04-audit-mode-replay/use-case.md) или [SEQ-F15-02 replay-cycle](../../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md)).
8. По завершении: ReplaySummary в `replay_summaries`, status `completed` или `failed`. Лайфцикл-события — в `replay.results`.

## Alternative Flows

### A1. Невалидный strategy

Любое нарушение инвариантов FlowOrder → отказ на шаге 2 с детальным `details.field`.

### A2. Нет исторических данных

ClickHouse возвращает 0 записей в `batchresults` за период → отказ на шаге 3.

### A3. RBAC отказ

Permission отсутствует → `403 Forbidden`; запись в `audit_log` со `status=denied`.

### A4. Конфликт session_id

Если клиент передал свой `session_id` и он уже занят → `409 Conflict`.

## Postconditions

- Запись в `replay_sessions` со статусом `pending` или `running`.
- Запись в `audit_log` (RBAC trail).
- Опубликован `ReplayProgressEvent` (status=`pending` → `running`).

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F15-01-system.md)
- [Service sequence — create](../../../05-components/sequences/SEQ-F15-01-create-session-services.md)
- [Service sequence — replay cycle](../../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md)
- [Internal: backtest-service SEQ-BACKTEST-01](../../../05-components/backtest-service/sequences/SEQ-BACKTEST-01-create-session.md)

## Related Contracts

- [REST `POST /api/v1/replay/sessions`](../../../06-api/rest/replay.md#post-apiv1replaysessions)
- [Proto `CreateReplayCommand`](../../../../contracts/proto/fob/replay/v1/replay.proto)
- [Kafka `replay.commands`](../../../06-api/messaging/replay-topics.md#replaycommands)
- [Kafka `replay.results`](../../../06-api/messaging/replay-topics.md#replayresults)

## Related Components

- [backtest-service](../../../05-components/backtest-service/overview.md)
- [matching-fob-core](../../../05-components/matching-fob-core/overview.md)
- [risk-manager](../../../05-components/risk-manager/overview.md)
- [ledger](../../../05-components/ledger/overview.md)

## Related Data

- PostgreSQL: [`replay_sessions`](../../../07-data/replay-sessions.md), [`replay_summaries`](../../../07-data/replay-summaries.md), [`audit_log`](../../../07-data/replay-rbac.md)
- ClickHouse: [`replay_agentlogs`](../../../07-data/replay-agentlogs.md), `batchresults` (read-only)

## Source Fragments

- IN-006-FR-001 (F15-1), IN-006-FR-002 (F15-2), IN-006-FR-022..025 (validation), IN-006-FR-040..043 (snapshot/seed)
