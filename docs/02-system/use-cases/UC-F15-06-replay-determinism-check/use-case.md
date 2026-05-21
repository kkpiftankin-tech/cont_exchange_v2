# UC-F15-06. Проверка детерминизма replay

## Feature

- [F-15. Backtest / Replay](../../features/F-15-backtest-replay/)

## Primary Actor

QA / Administrator (`admin`).

## Preconditions

- Существуют ≥ 2 завершённые (`completed`) сессии с идентичным
  `session_config_snapshot`, идентичным `strategy_json`, идентичным
  `date_range`, идентичным `random_seed`.
- Пользователь имеет permission `replay:read`.

## Trigger

Внутренний use case (вызывается из retry-flow или из CI), а также
доступен через REST `POST /api/v1/replay/determinism-checks`
(имплементация в `cpp/backtest/src/app/check_replay_determinism_uc.cpp`).

## Main Flow

1. RBAC `replay:read`.
2. `CheckReplayDeterminismUC.Run(sessionA, sessionB)`:
   1. Подтверждает, что snapshot ID, strategy, date_range, seed совпадают.
   2. Загружает AgentLog обеих сессий (`replay_agentlogs` per session).
   3. Для каждого batch_seq сравнивает:
      - `state_json` (canonical JSON normalization)
      - `action_json`
      - `reward` (с tolerance < 1e-9)
      - `pnl`, `is_value`, `fill_rate` (с tolerance)
      - `solve_time_ms` — игнорируется (non-deterministic)
      - `residual_norm` — с tolerance < 1e-9
3. Сравнивает `replay_summaries` обеих сессий по тем же полям (skip `created_at`, `avg_solve_time_ms`).
4. Результат:
   - `deterministic: true | false`
   - `mismatches: [{batch_seq, field, valueA, valueB, delta}]`

## Alternative Flows

### A1. Snapshot конфигов отличается

`400 Bad Request` — детерминизм проверяется только при идентичных snapshot.

### A2. Один из replay не завершён

`409 Conflict` — нужен `completed` или `partial` со сравнимыми результатами.

## Postconditions

- Read-only операция, БД не мутируется.
- Опционально: результат пишется в новую таблицу `determinism_check_results` (TODO, не реализовано).

## Related Sequence Diagrams

- [System sequence](sequences/SEQ-UC-F15-06-system.md)

## Related Components

- [backtest-service](../../../05-components/backtest-service/overview.md)

## Related Data

- [`replay_agentlogs`](../../../07-data/replay-agentlogs.md), [`replay_summaries`](../../../07-data/replay-summaries.md)

## Source Fragments

- IN-006-FR-016 (F15-16), IN-006-FR-040..043 (snapshot/seed)
