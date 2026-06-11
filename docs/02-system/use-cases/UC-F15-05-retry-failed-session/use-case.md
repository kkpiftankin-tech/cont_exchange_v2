<!-- IN-013 frontmatter — Cockburn decomposition level.
---
id: use-case
level: sea
---
-->

# UC-F15-05. Повторно запустить failed / cancelled сессию

## 🧭 Navigation (IN-013)

| Уровень | Где |
| --- | --- |
| ⬆️ Parent feature L0 ☁️ | [F-15-backtest-replay](../../features/F-15-backtest-replay/) |
| ☁️ L0 system sequence | [SEQ-UC-F15-05-system](sequences/SEQ-UC-F15-05-system.md) — system как чёрный ящик |
| 🐟 L2 component sequences | см. component overviews (ссылки в parent feature) |
| 💻 Source code | [`cpp/`](../../../../cpp/) |

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Primary Actor

Owner оригинальной сессии (`analyst`/`admin`) или `admin`.

## Preconditions

- Оригинальная сессия `original_session_id` существует со статусом `failed` или `cancelled`.
- Пользователь имеет permission `replay:create` (retry — создание новой сессии).
- Для strict retry: оригинальный `session_config_snapshot` непустой.

## Trigger

REST `POST /api/v1/replay/sessions/{id}/retry`.

Body:

```json
{
  "new_session_id": "uuid",
  "user_id": "uuid",
  "use_same_config": true,
  "override_config": null
}
```

## Main Flow

1. RBAC `replay:create` + ownership check (analyst — только свои).
2. `RetryReplaySessionUC.Run`:
   - Загружает оригинальную сессию.
   - Если `use_same_config=true`: копирует `session_config_snapshot` и `strategy_json` 1-в-1.
   - Если `use_same_config=false`: применяет `override_config` (новые `solver_config_id` / `risk_limits_id` / `fee_model`), строит новый snapshot через `ReplayConfigSnapshotBuilder`. `strategy_json` берётся из оригинала.
3. Insert в `replay_sessions`:
   - новый `session_id = new_session_id`,
   - `retry_parent_id = original_session_id` (migration 003),
   - status `pending`,
   - `name = "Retry of " + original.name`.
4. RunReplaySession (тот же worker, что и UC-F15-01) подхватывает retry.
5. Возврат `201 Created` с новой сессией.

## Alternative Flows

### A1. Оригинал в pending/running/completed

`409 Conflict` — retry допустим только для `failed`/`cancelled`.

### A2. Оригинал отсутствует

`404 Not Found`.

### A3. override_config валидация

При `use_same_config=false` каждое поле override_config валидируется (наличие `solver_config_id`, валидность `fee_model`).

### A4. Determinism check

После завершения retry, если `use_same_config=true` и original был
`completed/partial`, опционально можно запустить
[UC-F15-06 Determinism Check](../UC-F15-06-replay-determinism-check/use-case.md)
автоматически.

## Postconditions

- Новая сессия в `replay_sessions` с `retry_parent_id`.
- Цепочка retry: `original ← retry1 ← retry2 ← ...` хранится через `retry_parent_id`.

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F15-05-system.md)
- [Service sequence — create](../../../05-components/sequences/SEQ-F15-01-create-session-services.md) (reuses create flow with retry_parent_id)

## Related Contracts

- [REST `POST /api/v1/replay/sessions/{id}/retry`](../../../06-api/rest/replay.md#post-apiv1replaysessionsidretry)
- [Proto `RetryReplayCommand`](../../../../contracts/proto/fob/replay/v1/replay.proto)

## Related Components

- [backtest-service](../../../05-components/backtest-service/overview.md)

## Related Data

- [`replay_sessions`](../../../07-data/replay-sessions.md) (`retry_parent_id` column, migration 003)

## Source Fragments

- IN-006-FR-021 (F15-21)
- Migration 003 (retry_parent_id beyond IN-006)
