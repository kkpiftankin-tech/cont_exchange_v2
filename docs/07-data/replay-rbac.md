---
id: DOC-DATA-REPLAY-RBAC
phase: 07-data
status: implemented
owner: core-team
related:
  - docs/02-system/features/F-15-backtest-replay/
  - cpp/backtest/migrations/postgres/002_f15_rbac_schema.sql
---

# PostgreSQL: RBAC schema (F-15)

> **Источник истины DDL:** [cpp/backtest/migrations/postgres/002_f15_rbac_schema.sql](../../cpp/backtest/migrations/postgres/002_f15_rbac_schema.sql)

Внимание: RBAC модель не была частью исходного IN-006 (см. Conflict Note
CN-IN006-07 в [REST API](../06-api/rest/replay.md)). Добавлена
имплементацией для compliance и audit.

## Таблица roles

```sql
CREATE TABLE roles (
  role_id     UUID PRIMARY KEY,
  role_name   VARCHAR(255) NOT NULL UNIQUE,    -- ^[a-z_][a-z0-9_]*$
  description TEXT,
  is_system   BOOLEAN NOT NULL DEFAULT FALSE,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  created_by  TEXT NOT NULL
);
```

Системные роли (predefined):

| role_id (UUID)                            | role_name | Description                                                                  |
| ----------------------------------------- | --------- | ---------------------------------------------------------------------------- |
| `11111111-1111-1111-1111-111111111111`    | `admin`   | Full access to all replay operations and audit logs                          |
| `22222222-2222-2222-2222-222222222222`    | `analyst` | Can create and execute replay sessions, read own sessions                    |
| `33333333-3333-3333-3333-333333333333`    | `viewer`  | Read-only access to own replay sessions                                      |

## Таблица permissions

```sql
CREATE TABLE permissions (
  permission_id   UUID PRIMARY KEY,
  permission_name VARCHAR(255) NOT NULL UNIQUE,
  resource_type   VARCHAR(64) NOT NULL,
  action          VARCHAR(64) NOT NULL,
  description     TEXT,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(resource_type, action)
);
```

Предопределённые permissions:

| permission_name    | resource_type     | action    |
| ------------------ | ----------------- | --------- |
| `replay:create`    | `replay_session`  | `create`  |
| `replay:read`      | `replay_session`  | `read`    |
| `replay:execute`   | `replay_session`  | `execute` |
| `replay:cancel`    | `replay_session`  | `cancel`  |
| `audit:read`       | `audit_log`       | `read`    |

## Таблица role_permissions

M:N связь.

```sql
CREATE TABLE role_permissions (
  role_permission_id UUID PRIMARY KEY,
  role_id            UUID NOT NULL REFERENCES roles(role_id) ON DELETE CASCADE,
  permission_id      UUID NOT NULL REFERENCES permissions(permission_id) ON DELETE CASCADE,
  granted_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
  granted_by         TEXT NOT NULL,
  UNIQUE(role_id, permission_id)
);
```

Распределение по умолчанию:

- `admin` → все 5 permissions.
- `analyst` → `replay:create`, `replay:read`, `replay:execute`, `replay:cancel`.
- `viewer` → `replay:read` (только свои сессии).

## Таблица users_roles

```sql
CREATE TABLE users_roles (
  user_role_id UUID PRIMARY KEY,
  user_id      TEXT NOT NULL,
  role_id      UUID NOT NULL REFERENCES roles(role_id) ON DELETE CASCADE,
  assigned_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  assigned_by  TEXT NOT NULL,
  expires_at   TIMESTAMPTZ,
  UNIQUE(user_id, role_id)
);
```

Поддерживает временные назначения через `expires_at`.

## Таблица audit_log

```sql
CREATE TABLE audit_log (
  audit_id        UUID PRIMARY KEY,
  session_id      UUID REFERENCES replay_sessions(session_id) ON DELETE SET NULL,
  user_id         TEXT NOT NULL,
  actor_role_ids  UUID[] NOT NULL,
  resource_type   VARCHAR(64) NOT NULL,
  resource_id     TEXT NOT NULL,
  action          VARCHAR(64) NOT NULL,
  status          VARCHAR(32) NOT NULL CHECK (status IN ('success','failure','denied')),
  old_values      JSONB,
  new_values      JSONB,
  ip_address      INET,
  user_agent      TEXT,
  correlation_id  TEXT,
  error_details   TEXT,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
  CHECK (resource_id != '')
);
```

Индексы:

- `(user_id, created_at DESC)` — for-user-history
- `(resource_type, resource_id)` — drill-down
- `status` — filter denied
- `created_at DESC` — recent-first
- `session_id` — per-session drill-down
- `correlation_id` — distributed trace lookup

## Retention

`audit_log` — бессрочно (compliance). Архивирование в S3/cold storage
через политику operator (вне scope MVP).

## Доступ

- Read/Write: [cpp/backtest/src/infra/postgres/postgres_rbac_repository.cpp](../../cpp/backtest/src/infra/postgres/postgres_rbac_repository.cpp).
- Вход в UC через [rbac_engine.hpp](../../cpp/backtest/src/app/rbac_engine.hpp).

## Used In Features

- [F-15. Backtest / Replay](../02-system/features/F-15-backtest-replay/) (F15-RBAC-1..3)

## Source Fragments

- Migration 002 (источник истины DDL)
- За пределами IN-006 (Conflict Note CN-IN006-07)
