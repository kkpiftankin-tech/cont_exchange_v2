---
id: DOC-DATA-REPLAY-SESSIONS
phase: 07-data
status: implemented
owner: core-team
source:
  - IN-006 § ReplaySession + замечания о PostgreSQL DDL
related:
  - docs/02-system/features/F-15-backtest-replay/
  - cpp/backtest/migrations/postgres/001_f15_replay_schema.sql
  - cpp/backtest/migrations/postgres/003_f15_retry_parent_id.sql
---

# PostgreSQL: replay_sessions (+ compare cache + audit runs)

> **Источник истины DDL:** [cpp/backtest/migrations/postgres/001_f15_replay_schema.sql](../../cpp/backtest/migrations/postgres/001_f15_replay_schema.sql)
> и [003_f15_retry_parent_id.sql](../../cpp/backtest/migrations/postgres/003_f15_retry_parent_id.sql).

## Таблица replay_sessions

Лайфцикл одной replay-сессии.

### DDL (из 001 + 003)

```sql
CREATE TABLE IF NOT EXISTS replay_sessions (
  session_id              UUID PRIMARY KEY,
  user_id                 TEXT NOT NULL,
  name                    VARCHAR(255) NOT NULL,
  strategy                JSONB NOT NULL,
  date_range_from         TIMESTAMPTZ NOT NULL,
  date_range_to           TIMESTAMPTZ NOT NULL,
  solver_config_id        TEXT NOT NULL,
  risk_limits_id          TEXT NOT NULL,
  fee_model               JSONB NOT NULL,
  session_config_snapshot JSONB,                                -- замороженный snapshot
  status                  TEXT NOT NULL
                          CHECK (status IN ('pending','running','completed','failed','cancelled')),
  total_batches           INTEGER,
  progress_batches        INTEGER NOT NULL DEFAULT 0,
  created_at              TIMESTAMPTZ NOT NULL DEFAULT now(),
  started_at              TIMESTAMPTZ,
  completed_at            TIMESTAMPTZ,
  error_details           TEXT,
  retry_parent_id         UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL,
  CHECK (date_range_from <= date_range_to),
  CHECK (progress_batches >= 0),
  CHECK (total_batches IS NULL OR total_batches >= progress_batches)
);

CREATE INDEX IF NOT EXISTS replay_sessions_user_id_idx          ON replay_sessions (user_id);
CREATE INDEX IF NOT EXISTS replay_sessions_status_idx           ON replay_sessions (status);
CREATE INDEX IF NOT EXISTS replay_sessions_created_idx          ON replay_sessions (created_at DESC);
CREATE INDEX IF NOT EXISTS replay_sessions_user_created_idx     ON replay_sessions (user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS replay_sessions_status_created_idx   ON replay_sessions (status, created_at DESC);
CREATE INDEX IF NOT EXISTS replay_sessions_date_range_idx       ON replay_sessions (date_range_from, date_range_to);
CREATE INDEX IF NOT EXISTS replay_sessions_retry_parent_idx     ON replay_sessions (retry_parent_id);
```

### Поля

| Поле                     | Тип            | Назначение                                                                                |
| ------------------------ | -------------- | ----------------------------------------------------------------------------------------- |
| session_id               | UUID           | PK                                                                                        |
| user_id                  | TEXT           | Владелец (нужен для RBAC ownership)                                                       |
| name                     | VARCHAR(255)   | Человекочитаемое имя                                                                      |
| strategy                 | JSONB          | Массив FlowOrder параметров                                                               |
| date_range_from/to       | TIMESTAMPTZ    | Период replay                                                                             |
| solver_config_id         | TEXT           | FK на оригинальный solver_config                                                          |
| risk_limits_id           | TEXT           | FK на risk_limits                                                                         |
| fee_model                | JSONB          | Maker/taker rates + extensions                                                            |
| session_config_snapshot  | JSONB          | Замороженный snapshot всех конфигов для детерминизма                                      |
| status                   | TEXT (enum)    | `pending`/`running`/`completed`/`failed`/`cancelled`                                       |
| total_batches            | INTEGER        | Вычисляется на старте running                                                             |
| progress_batches         | INTEGER        | Текущий прогресс                                                                          |
| created_at/started_at/completed_at | TIMESTAMPTZ |                                                                                  |
| error_details            | TEXT           | Заполнено при `failed`                                                                    |
| retry_parent_id          | UUID FK self   | Цепочка retry (migration 003)                                                             |

### Retention

Бесконечная по умолчанию (audit / forensic value). Архивирование вне базы
по политике operator (вне scope MVP).

### Доступ

- Read/Write: `cpp/backtest/src/infra/postgres/postgres_replay_session_repository.cpp`.
- Read only: read_replay_sessions_uc, compare_replay_sessions_uc, audit endpoints.

## Таблица replay_compare_cache

Кэш ответов GET /api/v1/replay/compare.

### DDL

```sql
CREATE TABLE IF NOT EXISTS replay_compare_cache (
  compare_key      TEXT PRIMARY KEY,
  session_a_id     UUID NOT NULL REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  session_b_id     UUID NOT NULL REFERENCES replay_sessions(session_id) ON DELETE CASCADE,
  comparison_json  JSONB NOT NULL,
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  expires_at       TIMESTAMPTZ,
  CHECK (session_a_id <> session_b_id)
);

CREATE INDEX IF NOT EXISTS replay_compare_cache_session_pair_idx
  ON replay_compare_cache (session_a_id, session_b_id);
```

`compare_key` = `min(A,B):max(A,B)` для идемпотентности порядка.

## Таблица replay_audit_runs

Один single-batch audit-mode replay + diff vs production.

### DDL

```sql
CREATE TABLE IF NOT EXISTS replay_audit_runs (
  audit_run_id           UUID PRIMARY KEY,
  session_id             UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL,
  requested_by           TEXT NOT NULL,
  batch_id               TEXT NOT NULL,
  status                 TEXT NOT NULL
                         CHECK (status IN ('pending','running','completed','failed','cancelled')),
  equivalent             BOOLEAN,
  tolerance_json         JSONB,
  replay_result_json     JSONB,
  production_result_json JSONB,
  diff_json              JSONB,
  error_details          TEXT,
  created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
  completed_at           TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS replay_audit_runs_session_id_idx  ON replay_audit_runs (session_id);
CREATE INDEX IF NOT EXISTS replay_audit_runs_created_at_idx  ON replay_audit_runs (created_at DESC);
```

## Conflict Notes (DDL vs IN-006 source)

| ID          | Тема                                                                                                       | Резолюция                                                                                                                              |
| ----------- | ---------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| CN-IN006-02 | IN-006 описывает `replay_sessions` с полями без подчёркиваний (`sessionid`, `daterangefrom`, `errordetails`). Код использует snake_case + добавляет `session_config_snapshot` и `retry_parent_id`. | Источник истины: код (deployed). Документация фиксирует snake_case. |
| CN-IN006-08 | `replay_compare_cache` и `replay_audit_runs` отсутствуют в IN-006.                                          | Добавлены имплементацией для cache и audit-mode. Без конфликта, расширение.                                                            |

## Used In Features

- [F-15. Backtest / Replay](../02-system/features/F-15-backtest-replay/)

## Source Fragments

- IN-006 § ReplaySession (PostgreSQL DDL)
- Migrations 001, 003
