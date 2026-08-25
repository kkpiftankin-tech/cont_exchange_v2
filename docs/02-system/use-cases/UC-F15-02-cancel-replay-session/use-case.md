<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F15-02. Отменить replay-сессию

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-15-backtest-replay](../../features/F-15-backtest-replay/) |
| ☁️ L0 system sequence | [SEQ-UC-F15-02-system](sequences/SEQ-UC-F15-02-system.md) — system как чёрный ящик |
| 🌊 L1 service sequence | [SEQ-F15-02-replay-cycle-services](../../../05-components/sequences/SEQ-F15-02-replay-cycle-services.md) — взаимодействие сервисов |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Primary Actor

Owner сессии (роль `analyst`/`admin`) или Administrator.

## Preconditions

- Сессия существует в `replay_sessions` со статусом `pending` или `running`.
- Пользователь имеет permission `replay:cancel`.

## Trigger

- REST `DELETE /api/v1/replay/sessions/{id}`, или
- Kafka команда `CancelReplayCommand` в `replay.commands`.

## Main Flow

1. RBAC проверка `replay:cancel`. Для `analyst` — ownership check
   (`replay_sessions.user_id == requester`); `admin` — без ownership.
2. `CancelReplaySessionUC` устанавливает session-scoped `ICancellationToken`.
3. Если сессия `pending`: немедленно `UPDATE status='cancelled'`, `completed_at=now()`, без partial summary.
4. Если сессия `running`: токен будет поллинговаться `RunReplaySession` перед каждой следующей итерацией. Активный батч завершается, partial AgentLog и partial ReplaySummary сохраняются.
5. Backtest публикует `ReplayCancelledEvent` в `replay.results` с `cancelled_at_batch_seq`, `reason`.
6. Запись в `audit_log`.

## Alternative Flows

### A1. Сессия уже completed/failed/cancelled

`409 Conflict`. Никаких изменений.

### A2. RBAC отказ

`403 Forbidden`. `audit_log.status=denied`.

### A3. Параллельный race: pending → running одновременно с cancel

`CancelReplaySession` ведёт `cancelled_session_ids` set. Если worker уже
перевёл pending в running, токен принудительно срабатывает после первого
батча; результат — `cancelled`, не `running`. Гарантия: статус не залипает
в `running` после успешного DELETE.

## Postconditions

- `replay_sessions.status = 'cancelled'`.
- `replay_summaries` — partial (с `partial=true`), если хотя бы один батч был обработан.
- Опубликован `ReplayCancelledEvent`.
- AgentLog за обработанные батчи сохранён в ClickHouse.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F15-02-system.md)
- [Service sequence](../../../05-components/sequences/SEQ-F15-03-cancel-services.md)

## Related Contracts

- [REST `DELETE /api/v1/replay/sessions/{id}`](../../../06-api/rest/replay.md#delete-apiv1replaysessionsid)
- [Proto `CancelReplayCommand`](../../../../contracts/proto/fob/replay/v1/replay.proto)

## Related Components

- [backtest-service](../../../05-components/backtest-service/overview.md)

## Related Data

- [`replay_sessions`](../../../07-data/replay-sessions.md), [`replay_summaries`](../../../07-data/replay-summaries.md), [`audit_log`](../../../07-data/replay-rbac.md)

## Source Fragments

- IN-006-FR-012 (F15-12), IN-006-FR-037 (partial save)
